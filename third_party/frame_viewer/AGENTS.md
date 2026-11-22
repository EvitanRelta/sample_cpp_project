# AGENTS.md

C++17 static library `frame_viewer` (namespace `fv`): an OpenCV `cv::Mat` viewer on Qt 5. Qt
runs on a hidden UI thread owned by the library; callers stay plain C++. Public API and its
reference docs: `include/fv/viewer.h`. User docs: `README.md`; dev notes: `DEVELOPMENT.md`.

## Build

`cmake -S . -B build && cmake --build build -j` (Ubuntu 20.04: Qt 5.12.8, OpenCV 4.2, GCC 9;
Ubuntu 24.04: Qt 5.15.13, OpenCV 4.6, GCC 13). Code must build on Qt 5.12, the oldest supported.
Must stay warning-free under `-Wall -Wextra`. Executables land in `build/bin/`.

## Layout

```
include/fv/viewer.h     public API; threading model, coordinates, controls at the top
src/viewer.cpp          API implementation (thread-safe facade over ViewerState)
src/buffer_pool.cpp     fv::BufferPool
src/viewer_state.h      state shared between caller threads and the UI thread
src/ui_thread.*         hidden QApplication thread
src/view_widget.*       the window: input, camera, painting, overlays, help
src/compositor.*        frames + camera -> screen image (pure OpenCV)
src/key_utils.*         key normalisation and parsing
demos/, tests/          one executable per use case; test_show_shared, test_frame_boxes, test_batch
```

`test_show_shared` needs a display; run `test_frame_boxes` and `test_batch` with `QT_QPA_PLATFORM=offscreen`.

## Invariants

- Every `fv::Viewer` method is safe from any thread. Callbacks run on the caller's thread inside
  `wait_key()` / `wait_until_closed()`, never on the UI thread.
- `show()` / `set_frame()` copy; `*_shared()` never copy or wait, and hold a `cv::Mat` reference
  instead. `BufferPool` hands out only buffers with refcount 1.
- Frames are never queued: newest wins. Paint cost scales with window size, not frame size.
- No `Q_OBJECT` / moc. Qt Widgets stays private; `compositor.*` has no Qt dependency.
- Linux and Windows only (not macOS).

## Docs

- Keep `viewer.h` comments in sync with behaviour; they are the API reference.
- README is user-facing; internals go in `DEVELOPMENT.md` or code comments.

## Formatting

Once you're done editing CMake or C/C++ files, format them by running:

```sh
cmake-format -i dir1/CMakeLists.txt dir2/CMakeLists.txt
clang-format -i dir1/file.cpp dir2/file.cpp
```

Assume these 2 formatting tools are present, no need to check them before using.
If those tools are not present, skip formatting.
