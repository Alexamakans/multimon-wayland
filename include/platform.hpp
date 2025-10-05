#pragma once
void init_window_and_gl(int width, int height, const char* title);
bool window_should_close();
void window_poll();
void window_swap();
void window_get_framebuffer_size(int* w, int* h);
void shutdown_window();
