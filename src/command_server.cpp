#include "command_server.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_listen_fd = -1;

void (*cmd_on_align)()            = nullptr;
void (*cmd_on_push)()             = nullptr;
void (*cmd_on_pop)()              = nullptr;
void (*cmd_on_zoom_in)()          = nullptr;
void (*cmd_on_zoom_out)()         = nullptr;
void (*cmd_on_shift_left)()       = nullptr;
void (*cmd_on_shift_right)()      = nullptr;
void (*cmd_on_toggle_center_dot)()= nullptr;

static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

bool cmdsrv_init() {
  // Prefer systemd socket activation (fd=3):
  const char* fds = std::getenv("LISTEN_FDS");
  const char* pid = std::getenv("LISTEN_PID");
  if (fds && pid && std::atoi(fds) >= 1 && (pid_t)std::atoi(pid) == getpid()) {
    g_listen_fd = 3; // first passed socket
    set_nonblock(g_listen_fd);
    return true;
  }

  // Fallback: create our own socket in $XDG_RUNTIME_DIR (per-user tmp)
  const char* rt = std::getenv("XDG_RUNTIME_DIR");
  if (!rt) rt = "/tmp"; // worst-case fallback
  std::string path = std::string(rt) + "/viture.sock";

  ::unlink(path.c_str()); // remove stale

  g_listen_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (g_listen_fd < 0) {
    perror("socket");
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

  if (::bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    ::close(g_listen_fd); g_listen_fd = -1;
    return false;
  }

  ::chmod(path.c_str(), 0600);
  if (::listen(g_listen_fd, 8) < 0) {
    perror("listen");
    ::close(g_listen_fd); g_listen_fd = -1;
    return false;
  }

  set_nonblock(g_listen_fd);
  return true;
}

static void handle_cmd(const std::string& cmd) {
  if (cmd == "align")              { if (cmd_on_align)            cmd_on_align();            }
  else if (cmd == "push")          { if (cmd_on_push)             cmd_on_push();             }
  else if (cmd == "pop")           { if (cmd_on_pop)              cmd_on_pop();              }
  else if (cmd == "zoom-in")       { if (cmd_on_zoom_in)          cmd_on_zoom_in();          }
  else if (cmd == "zoom-out")      { if (cmd_on_zoom_out)         cmd_on_zoom_out();         }
  else if (cmd == "shift-left")    { if (cmd_on_shift_left)       cmd_on_shift_left();       }
  else if (cmd == "shift-right")   { if (cmd_on_shift_right)      cmd_on_shift_right();      }
  else if (cmd == "toggle-center-dot") { if (cmd_on_toggle_center_dot) cmd_on_toggle_center_dot(); }
  else {
    std::fprintf(stderr, "unknown cmd: %s\n", cmd.c_str());
  }
}

void cmdsrv_poll() {
  if (g_listen_fd < 0) return;

  for (;;) {
    int cfd = ::accept4(g_listen_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (cfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      perror("accept");
      break;
    }

    char buf[256];
    ssize_t n = ::recv(cfd, buf, sizeof(buf)-1, 0);
    if (n > 0) {
      buf[n] = 0;
      std::string s(buf);
      while (!s.empty() && (s.back()=='\n' || s.back()=='\r' || s.back()==' ')) s.pop_back();
      handle_cmd(s);
    }
    ::close(cfd);
  }
}

void cmdsrv_shutdown() {
  if (g_listen_fd >= 0) {
    ::close(g_listen_fd);
    g_listen_fd = -1;
  }
}

