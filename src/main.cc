#include "reader.cc"
#include <trieste/driver.h>

int main(int argc, char **argv) {
  return trieste::Driver(grunq::reader()).run(argc, argv);
}
