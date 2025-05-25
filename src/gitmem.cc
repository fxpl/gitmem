#include <CLI/CLI.hpp>

#include "lang.hh"

int main(int argc, char **argv)
{
    using namespace trieste;
    CLI::App app;

    std::filesystem::path input_path;
    app.add_option("input", input_path, "Path to the input file ")->required();

    std::string log_level;
    app.add_option(
           "-l,--log",
           log_level,
           "Set the log level (None, Error, Output, Warn, Info, Debug, Trace).")
        ->check(trieste::logging::set_log_level_from_string);
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

        wf::push_back(gitmem::wf);
        gitmem::interpret(result.ast);
        wf::pop_front();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception caught: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
