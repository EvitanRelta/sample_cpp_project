# C++20 Project Sample

This repository is a copy-and-adapt sample for a small C++20 application. Rename the sample
identifiers and replace the greeting code when starting a real project; it is not intended to be
used unchanged.

## Layout

- `src/main.cpp` defines the main application, `sample_cpp_project`.
- All other headers and `.cpp` files under `src/` form the STATIC library `sample_cpp_project_lib`.
- Each `.cpp` file found recursively under `extras/` becomes a separate executable and links
  `sample_cpp_project_lib`.
- `extras/CMakeLists.txt` discovers and configures those extra executables.
- `third_party/include/CLI11.hpp` contains the vendored CLI11 v2.6.2 header used by the main
  application.
- `.vscode.sample/` contains optional VS Code build tasks and GDB launch settings.

The project library is linked into the final executables. It is not a runtime file that must be
distributed beside them.

## Build

From the project root, configure and build (defaults to a Debug build):

```sh
cmake -B build
cmake --build build
```

Run the application:

```sh
./build/bin/sample_cpp_project --name Ada
```

## VS Code

Copy the sample directory before opening the project in VS Code:

```sh
cp -r .vscode.sample .vscode
```

The copied configuration supplies configure, build, and GDB debug entries. If the application
target or output path changes, update the `program` entry in `.vscode/launch.json`.

## Extras

Extras are enabled by default. Every recursively discovered `extras/*.cpp` file must define exactly
one `main()`. Relative paths are flattened with underscores for internal CMake target names, and
paths that flatten to the same target name are rejected during configuration. Output paths retain
the source layout: `extras/sample_extra.cpp` produces `build/extras/sample_extra`, while
`extras/admin/check.cpp` produces `build/extras/admin/check`.

Disable all extra executables while configuring with:

```sh
cmake -B build -DSAMPLE_CPP_PROJECT_BUILD_EXTRAS=OFF
```

## Formatting

Run the configured formatters from the project root:

```sh
find src -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +
find extras -type f -name '*.cpp' -exec clang-format -i {} +
cmake-format -i CMakeLists.txt extras/CMakeLists.txt
prettier --write README.md .vscode.sample/*.json
```

## Adaptation Checklist

1. Search for `sample_cpp_project` and replace the CMake project name, application target,
   `sample_cpp_project_lib`, `sample_cpp_project_configure_target`, extra-target prefix, source
   namespace, and VS Code launch path as appropriate.
2. Search for the uppercase prefix `SAMPLE_CPP_PROJECT`. Rename `SAMPLE_CPP_PROJECT_BUILD_EXTRAS`,
   `SAMPLE_CPP_PROJECT_LIBRARY_SOURCES`, and `SAMPLE_CPP_PROJECT_EXTRA_SOURCES` consistently.
3. Replace `src/greeting.h`, `src/greeting.cpp`, `make_greeting`, and the `sample_cpp_project`
   namespace with the real library API and implementation. Keep `src/main.cpp` outside the static
   library.
4. Update the CLI description, `-n,--name` option, default value, and application behavior in
   `src/main.cpp`. Update or remove `extras/sample_extra.cpp` for the extras the project needs.
5. If output directories or executable names change, update the `program` path in
   `.vscode.sample/launch.json`.
6. Keep `third_party/include/CLI11.hpp` and its include path if CLI11 is still used. If the `src/`,
   `extras/`, or `third_party/include/` layout changes, update the corresponding paths in the CMake
   files.
