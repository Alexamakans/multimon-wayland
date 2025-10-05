#include <GLFW/glfw3.h>
#include <stdexcept>

static GLFWwindow* gWin = nullptr;

void init_window_and_gl(int width, int height, const char* title) {
  if (!glfwInit()) throw std::runtime_error("glfwInit failed");

  // Force EGL so we have EGLDisplay for EGLImage imports on Wayland
  glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE); // you use fixed-function bits
  glfwWindowHint(GLFW_DOUBLEBUFFER, 1);

  gWin = glfwCreateWindow(width, height, title, nullptr, nullptr);
  if (!gWin) throw std::runtime_error("glfwCreateWindow failed");

  glfwMakeContextCurrent(gWin);
  glfwSwapInterval(0); // no vsync
}

bool window_should_close() { return glfwWindowShouldClose(gWin); }
void window_poll()         { glfwPollEvents(); }
void window_swap()         { glfwSwapBuffers(gWin); }
void window_get_framebuffer_size(int* w, int* h) { glfwGetFramebufferSize(gWin, w, h); }

void shutdown_window() {
  if (gWin) { glfwDestroyWindow(gWin); gWin = nullptr; }
  glfwTerminate();
}

