#include <regex>
#include <trieste/trieste.h>
#include <variant>
#include <fstream>
#include <sstream>

#include "debug.hh"
#include "interpreter.hh"
#include "memory_model.hh"
#include "overloaded.hh"

namespace gitmem {

using namespace trieste;

/* Interpreter for a gitmem program. Threads can read and write local
 * variables as well as versioned global variables. Globals are not stored
 * in a single memory location but instead in the state of 'synchronising
 * objects' which include threads and locks. Synchronising actions between
 * threads, and between threads and locks, synchronise the versioned memory
 * and if both objects see updates to the same versioned global variable
 * then a data race is detected. These synchronising actions include:
 * - thread t1 joining a thread t2, which waits for t2 to complete before
 *   trying to 'pull' the new data into t1
 * - t locking a lock l, which waits for the lock l to be available before
 *   trying to 'pull' the new data into t
 * - t unlocking a lock l, which updates l to have t's versioned memory
 */

// Map AST node types to sync operations
static std::optional<SyncOperation> get_sync_operation(Node stmt) {
  auto s = stmt / lang::Stmt;
  if (s == lang::Join) return SyncOperation::Join;
  if (s == lang::Lock) return SyncOperation::Lock;
  if (s == lang::Unlock) return SyncOperation::Unlock;

  // Spawn is an expression, not a statement, but we check for assignment of spawn
  if (s == lang::Assign) {
    // A little gross but okay for now
    auto rhs = s / lang::Expr / lang::Expr;
    if (rhs == lang::Spawn) return SyncOperation::Spawn;
  }

  return std::nullopt;
}

// Check if a statement is a scheduling point according to the protocol
static bool is_syncing(const MemoryModel& model, Node stmt) {
  if (auto op = get_sync_operation(stmt)) {
    return model.is_scheduling_point(*op);
  }
  return false;
}

static bool is_syncing(const MemoryModel& model, Thread &thread) {
  // Can only be true if a thread hasn't terminated
  // Either it has executed all statements but not yet terminated (and may sync)
  // Or it is at a synchronisation node
  // The lazy eval here is important
  return !thread.terminated &&
    ((thread.pc >= thread.block->size()) || is_syncing(model, thread.block->at(thread.pc)));
}

size_t Interpreter::thread_count() const {
  return gctx.threads.size();
}

bool Interpreter::has_thread(ThreadID tid) const {
  return tid < gctx.threads.size();
}

bool Interpreter::thread_terminated(ThreadID tid) const {
  return gctx.threads.at(tid).terminated.has_value();
}

bool Interpreter::all_threads_completed() const {
  return std::all_of(
    gctx.threads.begin(), gctx.threads.end(), [](const auto& thread) {
      return thread.terminated &&
             std::holds_alternative<termination::Completed>(*thread.terminated);
    });
}

bool Interpreter::any_thread_crashed() const {
  return std::any_of(
    gctx.threads.begin(), gctx.threads.end(), [](const auto& thread) {
      return thread.terminated &&
             !std::holds_alternative<termination::Completed>(*thread.terminated);
    });
}

std::optional<TerminationStatus> Interpreter::thread_termination(ThreadID tid) const {
  return gctx.threads.at(tid).terminated;
}

std::optional<std::string> Interpreter::pending_statement(ThreadID tid) const {
  const auto& thread = gctx.threads.at(tid);
  if (thread.pc >= thread.block->size()) {
    return std::nullopt;
  }

  return std::string(thread.block->at(thread.pc)->location().view());
}

bool Interpreter::same_state_as(const GlobalContext& other) const {
  return gctx == other;
}

GlobalContext Interpreter::take_context() {
  return std::move(gctx);
}

/* Evaluating an expression either returns the result of the expression or
 * a the exceptional termination status of the thread.
 */
std::variant<size_t, TerminationStatus>
Interpreter::evaluate_expression(trieste::Node expr, Thread& thread) {
  ThreadContext& ctx = thread.ctx;

  auto e = expr / lang::Expr;
  if (e == lang::Reg) {
    // It is invalid to read a previously unwritten value
    auto var = std::string(expr->location().view());
    if (ctx.locals.contains(var)) {
      return ctx.locals[var];
    } else {
      return termination::UnassignedRead(var);
    }
  } else if (e == lang::Var) {
    auto var = std::string(expr->location().view());

    auto result = gctx.model->read(ctx, var);

    return std::visit(overloaded{
      [&](std::monostate) -> std::variant<size_t, TerminationStatus> {
          // invalid: reading a variable that hasn't been written
          return termination::UnassignedRead(var);
      },
      [&](ValueWithSource value_with_source) -> std::variant<size_t, TerminationStatus> {
          // normal read
          thread.trace.on_read(var, value_with_source);
          return value_with_source.value;
      },
      [&](std::shared_ptr<ConflictBase>& conflict) -> std::variant<size_t, TerminationStatus> {
          verbose::out << (*conflict) << std::endl;
          thread.trace.on_read(var, conflict);
          return termination::DataRace(conflict);
      }
    }, result);
  } else if (e == lang::Const) {
    return size_t(std::stoi(std::string(e->location().view())));
  } else if (e == lang::Add) {
    size_t sum = 0;
    for (auto &child : *e) {
      auto result = evaluate_expression(child, thread);
      if (std::holds_alternative<TerminationStatus>(result))
        return result;
      sum += std::get<size_t>(result);
    }
    return sum;
  } else if (e == lang::Spawn) {
    ThreadID child_tid = gctx.threads.size();
    ThreadContext child_ctx(child_tid, gctx.model);

    if (std::optional<std::shared_ptr<ConflictBase>> conflict =
            gctx.model->on_spawn(ctx, child_ctx)) {
      throw std::logic_error("This code path should never be reached");
    }

    gctx.threads.emplace_back(child_tid, std::move(child_ctx), e / lang::Block);
    thread.trace.on_spawn(child_tid);
    return child_tid;
  } else if (e == lang::Eq || e == lang::Neq) {
    auto lhs = e / lang::Lhs;
    auto rhs = e / lang::Rhs;

    auto lhsEval = evaluate_expression(lhs, thread);
    if (std::holds_alternative<TerminationStatus>(lhsEval))
      return lhsEval;

    auto rhsEval = evaluate_expression(rhs, thread);
    if (std::holds_alternative<TerminationStatus>(rhsEval))
      return rhsEval;

    return e == lang::Eq
               ? (std::get<size_t>(lhsEval)) == (std::get<size_t>(rhsEval))
               : (std::get<size_t>(lhsEval)) != (std::get<size_t>(rhsEval));
  } else {
    throw std::runtime_error("Unknown expression: " +
                             std::string(expr->type().str()));
  }
}

/* Evaluating a statement either returns the resulting change of the program
 * counter (0 if waiting for some other thread) or the exceptional
 * termination status of the thread.
 */
std::variant<int, TerminationStatus> Interpreter::run_statement(Node stmt, Thread& thread) {
  ThreadContext& ctx = thread.ctx;

  auto s = stmt / lang::Stmt;
  if (s == lang::Nop) {

    verbose::out << "Nop" << std::endl;

  } else if (s == lang::Jump) {

    auto cnst = s / lang::Const;
    auto delta = std::stoi(std::string(cnst->location().view()));
    assert(delta > 0);
    return delta;

  } else if (s == lang::Cond) {

    auto expr = s / lang::Expr;
    auto cnst = s / lang::Const;
    auto result = evaluate_expression(expr, thread);

    if (auto b = std::get_if<size_t>(&result)) {
      auto delta = std::stoi(std::string(cnst->location().view()));
      assert(delta > 0);
      return *b ? 1 : delta;
    } else {
      return std::get<TerminationStatus>(result);
    }

  } else if (s == lang::Assign) {

    auto lhs = s / lang::LVal;
    auto var = std::string(lhs->location().view());
    auto rhs = s / lang::Expr;
    auto val_or_term = evaluate_expression(rhs, thread);

    if (size_t *val = std::get_if<size_t>(&val_or_term)) {
      if (lhs == lang::Reg) {

        // Local variables can be re-assigned whenever
        verbose::out << "Set register '" << lhs->location().view() << "' to " << *val
                << std::endl;
        ctx.locals[var] = *val;

      } else if (lhs == lang::Var) {

        auto [line, col] = stmt->location().linecol();
        FileLocation loc{stmt->location().source->origin(), line + 1, col + 1};
        auto write_event = thread.trace.on_write(var, *val, std::move(loc));
        gctx.model->write(ctx, var, ValueWithSource{*val, write_event});
      } else {
        throw std::runtime_error("Bad left-hand side: " +
                                 std::string(lhs->type().str()));
      }
    } else {
      return std::get<TerminationStatus>(val_or_term);
    }
  } else if (s == lang::Join) {
    // A join must waiting for the terminating thread to continue,
    // we don't want to re-evaluate the expression repeatedly as this
    // may be effecting so store the result in the cache.
    auto expr = s / lang::Expr;

    if (!gctx.cache.contains(expr)) {
      auto val_or_term = evaluate_expression(expr, thread);
      if (size_t *val = std::get_if<size_t>(&val_or_term)) {
        gctx.cache[expr] = *val;
      } else {
        return std::get<TerminationStatus>(val_or_term);
      }
    }

    auto result = gctx.cache[expr];
    // Check if the thread ID is valid
    if (result >= gctx.threads.size()) {
        verbose::out << "Join: invalid thread ID " << result
                << ". gctx.threads.size()=" << gctx.threads.size() << std::endl;
        return termination::UnassignedRead(std::to_string(result));
    }

    auto &joinee = gctx.threads[result];
    if (joinee.terminated &&
        std::holds_alternative<termination::Completed>(*joinee.terminated)) {
      if (auto conflict = gctx.model->on_join(ctx, joinee.ctx)) {
        verbose::out << (**conflict) << std::endl;
        thread.trace.on_join(result, *conflict);
        return termination::DataRace(*conflict);
      } else {
        thread.trace.on_join(result);
      }

    } else {
      verbose::out << "Waiting on thread " << result << std::endl;
      return 0;
    }
  } else if (s == lang::Lock) {
    // We can only lock unlocked locks, if a lock hasn't been used
    // before it is implicitly created, we then commit the pending
    // updates of this thread and pull the updates from the lock.
    auto v = s / lang::Var;
    auto var = std::string(v->location().view());

    Lock& lock = gctx.get_lock(var);
    if (lock.owner) {
      verbose::out << "Waiting for lock " << var << " owned by "
              << lock.owner.value() << std::endl;
      return 0;
    }

    lock.owner = thread.tid;

    if (auto conflict = gctx.model->on_lock(ctx, lock)) {
      verbose::out << (**conflict) << std::endl;
      thread.trace.on_lock(var, lock.last_unlock_event, *conflict);
      return termination::DataRace(*conflict);
    }

    thread.trace.on_lock(var, lock.last_unlock_event);
    verbose::out << "Locked " << var << std::endl;

  } else if (s == lang::Unlock) {
    // We can only unlock locks we previously locked. We commit any
    // pending updates and then copy the threads versioned globals
    // to the locks versioned globals (nobody could have changed
    // them since we locked the lock).

    // commit(ctx.globals);
    auto v = s / lang::Var;
    auto var = std::string(v->location().view());

    Lock& lock = gctx.get_lock(var);
    if (!lock.owner || (lock.owner && *lock.owner != thread.tid)) {
      return termination::UnlockError(var);
    }

    if (auto conflict = gctx.model->on_unlock(ctx, lock)) {
      verbose::out << (**conflict) << std::endl;
      thread.trace.on_unlock(var, *conflict);
      return termination::DataRace(*conflict);
    }

    // lock.globals = ctx.globals;
    lock.owner.reset();

    lock.last_unlock_event = thread.trace.on_unlock(var);

    verbose::out << "Unlocked " << var << std::endl;

  } else if (s == lang::Assert) {

    auto expr = s / lang::Expr;
    auto result_or_term = evaluate_expression(expr, thread);
    if (size_t *result = std::get_if<size_t>(&result_or_term)) {
      if (*result) {
        verbose::out << "Assertion passed: " << expr->location().view() << std::endl;
        thread.trace.on_assert_pass(std::string(expr->location().view()));
      } else {
        verbose::out << "Assertion failed: " << expr->location().view() << std::endl;
        thread.trace.on_assert_fail(std::string(expr->location().view()));
        return termination::AssertionFailure(std::string(expr->location().view()));
      }
    } else {
      return std::get<TerminationStatus>(result_or_term);
    }

  } else {
    throw std::runtime_error("Unknown statement: " +
                             std::string(stmt->type().str()));
  }
  return 1;
}

/* Run a particular thread until it reaches a synchronisation point or until
 * it terminates. Report whether the thread was able to progress or not, or
 * whether it terminated.
 */
std::variant<ProgressStatus, TerminationStatus>
Interpreter::run_single_thread_to_sync(Thread& thread) {
  if (thread.terminated)
    return *thread.terminated;

  auto& ctx = thread.ctx;
  auto& pc  = thread.pc;
  Node block = thread.block;

  // Initial sync when thread starts executing
  if (pc == 0) {
    gctx.model->on_start(ctx);
    thread.trace.on_start();
  }

  bool made_progress = false;

  while (pc < block->size()) {
    Node stmt = block->at(pc);

    // Stop *before* executing a sync statement (except first)
    if (made_progress && is_syncing(*gctx.model, stmt))
      return ProgressStatus::progress;

    auto result = run_statement(stmt, thread);

    if (auto term = std::get_if<TerminationStatus>(&result)) {
      thread.terminated = *term;
      return *term;
    }

    int delta = std::get<int>(result);

    // Blocked (e.g. waiting on lock/join)
    if (delta == 0)
      return made_progress ? ProgressStatus::progress
                           : ProgressStatus::no_progress;

    pc += delta;
    made_progress = true;
  }

  // If we ran *any* statements, finishing is a sync point for next iteration
  if (made_progress && gctx.model->is_scheduling_point(SyncOperation::End))
    return ProgressStatus::progress;

  // Otherwise, we truly reached the end this iteration
  if (auto conflict = gctx.model->on_end(ctx)) {
    verbose::out << (**conflict) << std::endl;
    TerminationStatus term = termination::DataRace(*conflict);
    thread.terminated = term;
    return term;
  }

  thread.terminated = termination::Completed();
  thread.trace.on_end();
  return termination::Completed();
}

/**
 * Run a thread to the next sync point, including any threads spawned by that
 * thread
 */
std::variant<ProgressStatus, TerminationStatus>
Interpreter::progress_thread(ThreadID tid) {
  return progress_thread(gctx.threads.at(tid));
}

std::variant<ProgressStatus, TerminationStatus>
Interpreter::progress_thread(Thread& thread) {
  auto no_threads = gctx.threads.size();
  auto prog_or_term = run_single_thread_to_sync(thread);

  bool any_progress =
      std::holds_alternative<ProgressStatus>(prog_or_term) &&
      std::get<ProgressStatus>(prog_or_term) == ProgressStatus::progress;

  for (size_t i = no_threads; i < gctx.threads.size(); ++i) {
    // If there are new threads, we can run them to sync as well
    any_progress = true;
    auto& new_thread = gctx.threads[i];
    if (!is_syncing(*gctx.model, new_thread)) {
      verbose::out << "==== Thread " << i << " (spawn) ====" << std::endl;
      progress_thread(new_thread);
    }
  }

  if (std::holds_alternative<TerminationStatus>(prog_or_term))
    return prog_or_term;

  return any_progress ? ProgressStatus::progress : ProgressStatus::no_progress;
}

/* Try to evaluate all threads until a sync point or termination point
 */
std::variant<ProgressStatus, TerminationStatus>
Interpreter::run_threads_to_sync() {
  verbose::out << "-----------------------" << std::endl;
  bool all_completed = true;
  ProgressStatus any_progress = ProgressStatus::no_progress;
  for (size_t i = 0; i < gctx.threads.size(); ++i) {
    verbose::out << "==== t" << i << " ====" << std::endl;
    auto& thread = gctx.threads[i];
    if (!thread.terminated) {
      auto prog_or_term = run_single_thread_to_sync(thread);
      if (ProgressStatus *prog = std::get_if<ProgressStatus>(&prog_or_term)) {
        any_progress |= *prog;
      } else {
        // We could return termination status of any error here and stop
        // at the first error
        thread.terminated = std::get<TerminationStatus>(prog_or_term);
        any_progress |= ProgressStatus::progress;
      }

      all_completed &= thread.terminated.has_value();
      // if a thread spawns a new thread, it will end up at the end so
      // we will always include the new threads in the termination
      // criteria
    }
  }

  if (all_completed)
    return termination::Completed();

  return any_progress;
}

static bool is_finished(const StepResult<ProgressStatus>& r) {
  // Either, the system is stuck and made no progress in which case there
  // is a deadlock (or a thread is stuck waiting for a crashed thread?)
  // Or, there was some termination criteria in which case we stop
  return is_terminated(r) ||
         std::get<ProgressStatus>(r) == ProgressStatus::no_progress;
}

static std::string user_facing_termination(const TerminationStatus& term) {
  return std::visit(overloaded{
    [](const termination::Completed&) {
      return std::string("Completed successfully");
    },
    [](const termination::DataRace& r) {
      assert(r.conflict != nullptr);
      auto locs = r.conflict->source_locations();
      auto var = r.conflict->object_name();
      std::string prefix = "Data race occurred";
      if (!var.empty()) {
        prefix += " on '" + var + "'";
      }
      return prefix + " at " + locs.first.str() + " and " + locs.second.str();
    },
    [](const auto& t) {
      std::ostringstream oss;
      oss << t;
      return oss.str();
    }
  }, term);
}

/* Try to evaluate all threads until they have all terminated in some way
 * or we have reached a stuck configuration.
 */
int Interpreter::run() {
  std::variant<ProgressStatus, TerminationStatus> prog_or_term;
  do {
    prog_or_term = run_threads_to_sync();
  } while (!is_finished(prog_or_term));

  verbose::out << "----------- execution complete -----------" << std::endl;

  bool exception_detected = false;
  for (size_t i = 0; i < gctx.threads.size(); ++i) {
    auto &thread = gctx.threads[i];

    if (thread.terminated) {
      verbose::out << "Thread " << i << ": ";

      std::visit(
        overloaded{
          [&](const termination::Completed &t) {
            verbose::out << t << std::endl;
          },

          [&](const auto &t) {
            // Any non-completed termination is exceptional
            verbose::out << t << std::endl;
            if (verbose::out.enabled) {
              std::cerr << "Error in thread " << i << ": " << t << std::endl;
            } else {
              std::cerr << "Error in thread " << i << ": "
                        << user_facing_termination(*thread.terminated)
                        << std::endl;
            }
            exception_detected = true;
          }
        },
        *thread.terminated
      );
    } else {
      exception_detected = true;
      thread.trace.on_end();
      verbose::out << "Thread " << i << " is stuck" << std::endl;
      std::cerr << "error: thread " << i << " is stuck (possible deadlock)" << std::endl;
    }
  }

  verbose::out << "------------------------------------------" << std::endl;

  print_thread_traces();

  return exception_detected ? 1 : 0;
}

void Interpreter::print_state(std::ostream& os, bool show_all) const {
  gctx.print(os, show_all);
}

void Interpreter::print_thread_traces() {
  for (size_t tid = 0; tid < gctx.threads.size(); ++tid) {
    const auto& thread = gctx.threads[tid];
    verbose::out << "=== Thread " << tid << " ===" << std::endl;
    verbose::out << thread.trace;
    verbose::out << "====================================\n";
  }
}

void Interpreter::print_revision_graph(const std::filesystem::path& output_path) {
  // Build revision graph - collect const raw pointers
  std::vector<const ThreadSyncState*> thread_state_ptrs;
  for (const auto& thread : gctx.threads) {
    thread_state_ptrs.push_back(thread.ctx.sync.get());
  }

  std::string dot = gctx.model->build_revision_graph_dot(thread_state_ptrs);
  if (!dot.empty()) {
    // Write to file
    auto dot_file = output_path.parent_path() / (output_path.stem().string() + "_revision_graph.dot");
    std::ofstream out(dot_file);
    if (out) {
      out << dot;
      verbose::out << "Revision graph written to " << dot_file << std::endl;
    } else {
      verbose::out << "Failed to write revision graph to " << dot_file << std::endl;
    }
  }
}

graph::ExecutionGraph Interpreter::build_execution_graph_from_traces() {
  // Map each thread ID to its last graph node in the execution graph (per-thread program order)
  std::unordered_map<ThreadID, std::shared_ptr<graph::Node>> thread_tails;

  // Track the last unlock event for each lock (for lock->unlock edges)
  std::unordered_map<std::string, std::shared_ptr<graph::Node>> last_unlock_per_lock;

  // Track join nodes that need fixing up after all threads are processed
  std::vector<std::shared_ptr<graph::Join>> joins_to_fix;

  // Track read nodes that need their source fixed up
  std::vector<std::pair<std::shared_ptr<graph::Read>, std::shared_ptr<Event>>> reads_to_fix;

  // Map from trace events to graph nodes
  std::unordered_map<std::shared_ptr<Event>, std::shared_ptr<graph::Node>> event_to_node;

  // Create Start nodes for all threads
  std::vector<std::shared_ptr<graph::Start>> thread_starts;
  thread_starts.reserve(gctx.threads.size());

  for (ThreadID tid = 0; tid < gctx.threads.size(); ++tid) {
    auto node = std::make_shared<graph::Start>(tid);
    thread_starts.push_back(node);
    thread_tails[tid] = node;
  }

  // The entry point is thread 0's start
  graph::ExecutionGraph g(thread_starts[0]);
  g.threads = std::move(thread_starts);

  // Helper to link a node in program order for its thread
  auto link_in_program_order = [&](ThreadID tid, std::shared_ptr<graph::Node> node) {
    if (thread_tails[tid]) {
      thread_tails[tid]->next = node;
    }
    thread_tails[tid] = node;
  };

  // Process events from all threads
  for (ThreadID tid = 0; tid < gctx.threads.size(); ++tid) {
    auto& thread = gctx.threads[tid];

    // Process all events from the trace
    for (const auto& event : thread.trace) {
      if (!event) {
        // Safety check: skip null events (shouldn't happen)
        continue;
      }

      std::visit(overloaded{
        [&](const StartEvent&) {
          // Skip: Start nodes already created above
        },
        [&](const EndEvent&) {
          auto node = std::make_shared<graph::End>();
          link_in_program_order(tid, node);
          event_to_node[event] = node;
        },
        [&](const WriteEvent& arg) {
          auto node = std::make_shared<graph::Write>(arg.var, arg.value, tid);
          link_in_program_order(tid, node);
          event_to_node[event] = node;
        },
        [&](const ReadEvent& arg) {
          // Link to the write that produced this value
          std::shared_ptr<graph::Read> node;
          std::visit(overloaded{
            [&](const ReadValue& val) {
              // Create the read node, but we might need to fix up the source later
              node = std::make_shared<graph::Read>(arg.var, val.value, tid, nullptr);
              assert(val.source_event && "source missing");
              reads_to_fix.push_back({node, val.source_event});
            },
            [&](const std::shared_ptr<ConflictBase>&) {
              node = std::make_shared<graph::Read>(arg.var, tid, graph::Conflict(arg.var));
            }
          }, arg.value_or_conflict);

          link_in_program_order(tid, node);
          event_to_node[event] = node;
        },
        [&](const SpawnEvent& arg) {
          // Link to the child thread's start node
          auto node = std::make_shared<graph::Spawn>(arg.child_tid, g.threads[arg.child_tid]);
          link_in_program_order(tid, node);
          event_to_node[event] = node;
        },
        [&](const JoinEvent& arg) {
          // Create join node - will fix up joinee pointer later
          std::optional<graph::Conflict> conflict;
          if (arg.maybe_conflict) {
            // Just mark as conflicting - version IDs don't map directly to nodes
            conflict = graph::Conflict("");  // empty var name for joins
          }
          auto node = std::make_shared<graph::Join>(arg.joinee_tid, nullptr, conflict);
          joins_to_fix.push_back(node);
          link_in_program_order(tid, node);
          event_to_node[event] = node;
        },
        [&](const LockEvent& arg) {
          // Link to the last unlock event using the event-to-node mapping
          std::shared_ptr<graph::Node> ordered_after = nullptr;
          if (arg.last_unlock_event && event_to_node.contains(arg.last_unlock_event)) {
            ordered_after = event_to_node[arg.last_unlock_event];
          }

          std::optional<graph::Conflict> conflict;
          if (arg.maybe_conflict) {
            // Mark as conflicting with the lock name
            conflict = graph::Conflict(arg.lock_name);
          }
          auto node = std::make_shared<graph::Lock>(arg.lock_name, ordered_after, conflict);
          link_in_program_order(tid, node);
          event_to_node[event] = node;
        },
        [&](const UnlockEvent& arg) {
          auto node = std::make_shared<graph::Unlock>(arg.lock_name);
          last_unlock_per_lock[arg.lock_name] = node;
          link_in_program_order(tid, node);
          event_to_node[event] = node;

          // Mark conflict if present
          if (arg.maybe_conflict) {
            // TODO: Visualize unlock conflicts
          }
        },
        [&](const AssertEvent& arg) {
          auto node = std::make_shared<graph::Assertion>(arg.condition, arg.pass);
          link_in_program_order(tid, node);
          event_to_node[event] = node;
        }
      }, event->data);
    }

    // Add pending node if thread hasn't terminated
    if (!thread.terminated) {
      if (thread.pc < thread.block->size()) {
        // Thread is stuck waiting at a specific statement
        trieste::Node stmt = thread.block->at(thread.pc);
        auto pending = std::make_shared<graph::Pending>(std::string(stmt->location().view()));
        link_in_program_order(tid, pending);
      } else {
        // Thread has finished all statements but hasn't terminated yet
        auto pending = std::make_shared<graph::Pending>("...");
        link_in_program_order(tid, pending);
      }
    }
  }

  // Fix up join nodes to point to the actual end of the joined threads
  for (auto& join_node : joins_to_fix) {
    ThreadID joinee_tid = join_node->tid;
    // thread_tails[joinee_tid] now points to the end (or pending) of that thread
    const_cast<std::shared_ptr<const graph::Node>&>(join_node->joinee) = thread_tails[joinee_tid];
  }

  // Fix up read nodes to point to their source write events
  for (auto& [read_node, source_event] : reads_to_fix) {
    assert(event_to_node.contains(source_event) && "source missing in event_to_node map");
    read_node->set_source(event_to_node[source_event]);
  }

  return g;
}

void Interpreter::print_execution_graph(const std::filesystem::path& output_path) {
  auto exec_graph = build_execution_graph_from_traces();
  graph::GraphvizPrinter gv(output_path);
  gv.visit(exec_graph.entry.get());
}

int interpret(const Node ast, const std::filesystem::path &output_path,
              const MemoryModelFactory& make_model) {
  Node starting_block = entry_block(ast);
  Interpreter interp(GlobalContext(starting_block, make_model()));
  int result = interp.run();

  interp.print_execution_graph(output_path);
  interp.print_revision_graph(output_path);

  return result;
}

} // namespace gitmem