# frame_viewer

A C++ image viewer for OpenCV `cv::Mat` frames, built on Qt 5 (5.12 or newer). It supports zooming and panning,
multi-resolution tiled/stitched content, key bindings, mouse events, screen-space overlays and
multiple windows from multiple threads. Qt runs on a hidden UI thread, so your `main()` stays
plain C++.

## Build (Ubuntu 20.04 or 24.04)

```bash
sudo apt install build-essential cmake qtbase5-dev libopencv-dev
cmake -S . -B build
cmake --build build -j
```

Tested with the apt packages of Ubuntu 20.04 (Qt 5.12.8, OpenCV 4.2.0, CMake 3.16, GCC 9) and
24.04 (Qt 5.15.13, OpenCV 4.6.0, CMake 3.28, GCC 13).

## Using it in your project

Add the repo as a subdirectory and link the `frame_viewer` target:

```cmake
set(FRAME_VIEWER_BUILD_DEMOS OFF) # optional: skip the demos
set(FRAME_VIEWER_BUILD_TESTS OFF) # optional: skip the tests
add_subdirectory(frame_viewer)
target_link_libraries(my_app PRIVATE frame_viewer)
```

```cpp
#include <fv/viewer.h>

int main() {
    fv::Viewer viewer("camera");
    viewer.show_fps();
    viewer.bind(Qt::Key_R, "Reset", [&] { /* runs on this thread, inside wait_key() */ });

    cv::VideoCapture cap(0);
    cv::Mat frame;
    while (viewer.is_open() && cap.read(frame)) {
        viewer.show(frame);
        if (viewer.wait_key() == Qt::Key_Space) { /* unbound keys are returned */ }
    }
}
```

The full API is documented in `include/fv/viewer.h`.

For frame-attached boxes with labels, submit them with the image:

```cpp
fv::Box box;
box.rect = cv::Rect2d(100, 80, 200, 150); // image pixel coordinates
box.label = "box 1";
viewer.show(frame, {box}); // or show_shared(frame, {box}) with a pooled buffer
```

Each submission replaces the previous boxes; `show(frame)` clears them. Images and boxes stay
synchronized, and outlines and labels keep their screen size during zoom/pan. For persistent
annotations independent of frame updates, use `set_overlay_box()` / `set_overlay_polygon()` /
`set_overlay_text()`. Overlay boxes take the same `fv::Box` in world coordinates, so they can label
regions spanning several `set_frame()` frames:

```cpp
viewer.set_overlay_box("target_7", {world_rect, colour, 2, "7"});
viewer.remove_overlay("target_7");
```

To change frames and overlays together, wrap the calls in `batch()`. No paint shows only some
of the changes. Inside a batch, `show()` / `set_frame()` still copy when called, so you can reuse
your buffer right away. Getters such as `current_viewport()` return the state from before the
batch until it ends.

```cpp
viewer.batch([&] {
    viewer.set_frame_shared(0, buf, world_rect);
    viewer.set_overlay_box("target_7", {world_rect, colour, 2, "7"});
});
```

`show_fps()` draws a readout of `stats()`: submitted / drawn / replaced rates, and paint time and
latency. Call `stats()` to use the numbers yourself. For example, pass a BGR colour to
`set_overlay_text()` to show a warning line when frames are being replaced (see `demo_simple`).

Frames placed with `set_frame()` can be rotated about their rect centre (degrees, clockwise on
screen). Frames below stay visible around a rotated frame:

```cpp
viewer.set_frame(id, image, cv::Rect2d(100, 50, 640, 480), /*layer=*/1, /*angle_deg=*/30.0);
```

Built-in controls in every window: mouse wheel to zoom at the cursor, Ctrl + left drag to pan,
Ctrl `+` / Ctrl `-` to zoom, Ctrl+0 to fit, Ctrl+H for help, Esc twice to close.

## Rules worth knowing

- **Threads.** Every `fv::Viewer` method is safe to call from any thread. Callbacks run on the
  thread that calls `wait_key()` / `wait_until_closed()`, inside that call, so they never race
  with your loop but are delayed if you stop calling it. The window stays responsive regardless.
- **Frame ownership.** `show()` / `set_frame()` copy the image. `show_shared()` /
  `set_frame_shared()` don't copy and never wait, but the viewer keeps a reference to your
  buffer, so don't write into it afterwards. Take each new buffer from a `fv::BufferPool`:

  ```cpp
  fv::BufferPool pool({5120, 2880}, CV_8UC3);
  while (viewer.is_open()) {
      cv::Mat buf = pool.acquire(); // never one the viewer is using
      run_algorithm_into(buf);
      viewer.show_shared(buf);
      viewer.wait_key();
  }
  ```

- **Lifetime.** Destroy viewers before `main()` returns; don't keep one in a global or static.
  Objects that call into a viewer from other threads must be destroyed first, so declare them
  after it.
- **Qt.** The viewer must be the first thing in the process to use Qt.
- **Platforms.** Linux and Windows. Not macOS (Qt's UI must run on the main thread there).
- **Performance.** Cost scales with the window size, not the frame size. Frames are never
  queued: the newest is drawn and older ones are skipped. Continuously changing frames (video)
  are shrunk with a fast method that can alias fine detail; the last frame is redrawn in full
  quality about half a second after the stream stops. Rotated frames take several times longer
  to draw than unrotated ones.

## Demos

| Executable | Shows |
| --- | --- |
| `build/bin/demo_simple` | `imshow`-style loop, stats readout and coloured `stats()` line, zoom and pan, auto-fit on resize |
| `build/bin/demo_keys` | `bind()`, keys returned by `wait_key()`, mouse clicks, viewport callback |
| `build/bin/demo_multi` | two threads, each with its own window |
| `build/bin/demo_mosaic` | stitched multi-resolution mosaic: low-res overview, sharper tiles where visible |
| `build/bin/demo_big_frame` | streaming large frames with `show_shared()` + `fv::BufferPool` (`--size 4k\|5k\|8k\|WxH`, `--fps N`, `--copy`) |
| `build/bin/demo_roi` | draw, move and resize ROI boxes with `set_overlay_box()` |
| `build/bin/demo_polygon` | click polygons with `set_overlay_polygon()`, save them as a bitmask (Ctrl+S) |
| `build/bin/demo_canvas` | drag and rotate (Q/E) images and shapes on a canvas with `move_frame()` |
| `build/bin/demo_boxes` | moving frame-attached boxes and labels, plus overlay boxes across the image edge; B toggles boxes, L toggles labels |

## WSL

Set `LIBGL_ALWAYS_SOFTWARE=1` (e.g. in `~/.bashrc`). The viewer doesn't use OpenGL, but Qt
checks for it at startup. Without this setting, Mesa prints harmless EGL/ZINK warnings. If Mesa
can reach the GPU through `/dev/dxg` (d3d12), closing the viewer can also take ~5 s and end in
a segfault.

Qt 5 uses X11 by default, and on WSLg (XWayland) the first mouse-wheel notch after moving the
mouse is lost. Use Qt's native Wayland backend instead, with X11 as the fallback:

```bash
sudo apt install qtwayland5
export QT_QPA_PLATFORM="wayland;xcb"   # e.g. in ~/.bashrc
```

If Qt prints `Could not find the Qt platform plugin "wayland"`, `qtwayland5` isn't installed and
Qt has fallen back to X11. Avoid `QT_WAYLAND_CLIENT_BUFFER_INTEGRATION=none` (drops the title bar).

Working on the library itself? See [DEVELOPMENT.md](DEVELOPMENT.md).
