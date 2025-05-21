#include <trieste/trieste.h>
#include "lang.hh"

namespace gitmem
{
    using namespace trieste;
    using Locals = std::unordered_map<std::string, size_t>;

    struct Context
    {
        Locals locals;
    };

    struct Thread
    {
        Context ctx;
        Node block;
        size_t pc = 0;
    };

    using Threads = std::vector<Thread>;

    bool is_syncing(Node stmt)
    {
        auto s = stmt / Stmt;
        return s == Join || s == Lock || s == Unlock;
    }

    size_t evaluate_expression(Node expr, Context &ctx)
    {
        auto e = expr / Expr;
        if (e == Reg)
        {
            return ctx.locals[std::string(expr->location().view())];
        }
        else if (e == Var)
        {
            std::cout << "Global variables not implemented yet" << std::endl;
            return 0;
        }
        else if (e == Const)
        {
            return std::stoi(std::string(e->location().view()));
        }
        else if (e == Spawn)
        {
            std::cout << "Spawn not implemented yet" << std::endl;
            return 0;
        }
        else if (e == Eq)
        {
            auto lhs = e / Lhs;
            auto rhs = e / Rhs;
            return evaluate_expression(lhs, ctx) == evaluate_expression(rhs, ctx);
        }
        else
        {
            std::cout << "Unknown expression: " << expr->type() << std::endl;
            return 0;
        }
    }

    void run_statement(Node stmt, Context &ctx)
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
            auto val = evaluate_expression(rhs, ctx);
            if (lhs == Reg)
            {
                std::cout << "Setting register '" << lhs->location().view() << "' to " << val << std::endl;
                ctx.locals[var] = val;
            }
            else if (lhs == Var)
            {
                std::cout << "Global variables not implemented yet" << std::endl;
            }
            else
            {
                std::cout << "Bad left-hand side: " << lhs->type() << std::endl;
            }
        }
        else if (s == Join)
        {
            std::cout << "Join not implemented yet" << std::endl;
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
            auto result = evaluate_expression(expr, ctx);
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
    }

    void run_thread_to_sync(Thread &thread)
    {
        Node block = thread.block;
        size_t &pc = thread.pc;
        Context &ctx = thread.ctx;
        while (pc < block->size())
        {
            Node stmt = block->at(pc);
            if (is_syncing(stmt)) return;

            run_statement(stmt, ctx);
            pc++;
        }
    }

    void run_threads_to_sync(Threads &threads)
    {
        for (size_t i = 0; i < threads.size(); i++)
        {
            Thread &thread = threads[i];
            run_thread_to_sync(thread);
        }
    }

    void run_threads(Threads &threads)
    {
        run_threads_to_sync(threads);
        // TODO: Set up multiverses
    }

    void interpret(const Node ast)
    {
        Node starting_block = ast / File / Block;
        Context starting_ctx = {};
        Thread main_thread = {starting_ctx, starting_block};
        Threads threads = {main_thread};
        // TODO: Set up global context
        run_threads(threads);
    }
}
