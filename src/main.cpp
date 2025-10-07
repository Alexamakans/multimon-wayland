#include <GL/gl.h>
#include <GL/glu.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <vector>

#include "command_server.hpp"
#include "glasses.hpp"
#include "platform.hpp"
#include "viture.h"

// multi-output capture (no xdg-output)
#include "capture_multi.hpp"

struct MyMonitor {
  int x, y, width, height, index;
};

static float screen_angle_offset_degrees = 0.0f;
static bool center_dot_enabled = true;

static float eye_zoom_mult = 1.0f;
static float angle_deg = 40.0f;

static std::vector<MyMonitor> monitors;
static std::vector<MyMonitor *> focusedmonitors;

// ---- Commands hooked into command_server ----
static void on_align() {
  glasses.oroll = -glasses.roll;
  glasses.opitch = -glasses.pitch;
  glasses.oyaw = -glasses.yaw;
}
static void on_push() {
  for (int i = int(focusedmonitors.size()) - 1; i >= 0; i--)
    if (i == int(focusedmonitors.size()) - 1)
      focusedmonitors.push_back(focusedmonitors[i]);
    else
      focusedmonitors[i + 1] = focusedmonitors[i];
  if (!focusedmonitors.empty())
    focusedmonitors[0] = nullptr;
}
static void on_pop() {
  for (size_t i = 0; i < focusedmonitors.size(); ++i)
    focusedmonitors[i] =
        (i + 1 < focusedmonitors.size() ? focusedmonitors[i + 1] : nullptr);
  if (!focusedmonitors.empty())
    focusedmonitors.pop_back();
}
static void on_zoom_in_fov() { glasses.fov *= 0.95; }
static void on_zoom_out_fov() { glasses.fov *= 1.05; }
static void on_zoom_in() { eye_zoom_mult += 0.05; }
static void on_zoom_out() { eye_zoom_mult -= 0.05; }
static void on_shift_left() { screen_angle_offset_degrees += angle_deg / 2.0f; }
static void on_shift_right() {
  screen_angle_offset_degrees -= angle_deg / 2.0f;
}
static void on_toggle_center_dot() { center_dot_enabled = !center_dot_enabled; }

// ---- Tiny helpers ----
static void draw_filled_center_rect(float half_w, float half_h) {
  int vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  int W = vp[2], H = vp[3];
  float cx = W / 2.f, cy = H / 2.f;

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  {
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glColor3f(1, 0, 0);
    glBegin(GL_QUADS);
    glVertex2f(cx - half_w, cy - half_h);
    glVertex2f(cx + half_w, cy - half_h);
    glVertex2f(cx + half_w, cy + half_h);
    glVertex2f(cx - half_w, cy + half_h);
    glEnd();
    glEnable(GL_DEPTH_TEST);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
  }
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glColor3f(1, 1, 1);
  glEnable(GL_TEXTURE_2D);
}

static void getLookVector(float &dx, float &dy, float &dz) {
  float pitchRad = get_pitch(glasses) * M_PI / 180.0;
  float yawRad = get_yaw(glasses) * M_PI / 180.0;
  dx = std::sin(yawRad) * -std::cos(pitchRad);
  dy = -std::sin(pitchRad);
  dz = -std::cos(yawRad) * std::cos(pitchRad);
}

static bool initGL() {
  glEnable(GL_TEXTURE_2D);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  int w = 0, h = 0;
  window_get_framebuffer_size(&w, &h);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(60.0, w > 0 ? double(w) / double(h) : 16.0 / 9.0, 0.1, 1000.0);
  glMatrixMode(GL_MODELVIEW);
  return true;
}

static void getMonitorUVs(const MyMonitor &m, int fbW, int fbH, float &u0,
                          float &v0, float &u1, float &v1) {
  // u0 = float(m.x) / fbW;
  // v0 = float(m.y) / fbH;
  // u1 = float(m.x + m.width) / fbW;
  // v1 = float(m.y + m.height) / fbH;
  u0 = 0.0f;
  v0 = 0.0f;
  u1 = 1.0f;
  v1 = 1.0f;
}

// We treat each output as a separate texture; when drawing quads we bind the
// needed texture.
static void drawTexturedQuad(GLuint tex, float u0, float v0, float u1, float v1,
                             float w, float h) {
  glBindTexture(GL_TEXTURE_2D, tex);
  glBegin(GL_QUADS);
  glTexCoord2f(u0, v0);
  glVertex3f(-w / 2, h / 2, 0);
  glTexCoord2f(u1, v0);
  glVertex3f(w / 2, h / 2, 0);
  glTexCoord2f(u1, v1);
  glVertex3f(w / 2, -h / 2, 0);
  glTexCoord2f(u0, v1);
  glVertex3f(-w / 2, -h / 2, 0);
  glEnd();
}

int focusIndex = 0;
int focusCandidate = -1;
int focusFrames = 0;
const int FOCUS_HOLD_FRAMES = 20;
static bool isLookingAt(float eyeX, float eyeY, float eyeZ, float rayX,
                        float rayY, float rayZ, float centerX, float centerY,
                        float centerZ, float width, float height) {
  // Avoid division by zero for ray parallel to plane
  if (fabs(rayZ) < 1e-5) {
    return false;
  }

  // Calculate intersection parameter t for plane Z = centerZ
  float t = (centerZ - eyeZ) / rayZ;

  // Intersection must be in front of the eye
  if (t < 0) {
    return false;
  }

  // Calculate intersection point coordinates
  float ix = eyeX + rayX * t;
  float iy = eyeY + rayY * t;

  // Check if intersection lies within quad bounds
  return (ix >= centerX - width / 2 && ix <= centerX + width / 2 &&
          iy >= centerY - height / 2 && iy <= centerY + height / 2);
}

static void render(const std::vector<CapturedOutput> &outs,
                   const std::vector<MyMonitor> &mons, int fbW, int fbH) {
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_TEXTURE_2D);

  int w = 0, h = 0;
  window_get_framebuffer_size(&w, &h);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(glasses.fov, w > 0 ? double(w) / double(h) : 16.0 / 9.0, 0.1,
                 100.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  float roll = get_roll(glasses);
  float roll_perc = 0.0f;
  float focused_w = 3.0f;
  float n = 360.0f / angle_deg;
  float pi_div_n = 3.14159265359f / n;
  float r = (focused_w / 2.0f) * (std::cos(pi_div_n) / std::sin(pi_div_n));
  float base_z = (-r + roll_perc);

  float rayX, rayY, rayZ;
  getLookVector(rayX, rayY, rayZ);
  float flat_z = (r - focused_w * 1.05f) * eye_zoom_mult;
  float eyeX = rayX * roll_perc, eyeY = rayY * roll_perc,
        eyeZ = rayZ * roll_perc - flat_z;
  float tx = eyeX + rayX, ty = eyeY + rayY, tz = eyeZ + rayZ;
  gluLookAt(eyeX, eyeY, eyeZ, tx, ty, tz, 0, 1, 0);
  glRotatef(roll, 0, 0, 1);

  while (focusedmonitors.size() > 7)
    focusedmonitors.pop_back();

  // Draw ring (background monitors)
  for (int i = 0; i < int(focusedmonitors.size()) - 1; ++i) {
    const MyMonitor *m = focusedmonitors[i + 1];
    if (!m)
      continue;

    // Find which output feeds this monitor rectangle
    // In this simplified layout every MyMonitor corresponds 1:1 to an output by
    // index.
    int idx = m->index;
    if (idx < 0 || idx >= (int)outs.size())
      continue;

    float aspect = float(m->height) / float(m->width);
    float focused_h = focused_w * aspect;

    glPushMatrix();
    glRotatef(-i * angle_deg + screen_angle_offset_degrees, 0, 1, 0);
    glTranslatef(0, 0, base_z);

    float u0, v0, u1, v1;
    getMonitorUVs(*m, fbW, fbH, u0, v0, u1, v1);
    drawTexturedQuad(outs[idx].texture, u0, v0, u1, v1, focused_w, focused_h);
    glPopMatrix();
  }

  // Foreground monitor
  if (!focusedmonitors.empty() && focusedmonitors[0]) {
    const MyMonitor *m = focusedmonitors[0];
    int idx = m->index;
    if (idx >= 0 && idx < (int)outs.size()) {
      float aspect = float(m->height) / float(m->width),
            focused_h = focused_w * aspect;
      glPushMatrix();
      glRotatef(-angle_deg * aspect, 1, 0, 0);
      glTranslatef(0, 0, base_z);
      float u0, v0, u1, v1;
      getMonitorUVs(*m, fbW, fbH, u0, v0, u1, v1);
      drawTexturedQuad(outs[idx].texture, u0, v0, u1, v1, focused_w, focused_h);
      glPopMatrix();
    }
  }

  // Thumbnails row (one per output)
  float thumbY = 1.2f, spacing = 0.6f, thumbSize = 0.55f;
  for (size_t i = 0; i < mons.size(); ++i) {
    const MyMonitor &m = mons[i];
    int idx = m.index;
    if (idx < 0 || idx >= (int)outs.size())
      continue;

    float x = (i - (mons.size() - 1) / 2.0f) * spacing, y = thumbY, z = base_z;
    glPushMatrix();
    glTranslatef(x, y, z);
    float u0, v0, u1, v1;
    getMonitorUVs(m, fbW, fbH, u0, v0, u1, v1);
    drawTexturedQuad(outs[idx].texture, u0, v0, u1, v1, thumbSize, thumbSize);
    glPopMatrix();

    // Gaze selection
    if (isLookingAt(eyeX, eyeY, eyeZ, rayX, rayY, rayZ, x, y, z, thumbSize,
                    thumbSize)) {
      if (focusCandidate == i) {
        focusFrames++;
        if (focusFrames >= FOCUS_HOLD_FRAMES) {
          focusIndex = i;
          focusCandidate = -1;
          focusFrames = 0;
          if (focusedmonitors.size() == 0) {
            focusedmonitors.push_back(&monitors[i]);
          } else {
            focusedmonitors[0] = &monitors[i];
          }
        }
      } else {
        focusCandidate = i;
        focusFrames = 1;
      }
    }
  }

  if (center_dot_enabled)
    draw_filled_center_rect(4, 4);
}

int main(int, char **) {
  if (init_glasses() != ERR_SUCCESS) {
    std::fprintf(stderr, "Failed to setup glasses\n");
    return 1;
  }
  glasses.fov = 40.0;
  on_align();

  // Hook command handlers
  cmd_on_align = on_align;
  cmd_on_push = on_push;
  cmd_on_pop = on_pop;
  cmd_on_zoom_in_fov = on_zoom_in_fov;
  cmd_on_zoom_out_fov = on_zoom_out_fov;
  cmd_on_zoom_in = on_zoom_in;
  cmd_on_zoom_out = on_zoom_out;
  cmd_on_shift_left = on_shift_left;
  cmd_on_shift_right = on_shift_right;
  cmd_on_toggle_center_dot = on_toggle_center_dot;

  // Window + GL (EGL)
  init_window_and_gl(1920, 1080, "Viture AR (Wayland DMA-BUF)");

  if (!initGL()) {
    std::fprintf(stderr, "GL init failed\n");
    shutdown_window();
    return 1;
  }

  // Command server (adopt systemd socket if present; otherwise bind
  // $XDG_RUNTIME_DIR/viture.sock)
  if (!cmdsrv_init()) {
    std::fprintf(stderr, "Command server init failed\n");
  }

  // Discover outputs & build a synthetic big framebuffer layout (side-by-side)
  std::vector<CapturedOutput> outs;
  int fbW = 0, fbH = 0;
  wlr_multi_capture_init(outs, &fbW, &fbH);
  std::fprintf(stdout, "[debug] fbW=%d, fbH=%d\n", fbW, fbH);

  // for (auto& o : outs) { fbW += o.width; fbH = std::max(fbH, o.height); }
  std::fprintf(stdout, "[debug] Found %zu monitors\n", outs.size());
  for (auto &o : outs) {
    std::fprintf(stdout,
                 "[debug] CapturedOutput { x=%d, y=%d, width=%d, height=%d }\n",
                 o.x, o.y, o.width, o.height);
  }

  monitors.clear();
  int curX = 0;
  for (int i = 0; i < (int)outs.size(); ++i) {
    MyMonitor m{};
    m.x = curX;
    m.y = 0;
    m.width = outs[i].width;
    m.height = outs[i].height;
    m.index = i; // 1:1 mapping monitor -> CapturedOutput index
    monitors.push_back(m);
    curX += m.width;
  }

  focusedmonitors.clear();
  if (!monitors.empty()) {
    focusedmonitors.push_back(&monitors[0]);
  }

  long highest = 0;
  int frame = 0;
  const int warm_frames = 1000;
  const useconds_t target_us = 1000000 / 120;

  while (!window_should_close()) {
    auto start = std::chrono::high_resolution_clock::now();

    // Update all outputs
    wlr_multi_next_frame(outs);

    // Render using our stitched layout
    cmdsrv_poll();
    render(outs, monitors, fbW, fbH);

    window_swap();
    window_poll();

    auto end = std::chrono::high_resolution_clock::now();
    auto dur =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
            .count();
    if (frame > warm_frames) {
      if (dur > highest) {
        highest = dur;
        std::cout << "new highest frame " << dur << " us\n";
      }
    } else {
      ++frame;
    }

    if (dur < target_us) {
      usleep(target_us - dur);
    }
  }

  wlr_multi_shutdown();
  cmdsrv_shutdown();
  shutdown_window();
  return 0;
}
