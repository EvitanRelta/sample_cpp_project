# Developing frame_viewer

## Build and test

The default build type is `RelWithDebInfo`. Use `-DCMAKE_BUILD_TYPE=Debug` to also get a warning
when `show_shared()` is passed the buffer that's currently displayed.

`build/bin/test_show_shared` (needs a display) checks that `show_shared()` doesn't copy or wait
and that `fv::BufferPool` never hands out a buffer in use. Exit code 0 means every check passed;
options are in the file header.

`QT_QPA_PLATFORM=offscreen build/bin/test_frame_boxes` needs no display. It checks box/label/fill
rendering (frame-attached and overlay boxes), zoom/pan, clipping, replacement, matching image/box
snapshots during concurrent submissions, rotated-frame coverage, `stats()` and overlay text
colour. Use `demo_boxes` and `demo_canvas` for interactive checks.

`QT_QPA_PLATFORM=offscreen build/bin/test_batch` needs no display. It checks that paints never
mix two `batch()`es, getters see pre-batch state, nesting and exceptions, and that batched
submissions count in `stats()` when the batch ends.

## Layout

```
include/fv/viewer.h     public API; threading model and coordinates documented at the top
src/viewer.cpp          API implementation (thread-safe facade)
src/buffer_pool.cpp     fv::BufferPool
src/viewer_state.h      state shared between caller threads and the UI thread
src/ui_thread.*         hidden Qt thread, startup and clean shutdown at exit
src/view_widget.*       the window: input, camera, painting, frame boxes, overlays, help
src/compositor.*        frames + camera -> screen image (pure OpenCV); live vs static scaling;
                        rotated frames in draw_rotated_frame() (warp + coverage mask)
src/key_utils.*         key normalisation and parsing
demos/                  one executable per use case
tests/                  test_show_shared, test_frame_boxes, test_batch
```

## Constraints

- Qt 5.12 is the oldest supported Qt (Ubuntu 20.04): no `QImage::Format_BGR888` or
  `QWheelEvent::position()`, and key combinations are plain `int`s (see `fv::Key`).
- No `Q_OBJECT` / moc. Qt Widgets stays private; the public header uses only Qt Core/Gui types.
- `compositor.*` must not depend on Qt.
- One `ViewerState::mutex` guards frames, overlays and the camera. `batch()` (staged per thread
  in `viewer.cpp`, applied by `apply_changes()`) and the paint snapshot rely on it being one lock.
- `BufferPool::is_free` relies on acquire ordering of the `cv::Mat` refcount (matters on ARM).
