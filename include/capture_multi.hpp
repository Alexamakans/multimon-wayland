#pragma once
#include <string>
#include <vector>
#include <GL/gl.h>

struct CapturedOutput {
  // Wayland side
  void* wl_output = nullptr;     // opaque wl_output*
  // Image/GL
  int width = 0, height = 0;
  int x = 0, y = 0;
  GLuint texture = 0;            // GL texture bound to an EGLImage
  // Metadata (optional)
  std::string name;              // wl_output.name if available
};

void wlr_multi_capture_init(std::vector<CapturedOutput>& outs, int* totalW, int* totalH);     // discover outputs, create textures
void wlr_multi_next_frame(std::vector<CapturedOutput>& outs); // update all textures
void wlr_multi_shutdown();   // free resources

