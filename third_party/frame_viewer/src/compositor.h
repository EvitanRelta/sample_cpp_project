/**
 * Turns frames + camera into a screen image; pure OpenCV with no Qt, so it can be used without
 * a window.
 */
#pragma once

#include <vector>
#include "viewer_state.h"

namespace fv::detail {

/**
 * Draw the visible part of every frame into `canvas` (CV_8UC3, already sized to the window), in
 * layer order. Only the visible region of each frame is read, so cost scales with the window size
 * rather than the frame size.
 *
 * Shrinking uses one of two methods:
 * - static frames: area averaging from cached power-of-two levels (best quality; the cache is
 *   built once and reused while the frame is unchanged)
 * - live frames (replaced repeatedly): linear sampling straight from the source (much faster,
 *   slight aliasing on fine detail, nothing cached)
 *
 * @return true if any frame used the fast method, so the caller can repaint in high quality once
 * the frame stops changing
 */
bool compose(cv::Mat& canvas,
             std::vector<FrameEntry>& frames,
             const Camera& cam,
             ClockType::time_point now);

/** Return the union of all frames' rotated world bounds (empty rect if there are none). */
cv::Rect2d content_bounds(const std::vector<FrameEntry>& frames);

/**
 * Convert any common Mat type to 8-bit BGR. Return a shallow view when no conversion is needed.
 * 16-bit is scaled to 8-bit; float/double are assumed to be in [0, 1].
 */
cv::Mat to_bgr8(const cv::Mat& in);

}  // namespace fv::detail
