#include <trieste/trieste.h>
#include <variant>
#include <regex>

#include "interpreter.hh"
#include "mermaid.hh"
#include "graphviz.hh"

namespace gitmem
{
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

    bool is_syncing(Node stmt)
    {
        auto s = stmt / Stmt;
        return s == Join || s == Lock || s == Unlock;
    }

    bool is_syncing(Thread &thread)
    {
        return !thread.terminated && is_syncing(thread.block->at(thread.pc));
    }

    /* At a commit point, walk through all the versioned variables and see if
     * they have a pending commit, if so commit the value by appending to
     * the variables history.
     */
    void commit(Globals &globals) {
        for (auto& [var, global] : globals) {
            if (global.commit)
            {
                global.history.push_back(*global.commit);
                verbose << "Committed global '" << var << "' with id " << *global.commit << std::endl;
                global.commit.reset();
            }
        }
    }


    /* A versioned value can be fastforwarded to another version, if one
     * version's history is a prefix of another version's history.
     * A conflict between two commit histories exists if neither history is a
     * prefix of the other.
     */
    std::optional<std::pair<Commit, Commit>> has_conflict(CommitHistory& h1, CommitHistory& h2)
    {
        size_t length = std::min(h1.size(), h2.size());

        for (size_t i = 0; i < length; i++)
        {
            if (h1[i] != h2[i]) return std::pair<Commit, Commit>{h1[i], h2[i]};
        }

        return std::nullopt;
    }

    struct Conflict
    {
        std::string var;
        std::pair<Commit, Commit> commits;
    };

    /* Walk through all the global versions from source and update the versions
     * in destination to be the most up-to-date version (this could come from
     * either source or destination). This means destination will now also
     * include variables it previously did not know about.
     */
    std::optional<Conflict> pull(Globals &dst, Globals &src) {
        for (auto& [var, global] : src) {
            if (dst.contains(var))
            {
                auto& src_var = src[var];
                auto& dst_var = dst[var];
                if (auto conflict = has_conflict(src_var.history, dst_var.history))
                {
                    auto [s1, s2] = *conflict;
                    verbose << "A data race on '" << var << "' was detected from commits " << s1 << " and " << s2 << std::endl;
                    return Conflict(var, *conflict);
                }
                else if (src_var.history.size() > dst_var.history.size())
                {
                    verbose << "Fast-forward '" << var << "' to id " << src_var.val << std::endl;
                    dst_var.val = src_var.val;
                    dst_var.history = src_var.history;
                }
            }
            else
            {
                dst[var].val = src[var].val;
                dst[var].history = src[var].history;
            }
        }
        return std::nullopt;
    }

    template<typename T, typename...Args>
    std::shared_ptr<T> thread_append_node(ThreadContext& ctx, Args&&...args)
    {
        assert(ctx.tail);
        auto node = std::make_shared<T>(std::forward<Args>(args)...);
        ctx.tail->next = node;
        ctx.tail = node;
        return node;
    }

    template<>
    std::shared_ptr<graph::Pending> thread_append_node<graph::Pending>(ThreadContext& ctx, std::string&& stmt)
    {
        // pending nodes don't update the tail position as we will destroy them
        // once we execute the node
        auto s = std::regex_replace(stmt, std::regex("\n"), "\\l   ");
        auto node = make_shared<graph::Pending>(std::move(s));
        ctx.tail->next = node;
        return node;
    }

    /* Evaluating an expression either returns the result of the expression or
     * a the exceptional termination status of the thread.
     */
    std::variant<size_t, TerminationStatus> evaluate_expression(Node expr, GlobalContext &gctx, ThreadContext &ctx)
    {
        auto e = expr / Expr;
        if (e == Reg)
        {
            // It is invalid to read a previously unwritten value
            auto var = std::string(expr->location().view());
            if (ctx.locals.contains(var))
            {
                return ctx.locals[var];
            }
            else
            {
                return TerminationStatus::unassigned_variable_read_exception;
            }
        }
        else if (e == Var)
        {
            // It is invalid to read a previously unwritten value
            auto var = std::string(expr->location().view());
            if (ctx.globals.contains(var))
            {
                auto& global = ctx.globals[var];
                auto commit = global.commit.value_or(global.history.back());
                auto source_node = gctx.commit_map[commit];
                thread_append_node<graph::Read>(ctx, var, global.val, commit, source_node);
                return global.val;
            }
            else
            {
                return TerminationStatus::unassigned_variable_read_exception;
            }
        }
        else if (e == Const)
        {
            return size_t(std::stoi(std::string(e->location().view())));
        }
        else if (e == Spawn)
        {
            // Spawning is a sync point, commit local pending commits, and
            // copy the global state to the spawned thread
            commit(ctx.globals);
            ThreadID tid = gctx.threads.size();
            auto node = std::make_shared<graph::Start>(tid);

            ThreadContext new_ctx = { Locals(), ctx.globals, node };
            gctx.threads.push_back(std::make_shared<Thread>(new_ctx, e / Block));

            thread_append_node<graph::Spawn>(ctx, tid, node);

            return tid;
        }
        else if (e == Eq)
        {
            auto lhs = e / Lhs;
            auto rhs = e / Rhs;

            auto lhsEval = evaluate_expression(lhs, gctx, ctx);
            if (std::holds_alternative<TerminationStatus>(lhsEval)) return lhsEval;

            auto rhsEval = evaluate_expression(rhs, gctx, ctx);
            if (std::holds_alternative<TerminationStatus>(rhsEval)) return rhsEval;

            return ((std::get<size_t>(lhsEval)) == (std::get<size_t>(rhsEval)));
        }
        else
        {
            throw std::runtime_error("Unknown expression: " + std::string(expr->type().str()));
        }
    }

    /* Evaluating a statement either returns whether the thread could progress
     * (progress was made or it is waiting for some other thread) or
     * the exceptional termination status of the thread.
     */
    std::variant<ProgressStatus, TerminationStatus> run_statement(Node stmt, GlobalContext &gctx, ThreadContext &ctx, const ThreadID& tid)
    {
        auto s = stmt / Stmt;
        if (s == Nop)
        {
            verbose << "Nop" << std::endl;
        }
        else if (s == Assign)
        {
            auto lhs = s / LVal;
            auto var = std::string(lhs->location().view());
            auto rhs = s / Expr;
            auto val_or_term = evaluate_expression(rhs, gctx, ctx);
            if(size_t* val = std::get_if<size_t>(&val_or_term))
            {
                if (lhs == Reg)
                {
                    // Local variables can be re-assigned whenever
                    verbose << "Set register '" << lhs->location().view() << "' to " << *val << std::endl;
                    ctx.locals[var] = *val;
                }
                else if (lhs == Var)
                {
                    // Global variable writes need to create a new commit id
                    // to track the history of updates
                    auto &global = ctx.globals[var];
                    global.val = *val;
                    global.commit = gctx.uuid++;
                    verbose <<  "Set global '" << lhs->location().view() << "' to " << *val <<  " with id " << *(global.commit) << std::endl;

                    auto node = thread_append_node<graph::Write>(ctx, var, global.val, *global.commit);
                    gctx.commit_map[*(global.commit)] = node;
                }
                else
                {
                    throw std::runtime_error("Bad left-hand side: " + std::string(lhs->type().str()));
                }
            }
            else
            {
                return std::get<TerminationStatus>(val_or_term);
            }
        }
        else if (s == Join)
        {
            // A join must waiting for the terminating thread to continue,
            // we don't want to re-evaluate the expression repeatedly as this
            // may be effecting so store the result in the cache.
            auto expr = s / Expr;

            if (!gctx.cache.contains(expr))
            {
                auto val_or_term = evaluate_expression(expr, gctx, ctx);
                if (size_t* val = std::get_if<size_t>(&val_or_term))
                {
                    gctx.cache[expr] = *val;
                }
                else
                {
                    return std::get<TerminationStatus>(val_or_term);
                }
            }

            // when joining, we commit the updates of both threads (the joined
            // thread will not necessarily have commited them), we then
            // pull the updates into the joining thread.
            auto result = gctx.cache[expr];
            auto& thread = gctx.threads[result];
            if (thread->terminated && (*thread->terminated == TerminationStatus::completed))
            {
                commit(ctx.globals);
                commit(thread->ctx.globals);
                verbose << "Pulling from thread " <<  result << std::endl;
                if(auto conflict = pull(ctx.globals, thread->ctx.globals))
                {
                    using graph::Node;
                    auto [s1, s2] = conflict->commits;
                    auto sources = std::pair<std::shared_ptr<Node>, std::shared_ptr<Node>>{gctx.commit_map[s1], gctx.commit_map[s2]};
                    auto graph_conflict = graph::Conflict(conflict->var, sources);
                    thread_append_node<graph::Join>(ctx, result, thread->ctx.tail, graph_conflict);
                    return TerminationStatus::datarace_exception;
                }

                thread_append_node<graph::Join>(ctx, result, thread->ctx.tail);
            }
            else
            {
                verbose << "Waiting on thread " << result << std::endl;
                return ProgressStatus::no_progress;
            }
        }
        else if (s == Lock)
        {
            // We can only lock unlocked locks, if a lock hasn't been used
            // before it is implicitly created, we then commit the pending
            // updates of this thread and pull the updates from the lock.
            auto v = s / Var;
            auto var = std::string(v->location().view());

            auto& lock = gctx.locks[var];
            if (lock.owner) {
                verbose << "Waiting for lock " << var << " owned by " << lock.owner.value() << std::endl;
                return ProgressStatus::no_progress;
            }

            lock.owner = tid;
            commit(ctx.globals);
            if(auto conflict = pull(ctx.globals, lock.globals))
            {
                using graph::Node;
                auto [s1, s2] = conflict->commits;
                auto sources = std::pair<std::shared_ptr<Node>, std::shared_ptr<Node>>{gctx.commit_map[s1], gctx.commit_map[s2]};
                auto graph_conflict = graph::Conflict(conflict->var, sources);
                thread_append_node<graph::Lock>(ctx, var, lock.last, graph_conflict);
                return TerminationStatus::datarace_exception;
            }

            thread_append_node<graph::Lock>(ctx, var, lock.last);

            verbose << "Locked " << var << std::endl;

        }
        else if (s == Unlock)
        {
            // We can only unlock locks we previously locked. We commit any
            // pending updates and then copy the threads versioned globals
            // to the locks versioned globals (nobody could have changed
            // them since we locked the lock).
            commit(ctx.globals);
            auto v = s / Var;
            auto var = std::string(v->location().view());

            auto& lock = gctx.locks[var];
            if (!lock.owner || (lock.owner && *lock.owner != tid))
            {
                return TerminationStatus::unlock_exception;
            }

            lock.globals = ctx.globals;
            lock.owner.reset();

            thread_append_node<graph::Unlock>(ctx, var);
            lock.last = ctx.tail;

            verbose << "Unlocked " << var << std::endl;
        }
        else if (s == Assert)
        {
            auto expr = s / Expr;
            auto result_or_term = evaluate_expression(expr, gctx, ctx);
            if (size_t* result = std::get_if<size_t>(&result_or_term))
            {
                if (*result)
                {
                    verbose << "Assertion passed: " << expr->location().view() << std::endl;
                }
                else
                {
                    verbose << "Assertion failed: " << expr->location().view() << std::endl;
                    return TerminationStatus::assertion_failure_exception;
                }
            }
            else
            {
                return std::get<TerminationStatus>(result_or_term);
            }
        }
        else
        {
            throw std::runtime_error("Unknown statement: " + std::string(stmt->type().str()));
        }
        return ProgressStatus::progress;
    }

    /* Run a particular thread until it reaches a synchronisation point or until
     * it terminates. Report whether the thread was able to progress or not, or
     * whether it terminated.
     */
    std::variant<ProgressStatus, TerminationStatus> run_single_thread_to_sync(GlobalContext& gctx, const ThreadID tid, std::shared_ptr<Thread> thread)
    {
        if (thread->terminated) {
            return *(thread->terminated);
        }
        Node block = thread->block;
        size_t &pc = thread->pc;
        ThreadContext &ctx = thread->ctx;

        bool first_statement = true;
        while(pc < block->size())
        {
            Node stmt = block->at(pc);

            if (!first_statement && is_syncing(stmt))
            {
                thread_append_node<graph::Pending>(ctx, std::string(stmt->location().view()));
                return ProgressStatus::progress;
            }

            auto prog_or_term = run_statement(stmt, gctx, ctx, tid);
            if (std::holds_alternative<TerminationStatus>(prog_or_term))
            {
                thread->terminated = std::get<TerminationStatus>(prog_or_term);
                thread_append_node<graph::End>(ctx);
                return prog_or_term;
            }

            if(!(std::get<ProgressStatus>(prog_or_term)))
            {
                thread_append_node<graph::Pending>(ctx, std::string(stmt->location().view()));
                return first_statement ? ProgressStatus::no_progress : ProgressStatus::progress;
            }

            pc++;
            first_statement = false;
        }

        thread->terminated = TerminationStatus::completed;
        thread_append_node<graph::End>(ctx);
        return TerminationStatus::completed;
    }

    /**
     * Run a thread to the next sync point, including any threads spawned by that thread
     */
    std::variant<ProgressStatus, TerminationStatus>
    progress_thread(GlobalContext &gctx, const ThreadID tid, std::shared_ptr<Thread> thread)
    {
        auto no_threads = gctx.threads.size();
        auto prog_or_term = run_single_thread_to_sync(gctx, tid, thread);

        bool any_progress = std::holds_alternative<ProgressStatus>(prog_or_term) &&
                            std::get<ProgressStatus>(prog_or_term) == ProgressStatus::progress;
        for (size_t i = no_threads; i < gctx.threads.size(); ++i)
        {
            // If there are new threads, we can run them to sync as well
            any_progress = true;
            auto new_thread = gctx.threads[i];
            if (!is_syncing(*new_thread))
            {
                verbose << "==== Thread " << i << " (spawn) ====" << std::endl;
                progress_thread(gctx, i, new_thread);
            }
        }

        if (std::holds_alternative<TerminationStatus>(prog_or_term))
            return prog_or_term;

        return any_progress ? ProgressStatus::progress : ProgressStatus::no_progress;
    }

    /* Try to evaluate all threads until a sync point or termination point
     */
    std::variant<ProgressStatus, TerminationStatus> run_threads_to_sync(GlobalContext& gctx)
    {
        verbose << "-----------------------" << std::endl;
        bool all_completed = true;
        ProgressStatus any_progress = ProgressStatus::no_progress;
        for (size_t i = 0; i < gctx.threads.size(); ++i)
        {
            verbose << "==== t" << i << " ====" << std::endl;
            auto thread = gctx.threads[i];
            if (!thread->terminated)
            {
                auto prog_or_term = run_single_thread_to_sync(gctx, i, thread);
                if (ProgressStatus* prog = std::get_if<ProgressStatus>(&prog_or_term))
                {
                    any_progress |= *prog;
                }
                else
                {
                    // We could return termination status of any error here and stop
                    // at the first error
                    thread->terminated = std::get<TerminationStatus>(prog_or_term);
                    any_progress |= ProgressStatus::progress;
                }

                all_completed &= thread->terminated.has_value();
                // if a thread spawns a new thread, it will end up at the end so
                // we will always include the new threads in the termination
                // criteria
            }
        }

        if (all_completed) return TerminationStatus::completed;

        return any_progress;
    }

    bool is_finished(std::variant<ProgressStatus, TerminationStatus>& prog_or_term)
    {
        // Either, the system is stuck and made no progress in which case there
        // is a deadlock (or a thread is stuck waiting for a crashed thread?)
        if (ProgressStatus* prog = std::get_if<ProgressStatus>(&prog_or_term))
            return (*prog) == ProgressStatus::no_progress;

        // Or, there was some termination criteria in which case we stop
        return true;
    }

    /* Try to evaluate all threads until they have all terminated in some way
     * or we have reached a stuck configuration.
     */
    int run_threads(GlobalContext &gctx)
    {
        std::variant<ProgressStatus, TerminationStatus> prog_or_term;
        do {
            prog_or_term = run_threads_to_sync(gctx);
        } while (!is_finished(prog_or_term));

        verbose << "----------- execution complete -----------" << std::endl;

        bool exception_detected = false;
        for (size_t i = 0; i < gctx.threads.size(); ++i)
        {
            const auto& thread = gctx.threads[i];
            if (thread->terminated)
            {
                switch (thread->terminated.value())
                {
                case TerminationStatus::completed:
                    verbose << "Thread " << i << " terminated normally" << std::endl;
                    break;

                case TerminationStatus::unlock_exception:
                    verbose << "Thread " << i << " unlocked a lock it does not own" << std::endl;
                    exception_detected = true;
                    break;

                case TerminationStatus::datarace_exception:
                    verbose << "Thread " << i << " encountered a data-race" << std::endl;
                    exception_detected = true;
                    break;

                case TerminationStatus::assertion_failure_exception:
                    verbose << "Thread " << i << " failed an assertion" << std::endl;
                    exception_detected = true;
                    break;

                case TerminationStatus::unassigned_variable_read_exception:
                    verbose << "Thread " << i << " read an uninitialised value" << std::endl;
                    exception_detected = true;
                    break;

                default:
                    verbose << "Thread " << i << " has an unhandled termination state" << std::endl;
                    break;
                }
            }
            else
            {
                exception_detected = true;
                thread_append_node<graph::End>(thread->ctx);
                verbose << "Thread " << i << " is stuck" << std::endl;
            }
        }

        return exception_detected ? 1 : 0;
    }

    int interpret(const Node ast, const std::filesystem::path &output_path)
    {
        GlobalContext gctx(ast);
        auto result = run_threads(gctx);
        gctx.print_execution_graph(output_path);

        return result;
    }
}
