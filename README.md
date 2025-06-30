# gitmem

Experimental interpreter for creating execution diagrams for a new
concurrency model.

## Building and Running

To build you need CMake and Ninja. CMake will fetch any other dependencies.

The following commands should set you up:

```
mkdir build
cd build
cmake -G Ninja .. -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
ninja
```

If you need, set the C standard with `-DCMAKE_CXX_STANDARD=20`.
If you are running a recent version of CMake, you may need
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

You can test if the build was successful by running the following
command in the `build` directory:

```
./gitmem -e ../examples/race_condition.gm
```

The build script creates two executables:

- `gitmem` parses source code and runs the interpreter in order to
  create an execution diagram (work in progress). You can run the
  interpreter interactively with the `-i` flag, and automatically
  explore all possible traces with the `-e` flag (showing failing
  runs).
- `gitmem_trieste` is the default
  [Trieste](https://github.com/microsoft/Trieste) driver which can
  be used to inspect the parsed source code and test the parser.
  Running `gitmem_trieste build foo.gm` will create a file
  `foo.trieste` with the parsed source code as an S-expression.

## VSCode Extension

You should be able to use `Developer: Install Extension from
Location` in the VSCode command palette to install a rudimentary
extension in the `gitmem-extension` directory and get syntax
highlighting..
