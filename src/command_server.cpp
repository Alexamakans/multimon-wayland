#include "command_server.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// If libsystemd is available we’ll prefer its helpers.
// CMake will define HAVE_LIBSYSTEMD when found (see patches below).
#ifdef HAVE_LIBSYSTEMD
  #include <systemd/sd-daemon.h>
#endif

// ---- public callbacks ----
static int g_listen_fd = -1;

void (*cmd_on_align)()             = nullptr;
void (*cmd_on_push)()              = nullptr;
void (*cmd_on_pop)()               = nullptr;
void (*cmd_on_zoom_in_fov)()           = nullptr;
void (*cmd_on_zoom_out_fov)()          = nullptr;
void (*cmd_on_zoom_in)()           = nullptr;
void (*cmd_on_zoom_out)()          = nullptr;
void (*cmd_on_shift_left)()        = nullptr;
void (*cmd_on_shift_right)()       = nullptr;
void (*cmd_on_toggle_center_dot)() = nullptr;

// ---- helpers ----
static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static bool is_seqpacket_unix(int fd) {
  // Check socket type
  int t = 0; socklen_t tl = sizeof(t);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &t, &tl) < 0) return false;
  if (t != SOCK_SEQPACKET) return false;

  // Check family is AF_UNIX
  sockaddr_un su{}; socklen_t sl = sizeof(su);
  if (getsockname(fd, (sockaddr*)&su, &sl) < 0) return false;
  return su.sun_family == AF_UNIX;
}

#ifdef HAVE_LIBSYSTEMD
static int pick_socket_fd_systemd_named_or_first() {
  // Prefer a named fd "viture" if present.
  char **names = nullptr;
  int n = sd_listen_fds_with_names(0, &names); // returns count, fds are 3..(3+n-1)
  if (n < 0) return -1;
  const int base = SD_LISTEN_FDS_START;

  int chosen = -1;
  // 1) Prefer by name
  for (int i = 0; i < n; ++i) {
    if (names && names[i] && std::string(names[i]) == "viture") {
      int fd = base + i;
      if (is_seqpacket_unix(fd)) { chosen = fd; break; }
    }
  }
  // 2) Fallback: first UNIX/SEQPACKET among passed fds
  if (chosen < 0) {
    for (int i = 0; i < n; ++i) {
      int fd = base + i;
      if (is_seqpacket_unix(fd)) { chosen = fd; break; }
    }
  }

  if (names) {
    for (int i = 0; i < n; ++i) free(names[i]);
    free(names);
  }
  return chosen;
}
#endif

static int pick_socket_fd_from_env() {
  // Pure env-based fallback for when libsystemd isn’t linked.
  const char* c_fds = std::getenv("LISTEN_FDS");
  const char* c_pid = std::getenv("LISTEN_PID");
  if (!c_fds || !c_pid) return -1;
  if ((pid_t)std::atoi(c_pid) != getpid()) return -1;

  int n = std::atoi(c_fds);
  if (n <= 0) return -1;

  const int base = 3; // documented systemd start fd
  // If LISTEN_FDNAMES exists, prefer "viture" name.
  const char* c_names = std::getenv("LISTEN_FDNAMES");
  if (c_names && *c_names) {
    // colon-separated names
    std::string names_str(c_names);
    size_t start = 0;
    for (int i = 0; i < n; ++i) {
      size_t end = names_str.find(':', start);
      std::string name = names_str.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (name == "viture") {
        int fd = base + i;
        if (is_seqpacket_unix(fd)) return fd;
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }

  // Otherwise first UNIX/SEQPACKET
  for (int i = 0; i < n; ++i) {
    int fd = base + i;
    if (is_seqpacket_unix(fd)) return fd;
  }
  return -1;
}

static int cmdsrv_bind_own_socket() {
  // Create our own socket in $XDG_RUNTIME_DIR.
  const char* rt = std::getenv("XDG_RUNTIME_DIR");
  if (!rt || !*rt) rt = "/run/user"; // try well-known root (we’ll still fall back safely)
  std::string path;

  if (std::getenv("XDG_RUNTIME_DIR") && *std::getenv("XDG_RUNTIME_DIR")) {
    path = std::string(std::getenv("XDG_RUNTIME_DIR")) + "/viture.sock";
  } else {
    // Fallback to /run/user/<uid>/viture.sock
    // (on NixOS user sessions this exists; on weird setups, it may not)
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/run/user/%u", (unsigned)getuid());
    path = std::string(buf) + "/viture.sock";
  }

  ::unlink(path.c_str()); // remove stale file

  int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

  if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    ::close(fd);
    return -1;
  }

  ::chmod(path.c_str(), 0600);
  if (::listen(fd, 8) < 0) {
    perror("listen");
    ::close(fd);
    return -1;
  }

  set_nonblock(fd);
  return fd;
}

// ---- public API ----
bool cmdsrv_init() {
  // 1) Prefer systemd socket activation (named fd if available)
  int fd = -1;

#ifdef HAVE_LIBSYSTEMD
  fd = pick_socket_fd_systemd_named_or_first();
  if (fd >= 0) {
    set_nonblock(fd);
    g_listen_fd = fd;
    return true;
  }
#else
  // If we didn’t link libsystemd, try env-heuristics
  fd = pick_socket_fd_from_env();
  if (fd >= 0) {
    set_nonblock(fd);
    g_listen_fd = fd;
    return true;
  }
#endif

  // 2) Fallback: create our own
  fd = cmdsrv_bind_own_socket();
  if (fd < 0) return false;

  g_listen_fd = fd;
  return true;
}

static void handle_cmd(const std::string& cmd) {
  if (cmd == "align")                 { if (cmd_on_align)             cmd_on_align(); }
  else if (cmd == "push")             { if (cmd_on_push)              cmd_on_push(); }
  else if (cmd == "pop")              { if (cmd_on_pop)               cmd_on_pop(); }
  else if (cmd == "zoom-in-fov")      { if (cmd_on_zoom_in_fov)       cmd_on_zoom_in_fov(); }
  else if (cmd == "zoom-out-fov")     { if (cmd_on_zoom_out_fov)      cmd_on_zoom_out_fov(); }
  else if (cmd == "zoom-in")          { if (cmd_on_zoom_in)           cmd_on_zoom_in(); }
  else if (cmd == "zoom-out")         { if (cmd_on_zoom_out)          cmd_on_zoom_out(); }
  else if (cmd == "shift-left")       { if (cmd_on_shift_left)        cmd_on_shift_left(); }
  else if (cmd == "shift-right")      { if (cmd_on_shift_right)       cmd_on_shift_right(); }
  else if (cmd == "toggle-center-dot"){ if (cmd_on_toggle_center_dot) cmd_on_toggle_center_dot(); }
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

    // One short command per connection
    char buf[256];
    ssize_t n = ::recv(cfd, buf, sizeof(buf)-1, 0);
    if (n > 0) {
      buf[n] = 0;
      std::string s(buf);
      // trim trailing whitespace/newlines
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

