#include <CLI/CLI.hpp>

#include "lang.hh"
#include "interpreter.hh"

int main(int argc, char **argv)
{
    using namespace trieste;
    CLI::App app;

    std::filesystem::path input_path;
    app.add_option("input", input_path, "Path to the input file ")->required()->check(CLI::ExistingFile);

    std::filesystem::path output_path = "";
    app.add_option(
        "-o,--output",
        output_path,
        "Path to the output file."
    );

    bool verbose = false;
    app.add_flag(
        "-v,--verbose",
        verbose,
        "Enable verbose output from the interpreter."
    );

    // TODO: These should probably be subcommands
    bool interactive = false;
    app.add_flag(
        "-i,--interactive",
        interactive,
        "Enable interactive scheduling mode (use command ? for help).");

    bool model_check = false;
    app.add_flag(
        "-e,--explore",
        model_check,
        "Explore all possible execution paths.");

    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError &e)
    {
        return app.exit(e);
    }

    try
    {
        gitmem::verbose.enabled = verbose;

        gitmem::verbose << "Reading file " << input_path << std::endl;
        if (!std::filesystem::exists(input_path))
        {
            std::cerr << "Input file does not exist: " << input_path << std::endl;
            return 1;
        }

        auto reader = gitmem::reader().file(input_path);
        auto result = reader.read();

        if (!result.ok)
        {
            trieste::logging::Error err;
            result.print_errors(err);
            trieste::logging::Debug() << result.ast;
            return 1;
        }

        if (output_path.empty())
            output_path = input_path.stem().replace_extension(".dot");

        gitmem::verbose << "Output will be written to " << output_path << std::endl;

        int exit_status;
        wf::push_back(gitmem::wf);
        if (model_check)
        {
            exit_status = gitmem::model_check(result.ast, output_path);
        }
        else if (interactive)
        {
            exit_status = gitmem::interpret_interactive(result.ast, output_path);
        }
        else
        {
            exit_status = gitmem::interpret(result.ast, output_path);
        }
        wf::pop_front();

        gitmem::verbose << "Execution finished with exit status " << exit_status << std::endl;
        return exit_status;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception caught: " << e.what() << std::endl;
        return 1;
    }
}
