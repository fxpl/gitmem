#include <regex>
#include <cstdlib>

#include "debug.hh"
#include "debugger.hh"
#include "interpreter.hh"
#include "overloaded.hh"

namespace gitmem {
/** A command that can be parsed by the debugger. Some commands store a
 * ThreadID argument. */
struct Command {
  enum {
    Step,    // Run a specified thread to next sync point
    Finish,  // Finish the rest of the program
    Restart, // Start the program from the beginning
    List,    // List all threads
    Print,   // Print the execution graph
    Graph,   // Toggle automatically printing the execution graph
    Quit,    // Quit the interpreter
    Info,    // Show commands
    Skip,    // Do nothing, used for invalid commands
  } cmd;
  ThreadID argument = 0;
};

/** Clear the terminal screen (platform-specific) */
void clear_terminal() {
#ifdef _WIN32
    std::system("cls");
#else
    std::system("clear");
#endif
}

/** Print a visual separator line */
void print_separator() {
    std::cout << std::string(60, '=') << std::endl;
}

/** Parse a command. See the help string for the 'Info' command for details.
 */
Command parse_command(std::string &input) {
  auto command = std::string(input);
  command.erase(0, command.find_first_not_of(" \t\n\r"));
  command.erase(command.find_last_not_of(" \t\n\r") + 1);

  if (command.find_first_not_of("0123456789") == std::string::npos) {
    // Interpret numbers as stepping
    return {Command::Step, std::stoul(command)};
  } else if (command == "s" ||
             (command.at(0) == 's' && !std::isalpha(command.at(1)))) {
    auto arg = command.substr(1);
    arg.erase(0, arg.find_first_not_of(" \t\n\r"));
    if (arg.size() > 0 &&
        arg.find_first_not_of("0123456789") == std::string::npos) {
      return {Command::Step, std::stoul(arg)};
    } else {
      std::cout << "Expected thread id" << std::endl;
      return {Command::Skip};
    }
  } else if (command == "q") {
    return {Command::Quit};
  } else if (command == "r") {
    return {Command::Restart};
  } else if (command == "f") {
    return {Command::Finish};
  } else if (command == "l") {
    return {Command::List};
  } else if (command == "g") {
    return {Command::Graph};
  } else if (command == "p") {
    return {Command::Print};
  } else if (command == "?") {
    return {Command::Info};
  } else {
    std::cout << "Unknown command: " << input << std::endl;
    return {Command::Skip};
  }
}

enum class StepKind {
  Progressed,   // Thread made progress
  Blocked,      // Thread is blocked on sync
  Terminated,   // Thread terminated this step
  Invalid       // Invalid thread id, etc.
};

struct StepUIResult {
  StepKind kind;
  std::optional<TerminationStatus> termination;
  std::optional<std::string> message;

  static StepUIResult progressed() {
    return {StepKind::Progressed, std::nullopt, std::nullopt};
  }

  static StepUIResult blocked(std::string msg) {
    return {StepKind::Blocked, std::nullopt, std::move(msg)};
  }

  static StepUIResult terminated(TerminationStatus t) {
    return {StepKind::Terminated, t, std::nullopt};
  }

  static StepUIResult invalid(std::string msg) {
    return {StepKind::Invalid, std::nullopt, std::move(msg)};
  }

  bool has_message() { return message.has_value(); }
  std::string& get_message() { return *message; }

  bool has_terminated() { return termination.has_value(); }
  TerminationStatus& get_termination() { return *termination; }
};

StepUIResult step_thread(Interpreter& interp, ThreadID tid) {
  if (!interp.has_thread(tid)) {
    return StepUIResult::invalid(
        "Invalid thread id: " + std::to_string(tid));
  }

  if (auto term = interp.thread_termination(tid)) {
    return StepUIResult::terminated(*term);
  }

  auto prog_or_term = interp.progress_thread(tid);

  if (auto prog = std::get_if<ProgressStatus>(&prog_or_term)) {
    if (*prog == ProgressStatus::no_progress) {
      return StepUIResult::blocked(
          "Thread " + std::to_string(tid) + " is blocking on '" +
          interp.pending_statement(tid).value_or("...") + "'");
    }
    return StepUIResult::progressed();
  }

  auto term = std::get<TerminationStatus>(prog_or_term);
  return StepUIResult::terminated(term);
}

/** Print the execution graph if requested */
void maybe_print_graph(Interpreter& interp,
                       bool print_graphs,
                       const std::filesystem::path &output_file) {
    if (print_graphs) {
      interp.print_revision_graph(output_file);
      interp.print_execution_graph(output_file);
      verbose::out << "Execution graph written to " << output_file << std::endl;
    }
}

/** Step a single thread and return the StepUIResult. Also prints the message. */
StepUIResult do_step(Interpreter &interp,
                   ThreadID tid,
                   bool print_graphs,
                   const std::filesystem::path &output_file) {
    StepUIResult result = step_thread(interp, tid);
    if (result.has_message())
        std::cout << result.get_message() << std::endl;
    if (result.has_terminated()) {
      std::cout << "Thread " << tid << ": ";
      std::visit(
        overloaded{
          [&](const auto &t) {
            // Any non-completed termination is exceptional
            std::cout << t << std::endl;
          }
        },
        result.get_termination()
      );
    }

    maybe_print_graph(interp, print_graphs, output_file);
    return result;
}

/** Reset the interpreter to a fresh state */
void do_restart(Interpreter &interp,
                const trieste::Node ast,
                bool print_graphs,
                const std::filesystem::path &output_file,
                const MemoryModelFactory& make_model) {
  trieste::Node starting_block = entry_block(ast);
  interp = Interpreter(GlobalContext(starting_block, make_model()));
    maybe_print_graph(interp, print_graphs, output_file);
}

/** Print the list of threads and optionally all threads */
void do_list(Interpreter &interp, bool show_all) {
    // Uncomment the next line if you prefer clearing the screen
    // clear_terminal();

    print_separator();
    interp.print_state(std::cout, show_all);
    print_separator();
}

void do_finish(Interpreter& interp, bool print_graphs, const std::filesystem::path &output_file) {
  if (!interp.run()) {
    std::cout << "Program finished successfully" << std::endl;
  } else {
    std::cout << "Program terminated with an error" << std::endl;
  }

  maybe_print_graph(interp, print_graphs, output_file);
}

/** Print interactive command help */
void print_help() {
    std::cout << "Commands:\n";
    std::cout << "s <tid> - Step to next sync point in thread\n";
    std::cout << "<tid>   - Step to next sync point in thread\n";
    std::cout << "f      - Finish the program\n";
    std::cout << "r      - Restart the program\n";
    std::cout << "l      - List all threads\n";
    std::cout << "g      - Toggle automatic execution graph printing\n";
    std::cout << "p      - Print the execution graph immediately\n";
    std::cout << "q      - Quit the interpreter\n";
    std::cout << "?      - Display this help message\n";
}

/** Main interactive interpreter loop */
int interpret_interactive(const trieste::Node ast,
                          const std::filesystem::path &output_file,
                          const MemoryModelFactory& make_model) {
  trieste::Node starting_block = entry_block(ast);
  Interpreter interp(GlobalContext(starting_block, make_model()));

    size_t prev_no_threads = 1;
    Command command = {Command::List};
    bool print_graphs = true;

    // clear the graph at the start
    maybe_print_graph(interp, print_graphs, output_file);

    while (command.cmd != Command::Quit) {
        // Print threads if new threads appeared or command is List
      if (command.cmd != Command::Skip || prev_no_threads != interp.thread_count()) {
            do_list(interp, command.cmd == Command::List);
        }
      prev_no_threads = interp.thread_count();

        // Read user input
        std::cout << "> ";
        std::string input;
        std::getline(std::cin, input);
        if (!input.empty() && input.find_first_not_of(" \t\n\r") != std::string::npos)
            command = parse_command(input);

        switch (command.cmd) {
            case Command::Step: {
                ThreadID tid = command.argument;
                StepUIResult res = do_step(interp, tid, print_graphs, output_file);
                if (res.kind != StepKind::Progressed && res.kind != StepKind::Terminated)
                    command = {Command::Skip};
                break;
            }

            case Command::Finish:
                do_finish(interp, print_graphs, output_file);
                break;

            case Command::Restart:
                do_restart(interp, ast, print_graphs, output_file, make_model);
                command = {Command::List};
                break;

            case Command::List:
                // Already handled before reading input, no-op here
                break;

            case Command::Graph:
                print_graphs = !print_graphs;
                std::cout << "Graphs " << (print_graphs ? "will" : "won't")
                          << " print automatically" << std::endl;
                command = {Command::Skip};
                break;

            case Command::Print:
                maybe_print_graph(interp, print_graphs, output_file);
                command = {Command::Skip};
                break;

            case Command::Info:
                print_help();
                command = {Command::Skip};
                break;

            case Command::Skip:
                // No-op
                break;

            case Command::Quit:
                // No-op
                break;
        }
    }

    return 0;
}

} // namespace gitmem