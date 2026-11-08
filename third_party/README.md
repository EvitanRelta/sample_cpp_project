# Third-party

Contains code that originated elsewhere (e.g. libraries).

## Libraries

- `frame_viewer` - Qt 5 viewer for OpenCV frames, with zoom/pan, overlays and multi-window support.
  Vendored as regular files (not a submodule); use the `frame_viewer` CMake target directly.
  Its standalone demos and tests are disabled in this project.

    Repo link: https://gitlab.com/evitanrelta/frame_viewer

    Vendored revision: `b0ce534c0e50f609431726e5b10dee921098cfa6`

    Docs: [README](frame_viewer/README.md), [development notes](frame_viewer/DEVELOPMENT.md),
    [API reference](frame_viewer/include/fv/viewer.h).

## Header-only libraries

Header-only libraries go directly into `include/` dir.

- `CLI11` v2.6.2 - Command line parser for C++11 \
  https://github.com/CLIUtils/CLI11
