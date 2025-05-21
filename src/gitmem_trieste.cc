#include <trieste/driver.h>
#include "lang.hh"

int main(int argc, char** argv)
{
  return trieste::Driver(gitmem::reader()).run(argc, argv);
}
