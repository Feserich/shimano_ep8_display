#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OLED_W 128
#define OLED_H 64

esp_err_t oled_init(void);
void oled_clear(void);
void oled_pixel(int x, int y, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);
void oled_rect(int x, int y, int w, int h);
/* Draw text with the 5x7 font scaled by `scale` (char cell = 6*scale x 8*scale). */
void oled_text(int x, int y, const char *s, int scale);
int oled_text_width(const char *s, int scale);
esp_err_t oled_flush(void);
