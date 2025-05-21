#include <trieste/driver.h>
#include "reader.cc"


int main(int argc, char** argv)
{
  return trieste::Driver(grunq::reader()).run(argc, argv);
}
