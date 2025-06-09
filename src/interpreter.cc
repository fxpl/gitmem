#include <trieste/trieste.h>
#include <random>
#include <variant>
#include <regex>
#include "lang.hh"

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


    /* A 'Global' is a structure to capture the current synchronising objects
     * representation of a global variable. The structure is the current value,
     * the current commit id for the variable, and the history of commited ids.
     */

    using Commit = size_t;
    using CommitHistory = std::vector<Commit>;

    struct Global {
        size_t val;
        std::optional<Commit> commit;
        CommitHistory history;
    };

    using Globals = std::unordered_map<std::string, Global>;

    enum class TerminationStatus {
        completed,
        datarace_exception,
        unlock_exception,
        assertion_failure_exception,
        unassigned_variable_read_exception,
    };

    using Locals = std::unordered_map<std::string, size_t>;

    struct ThreadContext
    {
        Locals locals;
        Globals globals;
    };

    using ThreadStatus = std::optional<TerminationStatus>;

    struct Thread
    {
        ThreadContext ctx;
        Node block;
        size_t pc = 0;
        ThreadStatus terminated = std::nullopt;

        bool operator==(const Thread& other) const {
            // Globals have a history that we don't care about, so we only
            // compare values
            if (ctx.globals.size() != other.ctx.globals.size())
                return false;
            for (const auto& [var, global] : ctx.globals) {
                if (!other.ctx.globals.contains(var) ||
                    ctx.globals.at(var).val != other.ctx.globals.at(var).val)
                {
                    return false;
                }
            }
            return ctx.locals == other.ctx.locals &&
                   block == other.block &&
                   pc == other.pc &&
                   terminated == other.terminated;
        }
    };

    using ThreadID = size_t;

    struct Lock {
        Globals globals;
        std::optional<ThreadID> owner = std::nullopt;
    };

    using Threads = std::vector<std::shared_ptr<Thread>>;

    using Locks = std::unordered_map<std::string, struct Lock>;

    struct GlobalContext {
        Threads threads;
        Locks locks;
        NodeMap<size_t> cache;
        Commit uuid = 0;

        bool operator==(const GlobalContext& other) const {
            if (threads.size() != other.threads.size() || locks.size() != other.locks.size())
                return false;

            // Threads may have been spawned in a different order, so we
            // find the thread with the same block in the other context
            for (auto& thread : threads) {
                auto it = std::find_if(other.threads.begin(), other.threads.end(),
                                       [&thread](auto& t) { return t->block == thread->block; });
                if (it == other.threads.end() || !(*thread == **it))
                    return false;
            }

            for (auto& [name, lock] : locks) {
                if (!other.locks.contains(name))
                    return false;
                auto& other_lock = other.locks.at(name);
                if (lock.owner != other_lock.owner)
                    return false;
            }
            return true;
        }
    };

    enum class ProgressStatus {
        progress,
        no_progress
    };

    bool operator!(ProgressStatus p) { return p == ProgressStatus::no_progress; }

    ProgressStatus operator||(const ProgressStatus& p1, const ProgressStatus& p2)
    {
        return (p1 == ProgressStatus::progress || p2 == ProgressStatus::progress) ? ProgressStatus::progress : ProgressStatus::no_progress;
    }

    void operator|=(ProgressStatus& p1, const ProgressStatus& p2) {  p1 = (p1 || p2); }

    bool is_syncing(Node stmt)
    {
        auto s = stmt / Stmt;
        return s == Join || s == Lock || s == Unlock;
    }

    /* At a commit point, walk through all the versioned variables and see if
     * they have a pending commit, if so commit the value by appending to
     * the variables history.
     */
    void commit(Globals &globals) {
        for (auto& [var, global] : globals) {
            if (global.commit)
            {
                global.history.push_back(global.commit.value());
                std::cout << "Committed global '" << var << "' with id " << global.commit.value() << std::endl;
                global.commit.reset();
            }
        }
    }


    /* A versioned value can be fastforwarded to another version, if one
     * version's history is a prefix of another version's history.
     * A conflict between two commit histories exists if neither history is a
     * prefix of the other.
     */
    bool has_conflict(CommitHistory& h1, CommitHistory& h2)
    {
        size_t length = std::min(h1.size(), h2.size());

        bool conflict = false;
        for (size_t i = 0; i < length && !conflict; ++i)
        {
            conflict |= (h1[i] != h2[i]);
        }

        return conflict;
    }

    /* Walk through all the global versions from source and update the versions
     * in destination to be the most up-to-date version (this could come from
     * either source or destination). This means destination will now also
     * include variables it previously did not know about.
     */
    bool pull(Globals &dst, Globals &src) {
        for (auto& [var, global] : src) {
            if (dst.contains(var))
            {
                auto& src_var = src[var];
                auto& dst_var = dst[var];
                if (has_conflict(src_var.history, dst_var.history))
                {
                    std::cout << "A data race on '" << var << "' was detected" << std::endl;
                    return false;
                }
                else if (src_var.history.size() > dst_var.history.size())
                {
                    std::cout << "Fast-forward '" << var << "' to id " << src_var.val << std::endl;
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
        return true;
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
                return ctx.globals[var].val;
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
            ThreadContext new_ctx = { Locals(), ctx.globals };
            gctx.threads.push_back(std::make_shared<Thread>(new_ctx, e / Block));
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
            std::cout << "Unknown expression: " << expr->type() << std::endl;
            return size_t(0);
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
            std::cout << "Nop" << std::endl;
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
                    std::cout << "Set register '" << lhs->location().view() << "' to " << *val << std::endl;
                    ctx.locals[var] = *val;
                }
                else if (lhs == Var)
                {
                    // Global variable writes need to create a new commit id
                    // to track the history of updates
                    auto &global = ctx.globals[var];
                    global.val = *val;
                    global.commit = gctx.uuid++;
                    std::cout <<  "Set global '" << lhs->location().view() << "' to " << *val <<  " with id " << *(global.commit) << std::endl;
                }
                else
                {
                    std::cout << "Bad left-hand side: " << lhs->type() << std::endl;
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
                std::cout << "Pulling from thread " <<  result << std::endl;
                if(!pull(ctx.globals, thread->ctx.globals))
                {
                    return TerminationStatus::datarace_exception;
                }
            }
            else
            {
                std::cout << "Waiting on thread " << result << std::endl;
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
                std::cout << "Waiting for lock " << var << " owned by " << lock.owner.value() << std::endl;
                return ProgressStatus::no_progress;
            }

            lock.owner = tid;
            commit(ctx.globals);
            if (!pull(ctx.globals, lock.globals))
            {
                return TerminationStatus::datarace_exception;
            }

            std::cout << "Locked " << var << std::endl;

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
            else
            {
                lock.globals = ctx.globals;
                lock.owner.reset();
                std::cout << "Unlocked " << var << std::endl;
            }
        }
        else if (s == Assert)
        {
            auto expr = s / Expr;
            auto result_or_term = evaluate_expression(expr, gctx, ctx);
            if (size_t* result = std::get_if<size_t>(&result_or_term))
            {
                if (*result)
                {
                    std::cout << "Assertion passed: " << expr->location().view() << std::endl;
                }
                else
                {
                    std::cout << "Assertion failed: " << expr->location().view() << std::endl;
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
            std::cout << "Unknown statement: " << stmt->type() << std::endl;
        }
        return ProgressStatus::progress;
    }

    /* Run a particular thread until it reaches a synchronisation point or until
     * it terminates. Report whether the thread was able to progress or not, or
     * whether it terminated.
     */
    std::variant<ProgressStatus, TerminationStatus> run_thread_to_sync(GlobalContext& gctx, const ThreadID& tid, std::shared_ptr<Thread> thread)
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
                return ProgressStatus::progress;

            auto prog_or_term = run_statement(stmt, gctx, ctx, tid);
            if (std::holds_alternative<TerminationStatus>(prog_or_term))
            {
                thread->terminated = std::get<TerminationStatus>(prog_or_term);
                return prog_or_term;
            }

            if(!(std::get<ProgressStatus>(prog_or_term)))
                return first_statement ? ProgressStatus::no_progress : ProgressStatus::progress;

            pc++;
            first_statement = false;
        }

        thread->terminated = TerminationStatus::completed;
        return TerminationStatus::completed;
    }

    /* Try to evaluate all threads until a sync point or termination point
     */
    std::variant<ProgressStatus, TerminationStatus> run_threads_to_sync(GlobalContext& gctx)
    {
        std::cout << "-----------------------" << std::endl;
        bool all_completed = true;
        ProgressStatus any_progress = ProgressStatus::no_progress;
        for (size_t i = 0; i < gctx.threads.size(); ++i)
        {
            std::cout << "==== t" << i << " ====" << std::endl;
            auto thread = gctx.threads[i];
            if (!thread->terminated)
            {
                auto prog_or_term = run_thread_to_sync(gctx, i, thread);
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

        std::cout << "----------- execution complete -----------" << std::endl;

        bool exception_detected = false;
        for (size_t i = 0; i < gctx.threads.size(); ++i)
        {
            const auto& thread = gctx.threads[i];
            if (thread->terminated)
            {
                switch (thread->terminated.value())
                {
                case TerminationStatus::completed:
                    std::cout << "Thread " << i << " terminated normally" << std::endl;
                    break;

                case TerminationStatus::unlock_exception:
                    std::cout << "Thread " << i << " unlocked a lock it does not own" << std::endl;
                    exception_detected = true;
                    break;

                case TerminationStatus::datarace_exception:
                    std::cout << "Thread " << i << " encountered a data-race" << std::endl;
                    exception_detected = true;
                    break;

                case TerminationStatus::assertion_failure_exception:
                    std::cout << "Thread " << i << " failed an assertion" << std::endl;
                    exception_detected = true;
                    break;

                case TerminationStatus::unassigned_variable_read_exception:
                    std::cout << "Thread " << i << " read an uninitialised value" << std::endl;
                    exception_detected = true;
                    break;

                default:
                    std::cout << "Thread " << i << " has an unhandled termination state" << std::endl;
                    break;
                }
            }
            else
            {
                exception_detected = true;
                std::cout << "Thread " << i << " is stuck" << std::endl;
            }
        }

        return exception_detected ? 1 : 0;
    }

    int interpret(const Node ast)
    {
        Node starting_block = ast / File / Block;
        ThreadContext starting_ctx = {};
        auto main_thread = std::make_shared<Thread>(starting_ctx, starting_block);

        GlobalContext gctx {{main_thread}, {}, {}};
        return run_threads(gctx);
    }

    // TODO: Everything from here should be moved to a separate file
    struct Command
    {
        enum
        {
            Step,    // Run to next sync point
            Finish,  // Finish the rest of the program
            Restart, // Start the program from the beginning
            List,    // List all threads
            Quit,    // Quit the interpreter
            Info,    // Show commands
            Skip,    // Do nothing, used for invalid commands
        } cmd;
        size_t argument = 0;
    };

    void show_thread(const Thread &thread, size_t tid)
    {
        std::cout << "---- Thread " << tid << std::endl;
        if (thread.ctx.locals.size() > 0) {
          for (auto& [reg, val] : thread.ctx.locals)
          {
            std::cout << reg << " = " << val << std::endl;
          }
          std::cout << "--" << std::endl;
        }

        if (thread.ctx.globals.size() > 0) {
          for (auto& [var, val] : thread.ctx.globals)
          {
            std::cout << var << " = " << val.val
                      << " [" << (val.commit? std::to_string(*val.commit): "_") << "; ";
            for (size_t i = 0; i < val.history.size(); ++i)
            {
              std::cout << val.history[i];
              if (i < val.history.size() - 1) {
                std::cout << ", ";
              }
            }
            std::cout << "]" << std::endl;
          }
          std::cout << "--" << std::endl;
        }

        size_t idx = 0;
        for (const auto &stmt : *thread.block)
        {
            if (idx == thread.pc)
            {
                std::cout << "-> ";
            }
            else
            {
                std::cout << "   ";
            }
            // Fix indentation of nested blocks
            auto s = std::string(stmt->location().view());
            s = std::regex_replace(s, std::regex("\n"), "\n   ");
            std::cout << s << ";" << std::endl;

            idx++;
        }
        if (thread.pc == thread.block->size()) {
            std::cout << "-> " << std::endl;
        }
    }

    void show_global_context(const GlobalContext& gctx, bool show_all = false)
    {
        auto& threads = gctx.threads;
        bool showed_any = false;
        for (size_t i = 0; i < threads.size(); i++)
        {
            auto thread = threads[i];
            if (show_all || !thread->terminated || *threads[i]->terminated != TerminationStatus::completed)
            {
                show_thread(*threads[i], i);
                std::cout << std::endl;
                showed_any = true;
            }
        }

        if (showed_any && gctx.locks.size() > 0)
        {
          std::cout << "---- Locks" << std::endl;

          for (const auto& [lock_name, lock] : gctx.locks)
          {
            std::cout << lock_name << ": ";
            if (lock.owner)
            {
                std::cout << *lock.owner;
            }
            else
            {
                std::cout << "<free>";
            }
            std::cout << std::endl;
          }

          if (gctx.locks.size() > 0)
            std::cout << "--" << std::endl;
        }
    }

    Command parse_command(std::string& input)
    {
        auto command = std::string(input);
        command.erase(0, command.find_first_not_of(" \t\n\r"));
        command.erase(command.find_last_not_of(" \t\n\r") + 1);

        if (command.find_first_not_of("0123456789") == std::string::npos)
        {
          // Interpret numbers as stepping
          return {Command::Step, std::stoul(command)};
        }
        else if (command == "s" || (command.at(0) == 's' && !std::isalpha(command.at(1))))
        {
          auto arg = command.substr(1);
          arg.erase(0, arg.find_first_not_of(" \t\n\r"));
          if (arg.size() > 0 && arg.find_first_not_of("0123456789") == std::string::npos)
          {
            return {Command::Step, std::stoul(arg)};
          }
          else
          {
            std::cout << "Expected thread id" << std::endl;
            return {Command::Skip};
          }
        }
        else if (command == "q")
        {
          return {Command::Quit};
        }
        else if (command == "r")
        {
          return {Command::Restart};
        }
        else if (command == "f")
        {
          return {Command::Finish};
        }
        else if (command == "l")
        {
          return {Command::List};
        }
        else if (command == "?")
        {
          return {Command::Info};
        }
        else
        {
          std::cout << "Unknown command: " << input << std::endl;
          return {Command::Skip};
        }
    }

    int interpret_interactive(const Node ast)
    {
        Node starting_block = ast / File / Block;
        ThreadContext starting_ctx = {};
        auto main_thread = std::make_shared<Thread>(starting_ctx, starting_block);

        GlobalContext gctx {{main_thread}, {}, {}};

        size_t prev_no_threads = 0;
        Command command = {Command::List};
        std::string msg = "";
        while (command.cmd != Command::Quit)
        {
            if (command.cmd != Command::Skip || prev_no_threads != gctx.threads.size())
            {
                bool show_all = command.cmd == Command::List;
                show_global_context(gctx, show_all);
            }
            prev_no_threads = gctx.threads.size();

            if (!msg.empty())
            {
                std::cout << msg << std::endl;
                msg.clear();
            }

            std::cout << "> ";
            std::string input;
            std::getline(std::cin, input);
            if (!input.empty() && input.find_first_not_of(" \t\n\r") != std::string::npos)
            {
                command = parse_command(input);
            }

            if (command.cmd == Command::Step)
            {
                auto tid = command.argument;
                if (tid >= gctx.threads.size())
                {
                    msg = "Invalid thread id: " + std::to_string(tid);
                    command = {Command::Skip};
                    continue;
                }

                auto thread = gctx.threads[tid];
                if (auto term = thread->terminated)
                {
                    if (*term == TerminationStatus::completed)
                    {
                        msg = "Thread " + std::to_string(tid) + " has terminated normally";
                    }
                    else
                    {
                        msg = "Thread " + std::to_string(tid) + " has terminated with an error";
                    }
                    command = {Command::Skip};
                    continue;
                }

                auto prog_or_term = run_thread_to_sync(gctx, tid, thread);
                if (ProgressStatus* prog = std::get_if<ProgressStatus>(&prog_or_term))
                {
                    if (!*prog) {
                      auto stmt = thread->block->at(thread->pc);
                      msg = "Thread " + std::to_string(tid) + " is blocking on '" + std::string(stmt->location().view()) + "'";
                      command = {Command::Skip};
                    }
                }
                else if (TerminationStatus* term = std::get_if<TerminationStatus>(&prog_or_term))
                {
                    switch (*term)
                    {
                    case TerminationStatus::completed:
                        msg = "Thread " + std::to_string(tid) + " terminated normally";
                        break;
                    case TerminationStatus::datarace_exception:
                        // TODO: Say on which variable the datarace occurred. To
                        // do this, have pull return an optional variable that
                        // is in a race and have the data race exception
                        // remember that variable.
                        msg = "Thread " + std::to_string(tid) + " encountered a data race and was terminated";
                        command = {Command::Skip};
                        break;
                    case TerminationStatus::assertion_failure_exception: {
                        auto expr = thread->block->at(thread->pc) / Stmt / Expr;
                        msg = "Thread " + std::to_string(tid) + " failed assertion '" + std::string(expr->location().view()) + "' and was terminated";
                        command = {Command::Skip};
                        break;
                    }
                    case TerminationStatus::unassigned_variable_read_exception:
                        throw std::runtime_error("Thread " + std::to_string(tid) + " read an uninitialised variable");
                    case TerminationStatus::unlock_exception:
                        throw std::runtime_error("Thread " + std::to_string(tid) + " unlocked an unlocked lock");
                    default:
                        throw std::runtime_error("Thread " + std::to_string(tid) + " has an unhandled termination state");
                    }
                }
            }
            else if (command.cmd == Command::Finish)
            {
                // Finish the program
                if (!run_threads(gctx))
                    msg = "Program finished successfully";
                else
                    msg = "Program terminated with an error";
            }
            else if (command.cmd == Command::Restart)
            {
                // Start the program from the beginning
                ThreadContext new_starting_ctx = {};
                auto new_main_thread = std::make_shared<Thread>(new_starting_ctx, starting_block);
                gctx = {{new_main_thread}, {}, {}};

                command = {Command::List};
            }
            else if (command.cmd == Command::List)
            {
                // Listing is a no-op
            }
            else if (command.cmd == Command::Info)
            {
                std::cout << "Commands:" << std::endl;
                std::cout << "s [tid] - Step to next sync point in thread" << std::endl;
                std::cout << "[tid] - Step to next sync point in thread" << std::endl;
                std::cout << "f - Finish the program" << std::endl;
                std::cout << "r - Restart the program" << std::endl;
                std::cout << "l - List all threads" << std::endl;
                std::cout << "q - Quit the interpreter" << std::endl;
                command = {Command::Skip};
            }
            else if (command.cmd == Command::Quit)
            {
                // Quit is a no-op
            }
        }
        return 0;
    }

    // Model checking code

    struct TraceNode
    {
        size_t tid_;
        bool complete;
        std::vector<std::shared_ptr<TraceNode>> children;

        TraceNode(const size_t tid) : tid_(tid), complete(false) {}

        std::shared_ptr<TraceNode> extend(ThreadID tid)
        {
            children.push_back(std::make_shared<TraceNode>(tid));
            return children.back();
        }
    };

    int model_check(const Node ast)
    {
        Node starting_block = ast / File / Block;
        ThreadContext starting_ctx = {};
        auto main_thread = std::make_shared<Thread>(starting_ctx, starting_block);
        GlobalContext gctx {{main_thread}, {}, {}};

        auto final_states = std::vector<GlobalContext>{};
        auto traces = std::vector<std::vector<size_t>>{};

        const auto root = std::make_shared<TraceNode>(0);
        auto cursor = root;
        auto current_trace = std::vector<size_t>{0}; // Start with the main thread
        run_thread_to_sync(gctx, cursor->tid_, gctx.threads[cursor->tid_]);

        // TODO: Check for newly spawned threads when checking for progress
        while (!root->complete)
        {
            std::cout << "== Top of the loop" << std::endl;
            // Step through the trace
            while (!cursor->children.empty() && !cursor->children.back()->complete)
            {
                // We have a child that is not complete, we can extend the trace
                std::cout << "== Extending trace with thread " << cursor->children.back()->tid_ << std::endl;
                cursor = cursor->children.back();
                current_trace.push_back(cursor->tid_);
                run_thread_to_sync(gctx, cursor->tid_, gctx.threads[cursor->tid_]);
            }

            // Try to find a thread to schedule next
            size_t start_idx = cursor->children.empty() ? 0 : cursor->children.back()->tid_ + 1;
            bool made_progress = false;
            for (size_t i = start_idx; i < gctx.threads.size() && !made_progress; ++i)
            {
                auto thread = gctx.threads[i];
                if (!thread->terminated)
                {
                    // Run the thread to the next sync point
                    auto prog_or_term = run_thread_to_sync(gctx, i, thread);
                    if (std::holds_alternative<TerminationStatus>(prog_or_term))
                    {
                        // Thread terminated, we can extend the trace
                        std::cout << "== Thread " << i << " terminated" << std::endl;
                        made_progress = true;
                        cursor = cursor->extend(i);
                        current_trace.push_back(i);
                        if (std::get<TerminationStatus>(prog_or_term) != TerminationStatus::completed)
                        {
                            // Thread terminated with an error, we can stop here
                            cursor->complete = true;
                        }
                    }
                    else if (std::get<ProgressStatus>(prog_or_term) == ProgressStatus::progress)
                    {
                        // Thread made progress, we can continue
                        std::cout << "== Thread " << i << " made progress" << std::endl;
                        made_progress = true;
                        cursor = cursor->extend(i);
                        current_trace.push_back(i);
                    }
                }
            }

            if (!made_progress)
            {
                // No further threads made progress, we can stop here
                cursor->complete = true;
            }

            bool final_state =
                   std::any_of(gctx.threads.begin(), gctx.threads.end(),
                               [](const auto& thread) { return thread->terminated && *thread->terminated != TerminationStatus::completed; })
                || std::all_of(gctx.threads.begin(), gctx.threads.end(),
                               [](const auto& thread) { return thread->terminated && *thread->terminated == TerminationStatus::completed; });

            if (final_state)
            {
                // Remember final result if it is new
                if (!std::any_of(final_states.begin(), final_states.end(),
                                 [&gctx](const GlobalContext& state) { return state == gctx; }))
                {
                    std::cout << "== Found a new final state" << std::endl;
                    final_states.push_back(gctx);
                    traces.push_back(current_trace);
                }

                cursor->complete = true;
            }

            if (cursor->complete)
            {
                std::cout << "Restarting..." << std::endl;
                ThreadContext new_starting_ctx = {};
                auto new_main_thread = std::make_shared<Thread>(new_starting_ctx, starting_block);
                gctx = {{new_main_thread}, {}, {}};

                cursor = root;
                current_trace.clear();
                current_trace.push_back(0); // Start with the main thread again
                run_thread_to_sync(gctx, cursor->tid_, gctx.threads[cursor->tid_]);
            }
        }

        std::cout << "Found a total of " << traces.size() << " trace(s) with distinct final states:" << std::endl;
        for (auto& trace : traces)
        {
            for (size_t i = 0; i < trace.size(); ++i)
            {
                std::cout << trace[i] << " ";
            }
            std::cout << std::endl;
        }

        return 0;
    }
}
