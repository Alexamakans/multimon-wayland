#include <GL/gl.h>
#include <GL/glu.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <unistd.h>

#include "platform.hpp"
#include "capture_wlr_dmabuf.hpp"
#include "command_server.hpp"
#include "glasses.hpp"
#include "viture.h"

struct MyMonitor { int x,y,width,height,index; };

static float screen_angle_offset_degrees = 0.0f;
static bool  center_dot_enabled = true;

static std::vector<MyMonitor>  monitors;
static std::vector<MyMonitor*> focusedmonitors;

// ---- Commands hooked into command_server ----
static void on_align()            { glasses.oroll=-glasses.roll; glasses.opitch=-glasses.pitch; glasses.oyaw=-glasses.yaw; }
static void on_push()             { for(int i=int(focusedmonitors.size())-1;i>=0;i--) if (i==int(focusedmonitors.size())-1) focusedmonitors.push_back(focusedmonitors[i]); else focusedmonitors[i+1]=focusedmonitors[i]; if(!focusedmonitors.empty()) focusedmonitors[0]=nullptr; }
static void on_pop()              { for(size_t i=0;i<focusedmonitors.size();++i) focusedmonitors[i]=(i+1<focusedmonitors.size()?focusedmonitors[i+1]:nullptr); if(!focusedmonitors.empty()) focusedmonitors.pop_back(); }
static void on_zoom_in()          { glasses.fov *= 0.99;  }
static void on_zoom_out()         { glasses.fov *= 1.01;  }
static void on_shift_left()       { screen_angle_offset_degrees += 5.0f; }
static void on_shift_right()      { screen_angle_offset_degrees -= 5.0f; }
static void on_toggle_center_dot(){ center_dot_enabled = !center_dot_enabled; }

// ---- Tiny math helpers ----
static void draw_filled_center_rect(float half_w, float half_h) {
  int vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
  int W=vp[2], H=vp[3];
  float cx=W/2.f, cy=H/2.f;

  glMatrixMode(GL_PROJECTION); glPushMatrix(); {
    glLoadIdentity(); glOrtho(0,W,0,H,-1,1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_TEXTURE_2D); glDisable(GL_DEPTH_TEST); glColor3f(1,0,0);
    glBegin(GL_QUADS);
      glVertex2f(cx-half_w, cy-half_h);
      glVertex2f(cx+half_w, cy-half_h);
      glVertex2f(cx+half_w, cy+half_h);
      glVertex2f(cx-half_w, cy+half_h);
    glEnd();
    glEnable(GL_DEPTH_TEST); glPopMatrix(); glMatrixMode(GL_PROJECTION);
  } glPopMatrix(); glMatrixMode(GL_MODELVIEW); glColor3f(1,1,1); glEnable(GL_TEXTURE_2D);
}

static void getLookVector(float &dx,float &dy,float &dz){
  float pitchRad = get_pitch(glasses) * M_PI / 180.0;
  float yawRad   = get_yaw(glasses)   * M_PI / 180.0;
  dx =  std::sin(yawRad) * -std::cos(pitchRad);
  dy = -std::sin(pitchRad);
  dz = -std::cos(yawRad) *  std::cos(pitchRad);
}

static bool isLookingAt(float ex,float ey,float ez,float rx,float ry,float rz,float cx,float cy,float cz,float w,float h){
  if (std::fabs(rz) < 1e-5) return false;
  float t = (cz - ez)/rz; if (t < 0) return false;
  float ix = ex + rx*t, iy = ey + ry*t;
  return (ix >= cx-w/2 && ix <= cx+w/2 && iy >= cy-h/2 && iy <= cy+h/2);
}

static bool initGL() {
  glEnable(GL_TEXTURE_2D);
  glClearColor(0.f,0.f,0.f,1.f);
  glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
  int w=0,h=0; window_get_framebuffer_size(&w,&h);
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluPerspective(60.0, w>0?double(w)/double(h):16.0/9.0, 0.1, 1000.0);
  glMatrixMode(GL_MODELVIEW);
  return true;
}

static void getMonitorUVs(const MyMonitor&m,int fbW,int fbH,float&u0,float&v0,float&u1,float&v1){
  u0=float(m.x)/fbW; v0=float(m.y)/fbH; u1=float(m.x+m.width)/fbW; v1=float(m.y+m.height)/fbH;
}

static void render(GLuint frameTex, int fbW, int fbH){
  glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST); glEnable(GL_TEXTURE_2D);

  int w=0,h=0; window_get_framebuffer_size(&w,&h);
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluPerspective(glasses.fov, w>0?double(w)/double(h):16.0/9.0, 0.1, 100.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();

  float roll = get_roll(glasses);
  float roll_perc = 0.0f; // shaping disabled for now

  float focused_w = 3.0f;
  float angle_deg = 20.0f;
  float n = 360.0f / angle_deg;
  float pi_div_n = 3.14159265359f / n;
  float r = (focused_w / 2.0f) * (std::cos(pi_div_n) / std::sin(pi_div_n));
  float base_z = -r + roll_perc;

  float rayX, rayY, rayZ; getLookVector(rayX, rayY, rayZ);
  float flat_z = r - focused_w*1.05f;
  float eyeX = rayX*roll_perc, eyeY = rayY*roll_perc, eyeZ = rayZ*roll_perc - flat_z;
  float tx = eyeX+rayX, ty = eyeY+rayY, tz = eyeZ+rayZ;
  gluLookAt(eyeX,eyeY,eyeZ, tx,ty,tz, 0,1,0);
  glRotatef(roll,0,0,1);

  glBindTexture(GL_TEXTURE_2D, frameTex);

  while (focusedmonitors.size() > 7) focusedmonitors.pop_back();

  for (int i=0; i<int(focusedmonitors.size())-1; ++i) {
    const MyMonitor*m = focusedmonitors[i+1]; if (!m) continue;
    float aspect = float(m->height)/m->width, focused_h = focused_w*aspect;

    glPushMatrix();
    glRotatef(-i*angle_deg + screen_angle_offset_degrees, 0,1,0);
    glTranslatef(0,0,base_z);

    float u0,v0,u1,v1; getMonitorUVs(*m, fbW, fbH, u0,v0,u1,v1);
    glBegin(GL_QUADS);
      glTexCoord2f(u0,v0); glVertex3f(-focused_w/2, focused_h/2, 0);
      glTexCoord2f(u1,v0); glVertex3f( focused_w/2, focused_h/2, 0);
      glTexCoord2f(u1,v1); glVertex3f( focused_w/2,-focused_h/2, 0);
      glTexCoord2f(u0,v1); glVertex3f(-focused_w/2,-focused_h/2, 0);
    glEnd();
    glPopMatrix();
  }

  if (!focusedmonitors.empty() && focusedmonitors[0]) {
    const MyMonitor*m = focusedmonitors[0];
    float aspect=float(m->height)/m->width, focused_h=focused_w*aspect;
    glPushMatrix();
    glRotatef(-angle_deg*aspect,1,0,0);
    glTranslatef(0,0,base_z);
    float u0,v0,u1,v1; getMonitorUVs(*m, fbW, fbH, u0,v0,u1,v1);
    glBegin(GL_QUADS);
      glTexCoord2f(u0,v0); glVertex3f(-focused_w/2, focused_h/2, 0);
      glTexCoord2f(u1,v0); glVertex3f( focused_w/2, focused_h/2, 0);
      glTexCoord2f(u1,v1); glVertex3f( focused_w/2,-focused_h/2, 0);
      glTexCoord2f(u0,v1); glVertex3f(-focused_w/2,-focused_h/2, 0);
    glEnd();
    glPopMatrix();
  }

  float thumbY=1.2f, spacing=0.6f, thumbSize=0.55f;
  for (size_t i=0;i<monitors.size();++i){
    float x=(i-(monitors.size()-1)/2.0f)*spacing, y=thumbY, z=base_z;
    glPushMatrix(); glTranslatef(x,y,z);
    float u0,v0,u1,v1; getMonitorUVs(monitors[i], fbW, fbH, u0,v0,u1,v1);
    glBegin(GL_QUADS);
      glTexCoord2f(u0,v0); glVertex3f(-thumbSize/2, thumbSize/2, 0);
      glTexCoord2f(u1,v0); glVertex3f( thumbSize/2, thumbSize/2, 0);
      glTexCoord2f(u1,v1); glVertex3f( thumbSize/2,-thumbSize/2, 0);
      glTexCoord2f(u0,v1); glVertex3f(-thumbSize/2,-thumbSize/2, 0);
    glEnd();
    glPopMatrix();

    // Gaze selection logic trimmed here for brevity — copy yours if needed.
  }

  if (center_dot_enabled) draw_filled_center_rect(4,4);
}

int main(int, char**) {
  if (init_glasses()!=ERR_SUCCESS) {
    std::fprintf(stderr,"Failed to setup glasses\n");
    return 1;
  }
  glasses.fov = 45.0;
  on_align();

  // Hook command handlers
  cmd_on_align            = on_align;
  cmd_on_push             = on_push;
  cmd_on_pop              = on_pop;
  cmd_on_zoom_in          = on_zoom_in;
  cmd_on_zoom_out         = on_zoom_out;
  cmd_on_shift_left       = on_shift_left;
  cmd_on_shift_right      = on_shift_right;
  cmd_on_toggle_center_dot= on_toggle_center_dot;

  // Window + GL (EGL)
  init_window_and_gl(1920,1080,"Viture AR (Wayland DMA-BUF)");

  if (!initGL()) {
    std::fprintf(stderr,"GL init failed\n");
    shutdown_window();
    return 1;
  }

  // Command server (adopt systemd socket if present; otherwise bind $XDG_RUNTIME_DIR/viture.sock)
  if (!cmdsrv_init()) {
    std::fprintf(stderr,"Command server init failed\n");
  }

  // Wayland capture (fast path)
  int fbW=0, fbH=0;
  wlr_dmabuf_capture_init(nullptr, &fbW, &fbH);

  // Single “monitor” covering whole framebuffer; extend if you want panes
  monitors.clear();
  monitors.push_back({0,0,fbW,fbH,0});
  focusedmonitors.clear();
  focusedmonitors.push_back(&monitors[0]);

  long highest=0; int frame=0;
  const int warm_frames=1000;
  const useconds_t target_us = 1000000/120;

  while (!window_should_close()) {
    auto start = std::chrono::high_resolution_clock::now();

    CaptureFrame f = wlr_dmabuf_next_frame();

    cmdsrv_poll();     // process all pending control messages
    render(f.texture, f.width, f.height);

    window_swap();
    window_poll();

    auto end = std::chrono::high_resolution_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(end-start).count();
    if (frame > warm_frames) { if (dur > highest){ highest = dur; std::cout<<"new highest frame "<<dur<<" us\n"; } }
    else ++frame;

    if (dur < target_us) usleep(target_us - dur);
  }

  wlr_dmabuf_capture_shutdown();
  cmdsrv_shutdown();
  shutdown_window();
  return 0;
}

