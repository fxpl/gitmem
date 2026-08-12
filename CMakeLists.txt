cmake_minimum_required(VERSION 3.14.0)
project(gitmem VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

FetchContent_Declare(
  trieste
  GIT_REPOSITORY https://github.com/microsoft/Trieste
  GIT_TAG main
  )

FetchContent_MakeAvailable(trieste)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Enable sanitizers option
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(ENABLE_SANITIZERS)
  set(SANITIZER_FLAGS
    -fsanitize=address,undefined
    -fsanitize-recover=all
    -fno-omit-frame-pointer
  )
  add_compile_options(${SANITIZER_FLAGS})
  add_link_options(${SANITIZER_FLAGS})
  message(STATUS "Sanitizers enabled: ASAN, UBSAN")
endif()

include_directories(src)

add_executable(gitmem
  src/gitmem.cc
  src/reader.cc
  src/parser.cc
  src/execution_state.cc
  src/passes/expressions.cc
  src/passes/statements.cc
  src/passes/check_refs.cc
  src/passes/hoist_volatiles.cc
  src/passes/branching.cc
  src/linear/memory_model.cc
  src/linear/version_store.cc
  src/branching/base_memory_model.cc
  src/branching/base_version_store.cc
  src/branching/lazy/version_store.cc
  src/branching/eager/version_store.cc
  src/interpreter.cc
  src/debugger.cc
  src/model_checker.cc
  src/graphviz.cc
  src/tikz.cc
)

add_executable(gitmem_trieste
  src/gitmem_trieste.cc
  src/reader.cc
  src/parser.cc
  src/passes/expressions.cc
  src/passes/statements.cc
  src/passes/check_refs.cc
  src/passes/hoist_volatiles.cc
  src/passes/branching.cc
)

target_link_libraries(gitmem
  CLI11::CLI11
  trieste::trieste
)

target_link_libraries(gitmem_trieste
  CLI11::CLI11
  trieste::trieste
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(run_gitmem_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test_gitmem.py --gitmem $<TARGET_FILE:gitmem>
    DEPENDS gitmem
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running gitmem functional tests"
)

enable_testing()

add_test(
    NAME gitmem_functional_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test_gitmem.py --gitmem $<TARGET_FILE:gitmem>
)
