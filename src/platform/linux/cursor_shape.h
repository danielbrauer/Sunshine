/**
 * @file src/platform/linux/cursor_shape.h
 * @brief Declarations for the X11 cursor shape watcher.
 */
#pragma once

// standard includes
#include <memory>

// local includes
#include "src/platform/common.h"

namespace platf::x11 {
  /**
   * @brief Check whether an X11 display with the XFixes extension is reachable.
   */
  bool cursor_shape_supported();

  /**
   * @brief Start the cursor shape watcher thread. Idempotent.
   */
  void cursor_shape_start();

  /**
   * @brief Stop the cursor shape watcher thread and wait for it to exit. Idempotent.
   */
  void cursor_shape_stop();

  /**
   * @brief Get the latest published cursor shape.
   */
  std::shared_ptr<const cursor_shape_t> current_cursor_shape();
}  // namespace platf::x11
