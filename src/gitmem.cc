#include <CLI/CLI.hpp>

#include "debug.hh"
#include "interpreter.hh"
#include "model_checker.hh"
#include "debugger.hh"
#include "lang.hh"
#include "linear/memory_model.hh"
#include "branching/base_memory_model.hh"
#include "branching/eager/memory_model.hh"
#include "branching/lazy/memory_model.hh"

int main(int argc, char **argv) {
  using namespace trieste;
  CLI::App app;

  std::filesystem::path input_path;
  app.add_option("input", input_path, "Path to the input file ")
      ->required()
      ->check(CLI::ExistingFile);

  std::filesystem::path output_path = "";
  app.add_option("-o,--output", output_path, "Path to the output file.");

  bool verbose = false;
  app.add_flag("-v,--verbose", verbose,
               "Enable verbose output from the interpreter.");

  std::string sync_model = "linear";
  auto sync_opt = app.add_option("--sync", sync_model, "Select a memory model for execution (default: linear)")
    ->check(CLI::IsMember({"linear", "branching"}))
    ->type_name("KIND");

  std::string branching_mode = "eager";
  auto branching_mode_opt = app.add_option("--branching-mode", branching_mode, "Select branching mode: eager or lazy (default: eager)")
    ->check(CLI::IsMember({"eager", "lazy"}))
    ->type_name("MODE");

  bool include_empty_commits = false;
  auto include_empty_opt = app.add_flag("--include-empty-commits", include_empty_commits,
               "Include empty commits in branching memory model output (branching mode only.).");

  bool raise_early_conflicts = false;
  auto raise_early_opt = app.add_flag("--raise-early-conflicts", raise_early_conflicts,
               "Raise conflict errors before suppressing writes (lazy branching mode only).");

  // Set up option dependencies
  branching_mode_opt->needs(sync_opt);
  include_empty_opt->needs(sync_opt);
  raise_early_opt->needs(branching_mode_opt);

  bool interactive = false;
  app.add_flag("-i,--interactive", interactive,
               "Enable interactive scheduling mode (use command ? for help).");

  bool model_check = false;
  app.add_flag("-e,--explore", model_check,
               "Explore all possible execution paths.");

  try {
    app.parse(argc, argv);

    // Additional validation for logical consistency
    if (sync_model == "linear") {
      if (*branching_mode_opt) {
        std::cerr << "Error: --branching-mode is only valid with --sync branching" << std::endl;
        return 1;
      }
      if (include_empty_commits) {
        std::cerr << "Error: --include-empty-commits is only valid with --sync branching" << std::endl;
        return 1;
      }
      if (raise_early_conflicts) {
        std::cerr << "Error: --raise-early-conflicts is only valid with --sync branching" << std::endl;
        return 1;
      }
    }

    if (sync_model == "branching" && branching_mode == "eager" && raise_early_conflicts) {
      std::cerr << "Error: --raise-early-conflicts is only valid with --branching-mode lazy" << std::endl;
      return 1;
    }
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  try {
    gitmem::verbose::out.enabled = verbose;

    gitmem::verbose::out << "Reading file " << input_path << std::endl;
    if (!std::filesystem::exists(input_path)) {
      std::cerr << "Input file does not exist: " << input_path << std::endl;
      return 1;
    }

    auto reader = gitmem::lang::reader().file(input_path);
    auto result = reader.read();

    if (!result.ok) {
      trieste::logging::Error err;
      result.print_errors(err);
      trieste::logging::Debug() << result.ast;
      return 1;
    }

    if (output_path.empty())
      output_path = input_path.stem().replace_extension(".dot");

    gitmem::verbose::out << "Output will be written to " << output_path << std::endl;

    // Build memory models on demand so each execution path gets a fresh instance.
    auto make_model = [&]() -> std::unique_ptr<gitmem::MemoryModel> {
      if (sync_model == "linear") {
        return std::make_unique<gitmem::linear::LinearMemoryModel>();
      }

      if (branching_mode == "eager") {
        return std::make_unique<gitmem::branching::BranchingEagerMemoryModel>(
          include_empty_commits
        );
      }

      return std::make_unique<gitmem::branching::BranchingLazyMemoryModel>(
        include_empty_commits,
        raise_early_conflicts
      );
    };

    int exit_status;
    wf::push_back(gitmem::lang::wf);
    if (model_check) {
      exit_status = gitmem::model_check(result.ast, output_path, make_model);
    } else if (interactive) {
      exit_status = gitmem::interpret_interactive(result.ast, output_path, make_model);
    } else {
      exit_status = gitmem::interpret(result.ast, output_path, make_model);
    }
    wf::pop_front();

    gitmem::verbose::out << "Execution finished with exit status " << exit_status
                    << std::endl;
    return exit_status;
  } catch (const std::exception &e) {
    std::cerr << "Exception caught: " << e.what() << std::endl;
    return 1;
  }
}
