#pragma once

#include <trieste/trieste.h>
#include "memory_model.hh"

namespace gitmem {
  int interpret_interactive(const trieste::Node,
                            const std::filesystem::path &output_file,
                            const MemoryModelFactory& make_model);
}