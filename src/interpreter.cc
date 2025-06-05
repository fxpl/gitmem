#include <trieste/trieste.h>
#include <random>
#include <variant>
#include "lang.hh"

namespace gitmem
{
    using namespace trieste;

    using Locals = std::unordered_map<std::string, size_t>;

    struct Global {
        size_t val;
        std::optional<size_t> commit;
        std::vector<size_t> history;
    };

    using Globals = std::unordered_map<std::string, Global>;

    enum class TerminationStatus {
        completed,
        datarace_exception,
        unlock_exception,
        assertion_failure_exception,
        unassigned_variable_read_exception,
    };

    using ThreadStatus = std::optional<TerminationStatus>;

    struct ThreadContext
    {
        Locals locals;
        Globals globals;
    };

    using ThreadID = size_t;

    struct Thread
    {
        ThreadContext ctx;
        Node block;
        size_t pc = 0;
        ThreadStatus terminated = std::nullopt;
    };

    using Threads = std::vector<std::shared_ptr<Thread>>;

    struct Lock {
        Globals globals;
        std::optional<ThreadID> owner = std::nullopt;
    };

    using Locks = std::unordered_map<std::string, struct Lock>;

    struct GlobalContext {
        Threads threads;
        Locks locks;
    };

    enum class ProgressStatus {
        progress,
        no_progress
    };

    bool operator!(ProgressStatus p) { return p == ProgressStatus::no_progress; }

    bool is_syncing(Node stmt)
    {
        auto s = stmt / Stmt;
        return s == Join || s == Lock || s == Unlock;
    }

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

    bool has_conflict(std::vector<size_t>& h1, std::vector<size_t>& h2)
    {
        size_t length = std::min(h1.size(), h2.size());

        bool conflict = false;
        for (size_t i = 0; i < length && !conflict; ++i)
        {
            conflict |= (h1[i] != h2[i]);
        }

        return conflict;
    }

    // The changes from source are pulled into destination
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

    size_t get_uuid() {
        static size_t uuid = 0;
        return uuid++;
    }

    std::variant<size_t, TerminationStatus> evaluate_expression(Node expr, GlobalContext &gctx, ThreadContext &ctx)
    {
        auto e = expr / Expr;
        if (e == Reg)
        {
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

            // variant type can be used as a monad and there are methods
            // to sort of do monadic composition but they're kind of horrible
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

    std::variant<ProgressStatus, TerminationStatus> run_statement(Node stmt, NodeMap<size_t>& cache, GlobalContext &gctx, ThreadContext &ctx, const ThreadID& tid)
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
                    std::cout << "Set register '" << lhs->location().view() << "' to " << *val << std::endl;
                    ctx.locals[var] = *val;
                }
                else if (lhs == Var)
                {
                    auto &global = ctx.globals[var];
                    global.val = *val;
                    global.commit = get_uuid();
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
            auto expr = s / Expr;

            if (!cache.contains(expr))
            {
                auto val_or_term = evaluate_expression(expr, gctx, ctx);
                if (size_t* val = std::get_if<size_t>(&val_or_term))
                {
                    cache[expr] = *val;
                }
                else
                {
                    return std::get<TerminationStatus>(val_or_term);
                }
            }

            auto result = cache[expr];
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
            commit(ctx.globals);
            auto v = s / Var;
            auto var = std::string(v->location().view());

            auto& lock = gctx.locks[var];
            if (!lock.owner)
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

    // The problem at the moment in join_spawn is in the final loop
    // Thread 1 makes no progress
    // Thread 2 completes and it's progress flag gets lost
    // So in the final pass, neither thread progresses but thread 1 is not complete

    std::variant<ProgressStatus, TerminationStatus> run_thread_to_sync(GlobalContext& gctx, const ThreadID& tid, std::shared_ptr<Thread> thread, NodeMap<size_t>& cache)
    {
        if (thread->terminated) return ProgressStatus::no_progress;

        Node block = thread->block;
        size_t &pc = thread->pc;
        ThreadContext &ctx = thread->ctx;

        bool first_statement = true;
        while(pc < block->size())
        {
            Node stmt = block->at(pc);

            if (!first_statement && is_syncing(stmt))
                return ProgressStatus::progress;

            auto prog_or_term = run_statement(stmt, cache, gctx, ctx, tid);
            if (std::holds_alternative<TerminationStatus>(prog_or_term))
                return prog_or_term;

            if(!(std::get<ProgressStatus>(prog_or_term)))
            {
                std::cout << "exit first statement: " << (first_statement ? "true" : "false") << std::endl;
                return first_statement ? ProgressStatus::no_progress : ProgressStatus::progress;
            }

            pc++;
            first_statement = false;
        }

        return TerminationStatus::completed;
    }

    // true if termination reached
    bool run_threads_to_sync(GlobalContext& gctx, NodeMap<size_t>& cache)
    {
        std::cout << "-----------------------" << std::endl;
        bool all_completed = true;
        bool any_progress = false;
        for (size_t i = 0; i < gctx.threads.size(); ++i)
        {
            std::cout << "==== t" << i << " ====" << std::endl;
            auto prog_or_term = run_thread_to_sync(gctx, i, gctx.threads[i], cache);
            if (ProgressStatus* prog = std::get_if<ProgressStatus>(&prog_or_term))
            {
                std::cout << "here" << std::endl;
                any_progress |= !!(*prog);
                std::cout << "any_progress: " << any_progress << std::endl;
            }
            else
            {
                gctx.threads[i]->terminated = std::get<TerminationStatus>(prog_or_term);
                any_progress |= (gctx.threads[i]->terminated == TerminationStatus::completed);
            }

            all_completed &= gctx.threads[i]->terminated.has_value();
            // if a thread spawns a new thread, it will end up at the end so
            // we will always include the new threads in the termination
            // criteria
        }

        std::cout << "any_progress: " << any_progress << " all_completed: " << all_completed << std::endl;

        return (!any_progress || all_completed);
    }

    int run_threads(GlobalContext &gctx)
    {
        NodeMap<size_t> cache;

        while (!run_threads_to_sync(gctx, cache)) {}

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
                    std::cout << "Thread " << i << " unlocked an unlocked lock" << std::endl;
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

        GlobalContext gctx {{main_thread}, {}};
        return run_threads(gctx);
    }
}
