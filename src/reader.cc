#include "internal.hh"

namespace gitmem {

using namespace trieste;

Reader reader()
  {
    return {
      "gitmem",
      {
        expressions(),
        statements(),
        check_refs(),
        branching(),
      },
      gitmem::parser(),
    };
  }

}
