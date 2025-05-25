cmake_minimum_required(VERSION 3.14.0)
project(gitmem VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

FetchContent_Declare(
  trieste
  GIT_REPOSITORY https://github.com/microsoft/Trieste
  GIT_TAG main
  )

FetchContent_MakeAvailable(trieste)

add_executable(gitmem
  src/gitmem.cc
  src/reader.cc
  src/parser.cc
  src/passes/expressions.cc
  src/passes/statements.cc
  src/passes/check_refs.cc
  src/interpreter.cc
)

add_executable(gitmem_trieste
  src/gitmem_trieste.cc
  src/reader.cc
  src/parser.cc
  src/passes/expressions.cc
  src/passes/statements.cc
  src/passes/check_refs.cc
)

target_link_libraries(gitmem
  CLI11::CLI11
  trieste::trieste
)

target_link_libraries(gitmem_trieste
  CLI11::CLI11
  trieste::trieste
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
