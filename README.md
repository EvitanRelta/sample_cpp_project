# C++20 Project Sample

This repository is a copy-and-adapt sample for a small C++20 application. Rename the sample
identifiers and replace the greeting code when starting a real project; it is not intended to be
used unchanged.

## Layout

- `src/main.cpp` defines the main application, `sample_cpp_project`.
- All other headers and `.cpp` files under `src/` form the STATIC library
  `sample_cpp_project_lib`.
- Each `.cpp` file found recursively under `tools/` becomes a separate executable and links
  `sample_cpp_project_lib`.
- `tools/CMakeLists.txt` discovers and configures those tool executables.
- `third_party/include/CLI11.hpp` contains the vendored CLI11 v2.6.2 header used by the main
  application.
- `CMakePresets.json` provides Linux/GCC-compatible and Windows/MSVC debug presets using Ninja.
- `.vscode.sample/` contains optional Linux/GDB and Windows/MSVC editor tasks and launch settings.

The project library is linked into the final executables. It is not a runtime file that must be
distributed beside them.

## Linux

From the project root, configure and build the debug preset:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
```

Run the application:

```sh
./build/bin/sample_cpp_project --name Ada
```

The Linux and Windows presets both use `build/`. When switching platforms in the same checkout,
delete `build/` before configuring the other preset because an existing CMake build tree cannot be
reused with a different platform or compiler.

## Visual Studio 2022 Toolchain

On Windows, run the following from a regular Command Prompt:

```bat
build_vs2022.bat
```

The script locates Visual Studio 2022, initializes its x64 compiler environment, configures the
`windows-msvc-debug` Ninja preset, and builds `build\bin\sample_cpp_project.exe`. The CMake targets
use the static MSVC runtime, CLI11 is header-only, and `sample_cpp_project_lib` is a static library,
so no project DLL or separate project-library file is needed beside the executable. Normal Windows
system components are still provided by the target machine.

## VS Code

Copy the sample directory before opening the project in VS Code:

```sh
cp -r .vscode.sample .vscode
```

On Windows Command Prompt, the equivalent is:

```bat
xcopy .vscode.sample .vscode\ /E /I
```

The copied configuration supplies configure, build, and debug entries for Linux with GDB and
Windows with the Visual Studio debugger. If the application target or output path changes, update
the `program` entries in `.vscode/launch.json` and the related preset names in
`.vscode/tasks.json`.

## Tools

Tools are enabled by default. Every recursively discovered `tools/*.cpp` file must define exactly
one `main()`. Relative paths are flattened with underscores for internal CMake target names, and
paths that flatten to the same target name are rejected during configuration. Output paths retain
the source layout: `tools/sample_tool.cpp` produces `build/tools/sample_tool`, while
`tools/admin/check.cpp` produces `build/tools/admin/check` (with `.exe` on Windows).

Disable all tool executables while configuring with:

```sh
cmake --preset linux-debug -DSAMPLE_CPP_PROJECT_BUILD_TOOLS=OFF
```

Use the same option with `windows-msvc-debug` on Windows.

## Formatting

Run the configured formatters from the project root:

```sh
find src -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +
find tools -type f -name '*.cpp' -exec clang-format -i {} +
cmake-format -i CMakeLists.txt tools/CMakeLists.txt
prettier --write README.md CMakePresets.json .vscode.sample/*.json
```

## Adaptation Checklist

1. Search for `sample_cpp_project` and replace the CMake project name, application target,
   `sample_cpp_project_lib`, `sample_cpp_project_configure_target`, tool-target prefix, source
   namespace, batch output name, and VS Code launch paths as appropriate.
2. Search for the uppercase prefix `SAMPLE_CPP_PROJECT`. Rename
   `SAMPLE_CPP_PROJECT_BUILD_TOOLS`, `SAMPLE_CPP_PROJECT_LIBRARY_SOURCES`, and
   `SAMPLE_CPP_PROJECT_TOOL_SOURCES` consistently.
3. Replace `src/greeting.h`, `src/greeting.cpp`, `make_greeting`, and the
   `sample_cpp_project` namespace with the real library API and implementation. Keep
   `src/main.cpp` outside the static library.
4. Update the CLI description, `-n,--name` option, default value, and application behavior in
   `src/main.cpp`. Update or remove `tools/sample_tool.cpp` for the tools the project needs.
5. Rename the `linux-debug` and `windows-msvc-debug` presets if desired, then update matching names
   in `build_vs2022.bat` and `.vscode.sample/tasks.json`.
6. If output directories or executable names change, update `build_vs2022.bat` and both `program`
   paths in `.vscode.sample/launch.json`.
7. Keep `third_party/include/CLI11.hpp` and its include path if CLI11 is still used. If the
   `src/`, `tools/`, or `third_party/include/` layout changes, update the corresponding paths in
   the CMake files.
