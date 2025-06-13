#include <regex>

#include "interpreter.hh"

namespace gitmem
{
    /** A command that can be parsed by the debugger. Some commands store a
     * ThreadID argument. */
    struct Command
    {
        enum
        {
            Step,    // Run a specified thread to next sync point
            Finish,  // Finish the rest of the program
            Restart, // Start the program from the beginning
            List,    // List all threads
            Quit,    // Quit the interpreter
            Info,    // Show commands
            Skip,    // Do nothing, used for invalid commands
        } cmd;
        ThreadID argument = 0;
    };

    /** Print the state of a thread, including its local and global variables,
     * and the current position in the program. */
    void show_thread(const Thread &thread, size_t tid)
    {
        std::cout << "---- Thread " << tid << std::endl;
        if (thread.ctx.locals.size() > 0)
        {
            for (auto &[reg, val] : thread.ctx.locals)
            {
                std::cout << reg << " = " << val << std::endl;
            }
            std::cout << "--" << std::endl;
        }

        if (thread.ctx.globals.size() > 0)
        {
            for (auto &[var, val] : thread.ctx.globals)
            {
                std::cout << var << " = " << val.val
                          << " [" << (val.commit ? std::to_string(*val.commit) : "_") << "; ";
                for (size_t i = 0; i < val.history.size(); ++i)
                {
                    std::cout << val.history[i];
                    if (i < val.history.size() - 1)
                    {
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
        if (thread.pc == thread.block->size())
        {
            std::cout << "-> " << std::endl;
        }
    }

    /** Show the global context, including locks and non-completed threads. If
     * show_all is true, show all threads, even those that have terminated
     * normally. */
    void show_global_context(const GlobalContext &gctx, bool show_all = false)
    {
        auto &threads = gctx.threads;
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

            for (const auto &[lock_name, lock] : gctx.locks)
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

    /** Parse a command. See the help string for the 'Info' command for details.
     */
    Command parse_command(std::string &input)
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

    /** Interpret the AST in an interactive way, letting the user choose which
     * thread to schedule next. */
    int interpret_interactive(const Node ast)
    {
        Node starting_block = ast / File / Block;
        ThreadContext starting_ctx = {};
        auto node = std::make_shared<graph::Start>(0);
        starting_ctx.tail = node;
        auto main_thread = std::make_shared<Thread>(starting_ctx, starting_block);

        GlobalContext gctx{{main_thread}, {}, {}};

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
                if (ProgressStatus *prog = std::get_if<ProgressStatus>(&prog_or_term))
                {
                    if (!*prog)
                    {
                        auto stmt = thread->block->at(thread->pc);
                        msg = "Thread " + std::to_string(tid) + " is blocking on '" + std::string(stmt->location().view()) + "'";
                        command = {Command::Skip};
                    }
                }
                else if (TerminationStatus *term = std::get_if<TerminationStatus>(&prog_or_term))
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
                    case TerminationStatus::assertion_failure_exception:
                    {
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
                auto node = std::make_shared<graph::Start>(0);
                starting_ctx.tail = node;
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
}
