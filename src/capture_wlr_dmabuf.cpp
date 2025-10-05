#include "capture_wlr_dmabuf.hpp"

#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <cstdio>

// Generated from our XMLs at build time
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

// We need the OES function to bind an EGLImage to a GL texture
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;

struct WLCtx {
  // Wayland
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_output*  output = nullptr;

  // Protocols
  zwlr_screencopy_manager_v1* screencopy = nullptr;
  zwp_linux_dmabuf_v1* linux_dmabuf = nullptr;

  // DRM/GBM for client-allocated dmabuf
  int drm_fd = -1;
  gbm_device* gbm = nullptr;

  // Geometry & format
  int width = 0, height = 0;
  uint32_t fourcc = DRM_FORMAT_XRGB8888; // prefer XRGB on i915
  uint64_t modifier = DRM_FORMAT_MOD_INVALID;

  // GBM BO + dmabuf plane info and wl_buffer
  gbm_bo*   bo = nullptr;
  int       nplanes = 1;
  int       fds[4] = {-1,-1,-1,-1};
  uint32_t  strides[4] = {0};
  uint32_t  offsets[4] = {0};
  wl_buffer* wlbuf = nullptr;

  // EGL/GL
  EGLDisplay egl_dpy = EGL_NO_DISPLAY;
  EGLImageKHR egl_img = EGL_NO_IMAGE_KHR;
  GLuint tex = 0;

  // State
  bool got_linux_dmabuf = false;
  bool frame_ready = false;
} static G;

// --- Helpers ---
static void ensure_gl_egl_image_fn() {
  if (!glEGLImageTargetTexture2DOES_) {
    glEGLImageTargetTexture2DOES_ =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!glEGLImageTargetTexture2DOES_)
      throw std::runtime_error("GL_OES_EGL_image not available (glEGLImageTargetTexture2DOES missing)");
  }
}

static int open_render_node() {
  const char* cands[] = {
    "/dev/dri/renderD128",
    "/dev/dri/renderD129",
    "/dev/dri/renderD130"
  };
  for (const char* p : cands) {
    int fd = open(p, O_RDWR | O_CLOEXEC);
    if (fd >= 0) return fd;
  }
  throw std::runtime_error("Failed to open /dev/dri/renderD* (render node)");
}

static EGLDisplay current_egl_display_or_throw() {
  EGLDisplay d = eglGetCurrentDisplay();
  if (d == EGL_NO_DISPLAY) throw std::runtime_error("No current EGLDisplay (GLFW must use EGL)");
  return d;
}

// --- Registry ---
static void reg_global(void*, wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
  (void)ver;
  if (strcmp(iface, wl_output_interface.name) == 0) {
    if (!G.output) G.output = (wl_output*)wl_registry_bind(reg, name, &wl_output_interface, 2);
  } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
    G.screencopy = (zwlr_screencopy_manager_v1*)
      wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 3);
  } else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) {
    G.linux_dmabuf = (zwp_linux_dmabuf_v1*)
      wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 4);
  }
}
static void reg_remove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener REG_LST = { reg_global, reg_remove };

// --- Screencopy frame callbacks ---
static void frame_buffer(void*, zwlr_screencopy_frame_v1*,
                         uint32_t /*fmt*/, uint32_t w, uint32_t h, uint32_t /*stride*/) {
  G.width = (int)w; G.height = (int)h;
}
static void frame_flags(void*, zwlr_screencopy_frame_v1*, uint32_t /*flags*/) {}
static void frame_ready(void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {
  G.frame_ready = true;
}
static void frame_failed(void*, zwlr_screencopy_frame_v1*) {
  throw std::runtime_error("screencopy failed");
}
static void frame_damage(void*, zwlr_screencopy_frame_v1*, int32_t, int32_t, int32_t, int32_t) {}
static void frame_linux_dmabuf(void*, zwlr_screencopy_frame_v1*, uint32_t format,
                               uint32_t width, uint32_t height) {
  // Prefer what compositor tells us, but default stays XRGB8888
  G.fourcc = format ? format : DRM_FORMAT_XRGB8888;
  G.width  = (int)width;
  G.height = (int)height;
  G.got_linux_dmabuf = true;
}
static const zwlr_screencopy_frame_v1_listener FRAME_LST = {
  frame_buffer, frame_flags, frame_ready, frame_failed, frame_damage, frame_linux_dmabuf
};

// --- Create client dmabuf (GBM) + wl_buffer via zwp_linux_dmabuf_v1 ---
static void try_make_gbm_bo_with(uint32_t fmt, uint32_t use) {
  G.bo = gbm_bo_create(G.gbm, G.width, G.height, fmt, use);
  if (G.bo) return;

  char fmtc[5] = {0};
  *(uint32_t*)fmtc = fmt;
  std::fprintf(stderr,
    "gbm_bo_create failed (w=%d h=%d fmt=%s(0x%x) use=0x%x); trying fallback…\n",
    G.width, G.height, fmtc, fmt, use);
}

static void create_gbm_wlbuffer() {
  if (G.width <= 0 || G.height <= 0)
    throw std::runtime_error("invalid size before GBM allocation");

  if (G.drm_fd < 0) G.drm_fd = open_render_node();
  if (!G.gbm) G.gbm = gbm_create_device(G.drm_fd);
  if (!G.gbm) throw std::runtime_error("gbm_create_device failed");

  const uint32_t fmt = G.fourcc ? G.fourcc : DRM_FORMAT_XRGB8888;

  // Allocation fallback ladder (most permissive first for i915):
  //  1) RENDERING only
  //  2) no flags
  //  3) LINEAR (as last resort; often fails for large sizes on i915)
  try_make_gbm_bo_with(fmt, GBM_BO_USE_RENDERING);
  if (!G.bo) try_make_gbm_bo_with(fmt, 0);
  if (!G.bo) try_make_gbm_bo_with(fmt, GBM_BO_USE_LINEAR);

  if (!G.bo) throw std::runtime_error("gbm_bo_create failed (all attempts)");

  G.nplanes = 1;
  G.fds[0]      = gbm_bo_get_fd(G.bo);
  if (G.fds[0] < 0) throw std::runtime_error("gbm_bo_get_fd failed");

  G.strides[0]  = gbm_bo_get_stride(G.bo);
  G.offsets[0]  = 0;

  if (!G.linux_dmabuf) throw std::runtime_error("zwp_linux_dmabuf_v1 missing");

  zwp_linux_dmabuf_params_v1* params = zwp_linux_dmabuf_v1_create_params(G.linux_dmabuf);
  if (!params) throw std::runtime_error("zwp_linux_dmabuf_v1_create_params failed");

  // plane-index=0, modifier split into hi/lo (INVALID -> linear/driver default)
  zwp_linux_dmabuf_params_v1_add(params, G.fds[0], 0,
                                 G.offsets[0], G.strides[0],
                                 (uint32_t)(G.modifier >> 32),
                                 (uint32_t)(G.modifier & 0xffffffff));

  G.wlbuf = zwp_linux_dmabuf_params_v1_create_immed(params, G.width, G.height, fmt, 0);
  zwp_linux_dmabuf_params_v1_destroy(params);
  if (!G.wlbuf) throw std::runtime_error("dmabuf create_immed failed");
}

// --- Create EGLImage from the dmabuf and bind to GL texture ---
static void ensure_egl_image_and_texture() {
  if (G.tex == 0) glGenTextures(1, &G.tex);

  if (G.egl_img == EGL_NO_IMAGE_KHR) {
    G.egl_dpy = eglGetCurrentDisplay();
    if (G.egl_dpy == EGL_NO_DISPLAY) throw std::runtime_error("No current EGLDisplay");
    ensure_gl_egl_image_fn();

    // Prefer core eglCreateImage (EGL 1.5) signature with EGLAttrib*
    // Fallback to KHR with EGLint* if needed.
#if defined(EGL_VERSION_1_5)
    const EGLAttrib attrs[] = {
      EGL_WIDTH,                      (EGLAttrib)G.width,
      EGL_HEIGHT,                     (EGLAttrib)G.height,
      EGL_LINUX_DRM_FOURCC_EXT,       (EGLAttrib)G.fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT,      (EGLAttrib)G.fds[0],
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,  (EGLAttrib)G.offsets[0],
      EGL_DMA_BUF_PLANE0_PITCH_EXT,   (EGLAttrib)G.strides[0],
      EGL_NONE
    };
    G.egl_img = eglCreateImage(
      G.egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
      (EGLClientBuffer)nullptr, attrs);
#else
    const EGLint attrs[] = {
      EGL_WIDTH,                      (EGLint)G.width,
      EGL_HEIGHT,                     (EGLint)G.height,
      EGL_LINUX_DRM_FOURCC_EXT,       (EGLint)G.fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT,      (EGLint)G.fds[0],
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,  (EGLint)G.offsets[0],
      EGL_DMA_BUF_PLANE0_PITCH_EXT,   (EGLint)G.strides[0],
      EGL_NONE
    };
# ifndef EGL_EGLEXT_PROTOTYPES
#  define EGL_EGLEXT_PROTOTYPES 1
# endif
    G.egl_img = eglCreateImageKHR(
      G.egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
      (EGLClientBuffer)nullptr, attrs);
#endif

    if (G.egl_img == EGL_NO_IMAGE_KHR)
      throw std::runtime_error("eglCreateImage(EGL_LINUX_DMA_BUF_EXT) failed");
  }

  glBindTexture(GL_TEXTURE_2D, G.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, G.egl_img);
}

// --- Public API ---
void wlr_dmabuf_capture_init(const char* /*outputNameOptional*/, int* outW, int* outH) {
  G.display = wl_display_connect(nullptr);
  if (!G.display) throw std::runtime_error("wl_display_connect failed");

  G.registry = wl_display_get_registry(G.display);
  wl_registry_add_listener(G.registry, &REG_LST, nullptr);
  wl_display_roundtrip(G.display);

  if (!G.screencopy)
    throw std::runtime_error("zwlr_screencopy_manager_v1 missing");
  if (!G.linux_dmabuf)
    throw std::runtime_error("zwp_linux_dmabuf_v1 missing");

  // Probe first frame to learn size+format via linux_dmabuf
  zwlr_screencopy_frame_v1* f =
      zwlr_screencopy_manager_v1_capture_output(G.screencopy, 0, G.output);
  zwlr_screencopy_frame_v1_add_listener(f, &FRAME_LST, nullptr);

  while (wl_display_dispatch(G.display) != -1 && !G.got_linux_dmabuf) {}
  if (G.width <= 0 || G.height <= 0)
    throw std::runtime_error("invalid w/h from screencopy probe");

  create_gbm_wlbuffer();

  // Copy into our dma-buf and wait for ready
  zwlr_screencopy_frame_v1_copy(f, G.wlbuf);
  while (wl_display_dispatch(G.display) != -1 && !G.frame_ready) {}
  zwlr_screencopy_frame_v1_destroy(f);
  G.frame_ready = false;

  ensure_egl_image_and_texture();

  if (outW) *outW = G.width;
  if (outH) *outH = G.height;
}

CaptureFrame wlr_dmabuf_next_frame() {
  // Request next frame into same wl_buffer (same dmabuf)
  zwlr_screencopy_frame_v1* f =
      zwlr_screencopy_manager_v1_capture_output(G.screencopy, 0, G.output);
  zwlr_screencopy_frame_v1_add_listener(f, &FRAME_LST, nullptr);
  zwlr_screencopy_frame_v1_copy(f, G.wlbuf);

  while (wl_display_dispatch(G.display) != -1 && !G.frame_ready) {}
  G.frame_ready = false;
  zwlr_screencopy_frame_v1_destroy(f);

  // No glTex(Sub)Image calls here: texture already sources from EGLImage.
  glBindTexture(GL_TEXTURE_2D, G.tex);

  CaptureFrame cf; cf.texture = G.tex; cf.width = G.width; cf.height = G.height;
  return cf;
}

void wlr_dmabuf_capture_shutdown() {
#if defined(EGL_VERSION_1_5)
  if (G.egl_img != EGL_NO_IMAGE_KHR) { eglDestroyImage(G.egl_dpy, G.egl_img); G.egl_img = EGL_NO_IMAGE_KHR; }
#else
  if (G.egl_img != EGL_NO_IMAGE_KHR) { eglDestroyImageKHR(G.egl_dpy, G.egl_img); G.egl_img = EGL_NO_IMAGE_KHR; }
#endif
  if (G.tex)  { glDeleteTextures(1, &G.tex); G.tex = 0; }
  if (G.wlbuf){ wl_buffer_destroy(G.wlbuf); G.wlbuf = nullptr; }
  for (int i=0;i<4;++i) if (G.fds[i]>=0) { close(G.fds[i]); G.fds[i] = -1; }
  if (G.bo)   { gbm_bo_destroy(G.bo); G.bo = nullptr; }
  if (G.gbm)  { gbm_device_destroy(G.gbm); G.gbm = nullptr; }
  if (G.output)      { wl_output_destroy(G.output); G.output = nullptr; }
  if (G.linux_dmabuf){ zwp_linux_dmabuf_v1_destroy(G.linux_dmabuf); G.linux_dmabuf = nullptr; }
  if (G.screencopy)  { zwlr_screencopy_manager_v1_destroy(G.screencopy); G.screencopy = nullptr; }
  if (G.registry)    { wl_registry_destroy(G.registry); G.registry = nullptr; }
  if (G.display)     { wl_display_disconnect(G.display); G.display = nullptr; }
  if (G.drm_fd >= 0) { close(G.drm_fd); G.drm_fd = -1; }
}

