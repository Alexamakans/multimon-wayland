#pragma once
#include <GL/gl.h>

struct CaptureFrame {
  GLuint texture = 0;
  int width = 0;
  int height = 0;
};

// Initialize Wayland + wlroots screencopy using DMA-BUF (fast path).
// If outputNameOptional is nullptr, captures the first active output.
void wlr_dmabuf_capture_init(const char* outputNameOptional, int* outW, int* outH);

// Fetch next frame (blocks until compositor writes).
// Zero-copy: returned texture is backed by an EGLImage import of the dmabuf.
CaptureFrame wlr_dmabuf_next_frame();

// Cleanup
void wlr_dmabuf_capture_shutdown();

