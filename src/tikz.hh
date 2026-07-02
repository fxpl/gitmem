#pragma once
#include "graph.hh"
#include <string>
#include <filesystem>

namespace gitmem {
namespace graph {

struct TikzPrinter {
  void print(const ExecutionGraph& g, const std::filesystem::path& path,
             bool linear_mode = false);
};

} // namespace graph
} // namespace gitmem
