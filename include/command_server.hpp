#pragma once

// Minimal SEQPACKET command server designed for systemd user socket activation.
// If LISTEN_FDS/LISTEN_PID are present, adopts fd=3. Otherwise, binds %t/viture.sock.
// Messages are single tokens like: "align", "zoom-in", etc.

bool cmdsrv_init();
void cmdsrv_poll();     // nonblocking: accept and process all pending messages
void cmdsrv_shutdown();

// Hooks to be set by your app:
extern void (*cmd_on_align)();
extern void (*cmd_on_push)();
extern void (*cmd_on_pop)();
extern void (*cmd_on_zoom_in_fov)();
extern void (*cmd_on_zoom_out_fov)();
extern void (*cmd_on_zoom_in)();
extern void (*cmd_on_zoom_out)();
extern void (*cmd_on_shift_left)();
extern void (*cmd_on_shift_right)();
extern void (*cmd_on_toggle_center_dot)();

