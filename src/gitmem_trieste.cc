#include "lang.hh"
#include <trieste/driver.h>

int main(int argc, char **argv) {
  return trieste::Driver(gitmem::lang::reader()).run(argc, argv);
}
