#include <trieste/trieste.h>
#include <random>
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

    struct Lock {
        bool locked;
        Globals globals;
    };

    using Lock = gitmem::Lock;

    struct ThreadContext
    {
        Locals locals;
        Globals globals;
    };

    struct Thread
    {
        ThreadContext ctx;
        Node block;
        size_t id;
        size_t pc = 0;
        bool completed = false;
    };

    using Threads = std::vector<std::shared_ptr<Thread>>;
    using Locks = std::vector<std::shared_ptr<Lock>>;

    struct GlobalContext {
        Threads threads;
        Locks locks;
    };

    bool is_syncing(Node stmt)
    {
        auto s = stmt / Stmt;
        return s == Join || s == Lock || s == Unlock;
    }

    void commit(ThreadContext &ctx) {
        for (auto& [var, global] : ctx.globals) {
            if (global.commit)
            {
                std::cout << "Committing global '" << var << "' with id " << global.commit.value() << std::endl;
                global.history.push_back(global.commit.value());
                global.commit.reset();
            }
        }
    }

    bool conflict(std::vector<size_t>& h1, std::vector<size_t>& h2)
    {
        size_t length = std::min(h1.size(), h2.size());

        bool conflict = false;
        for (size_t i = 0; i < length && !conflict; ++i)
        {
            std::cout << h1[i] << " = "  << h2[i] << std::endl;
            conflict |= (h1[i] != h2[i]);
        }

        return conflict;
    }

    // The changes from source are pulled into destination
    bool pull(ThreadContext &dst, ThreadContext &src) {
        std::cout << "Pull" << std::endl;

        for (auto& [var, global] : src.globals) {
            std::cout << var << std::endl;
            if (dst.globals.contains(var))
            {
                auto& src_var = src.globals[var];
                auto& dst_var = dst.globals[var];
                if (conflict(src_var.history, dst_var.history))
                {
                    std::cout << "Data race" << std::endl;
                    return false;
                }
                else if (src_var.history.size() > dst_var.history.size())
                {
                    std::cout << "Fast-forwards '" << var << "' to " << src_var.val << " with id " << *(src_var.history.end() - 1) << std::endl;
                    dst_var.val = src_var.val;
                    dst_var.history = src_var.history;
                }
            }
            else
            {
                dst.globals[var].val = src.globals[var].val;
                dst.globals[var].history = src.globals[var].history;
            }
        }
        return true;
    }

    size_t get_uuid() {
        static size_t uuid = 0;
        return uuid++;
    }

    size_t evaluate_expression(Node expr, ThreadContext &ctx, GlobalContext &gctx)
    {
        auto e = expr / Expr;
        if (e == Reg)
        {
            return ctx.locals[std::string(expr->location().view())];
        }
        else if (e == Var)
        {
            return ctx.globals[std::string(expr->location().view())].val;
        }
        else if (e == Const)
        {
            return std::stoi(std::string(e->location().view()));
        }
        else if (e == Spawn)
        {
            commit(ctx);
            ThreadContext new_ctx = { Locals(), ctx.globals };
            gctx.threads.push_back(std::make_shared<Thread>(new_ctx, e / Block));
            return gctx.threads.size() - 1;
        }
        else if (e == Eq)
        {
            auto lhs = e / Lhs;
            auto rhs = e / Rhs;
            return evaluate_expression(lhs, ctx, gctx) == evaluate_expression(rhs, ctx, gctx);
        }
        else
        {
            std::cout << "Unknown expression: " << expr->type() << std::endl;
            return 0;
        }
    }

    bool run_statement(Node stmt, ThreadContext &ctx, GlobalContext &gctx)
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
            auto val = evaluate_expression(rhs, ctx, gctx);
            if (lhs == Reg)
            {
                std::cout << "Setting register '" << lhs->location().view() << "' to " << val << std::endl;
                ctx.locals[var] = val;
            }
            else if (lhs == Var)
            {
                auto &global = ctx.globals[var];
                global.val = val;
                global.commit = get_uuid();
                std::cout << "Setting global '" << lhs->location().view() << "' to " << val <<  " with id " << *(global.commit) << std::endl;
            }
            else
            {
                std::cout << "Bad left-hand side: " << lhs->type() << std::endl;
            }
        }
        else if (s == Join)
        {
            auto expr = s / Expr;
            auto result = evaluate_expression(expr, ctx, gctx);

            auto& thread = gctx.threads[result];
            if (thread->completed)
            {
                commit(ctx);
                commit(thread->ctx);
                pull(ctx, thread->ctx);
            }
            else
            {
                // The thread to join on is incomplete so we have to stop
                std::cout << "Waiting on thread " << result << std::endl;
                return false;
            }
        }
        else if (s == Lock)
        {
            std::cout << "Lock not implemented yet" << std::endl;
        }
        else if (s == Unlock)
        {
            std::cout << "Unlock not implemented yet" << std::endl;
        }
        else if (s == Assert)
        {
            auto expr = s / Expr;
            auto result = evaluate_expression(expr, ctx, gctx);
            if (!result)
            {
                std::cout << "Assertion failed: " << expr->location().view() << std::endl;
            }
            else
            {
                std::cout << "Assertion passed: " << expr->location().view() << std::endl;
            }
        }
        else
        {
            std::cout << "Unknown statement: " << stmt->type() << std::endl;
        }
        return true;
    }

    // Add a descriptive return value
    bool run_thread_to_sync(GlobalContext &gctx, std::shared_ptr<Thread> thread)
    {
        if (thread->completed)
            return false;

        Node block = thread->block;
        size_t &pc = thread->pc;
        ThreadContext &ctx = thread->ctx;
        bool first = true;

        while (pc < block->size())
        {
            Node stmt = block->at(pc);
            if (!first && is_syncing(stmt))
                return first;

            if (!run_statement(stmt, ctx, gctx))
                return false;

            pc++;
            first = false;
        }

        thread->completed = true;
        return true;
    }

    bool run_threads_to_sync(GlobalContext &gctx)
    {
        bool all_completed = true;
        bool stuck = true;
        for (size_t i = 0; i < gctx.threads.size(); ++i) {
            stuck &= !run_thread_to_sync(gctx, gctx.threads[i]);
            all_completed &= gctx.threads[i]->completed;
            // if a thread spawns a new thread, it will end up at the end so
            // we will always include the new threads in the termination
            // criteria
        }

        if (stuck)
            std::cout << "Deadlock" << std::endl;
        return stuck || all_completed;
    }

    void run_threads(GlobalContext &gctx)
    {
        while (!run_threads_to_sync(gctx)) {
            std::cout << "-----------------------" << std::endl;
        }
    }

    void interpret(const Node ast)
    {
        Node starting_block = ast / File / Block;
        ThreadContext starting_ctx = {};
        auto main_thread = std::make_shared<Thread>(starting_ctx, starting_block);

        // TODO: Set up global context
        GlobalContext gctx {{main_thread}};
        run_threads(gctx);
    }
}
