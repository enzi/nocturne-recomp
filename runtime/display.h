#pragma once

#include <cstdint>

void display_prepare_window_attributes();
bool display_init();
void display_drawable_size(int& w, int& h);
void display_present_bgrx(const void* pixels, int w, int h, int pitchBytes);
void display_present_texture(uint32_t texture, int w, int h);
