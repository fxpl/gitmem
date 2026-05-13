#include "internal.hh"

namespace gitmem {

namespace lang {

using namespace trieste;

Reader reader() {
  return {
      "gitmem",
      {
          expressions(),
          statements(),
          check_refs(),
          branching(),
      },
      gitmem::lang::parser(),
  };
}

} // namespace lang

} // namespace gitmem