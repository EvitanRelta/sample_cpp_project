# Developing frame_viewer

## Build and test

The default build type is `RelWithDebInfo`. Use `-DCMAKE_BUILD_TYPE=Debug` to also get a warning
when `show_shared()` is passed the buffer that's currently displayed.

`build/bin/test_show_shared` (needs a display) checks that `show_shared()` doesn't copy or wait
and that `fv::BufferPool` never hands out a buffer in use. Exit code 0 means every check passed;
options are in the file header. There is no other automated test: check behaviour changes with
the relevant demo (see the README table).

## Layout

```
include/fv/viewer.h     public API; threading model and coordinates documented at the top
src/viewer.cpp          API implementation (thread-safe facade)
src/buffer_pool.cpp     fv::BufferPool
src/viewer_state.h      state shared between caller threads and the UI thread
src/ui_thread.*         hidden Qt thread, startup and clean shutdown at exit
src/view_widget.*       the window: input, camera, painting, overlays, help
src/compositor.*        frames + camera -> screen image (pure OpenCV); live vs static scaling
src/key_utils.*         key normalisation and parsing
demos/                  one executable per use case
tests/                  test_show_shared
```

## Constraints

- Qt 5.12 is the oldest supported Qt (Ubuntu 20.04): no `QImage::Format_BGR888` or
  `QWheelEvent::position()`, and key combinations are plain `int`s (see `fv::Key`).
- No `Q_OBJECT` / moc. Qt Widgets stays private; the public header uses only Qt Core/Gui types.
- `compositor.*` must not depend on Qt.
- `BufferPool::is_free` relies on acquire ordering of the `cv::Mat` refcount (matters on ARM).
