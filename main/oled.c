#include "oled.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_check.h"
#include "esp_log.h"
#include "font5x7.h"

static const char *TAG = "oled";

static esp_lcd_panel_handle_t s_panel;
/* SSD1306 page format: byte = 8 vertical pixels, LSB on top. */
static uint8_t s_fb[OLED_W * OLED_H / 8];

esp_err_t oled_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = 0,
        .sda_io_num = CONFIG_EP8_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_EP8_I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = CONFIG_EP8_OLED_I2C_ADDR,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_ssd1306_config_t ssd_cfg = {.height = OLED_H};
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");

    oled_clear();
    return oled_flush();
}

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) {
        return;
    }
    uint8_t *b = &s_fb[x + (y / 8) * OLED_W];
    if (on) {
        *b |= 1u << (y % 8);
    } else {
        *b &= ~(1u << (y % 8));
    }
}

void oled_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            oled_pixel(i, j, on);
        }
    }
}

void oled_rect(int x, int y, int w, int h)
{
    oled_fill_rect(x, y, w, 1, true);
    oled_fill_rect(x, y + h - 1, w, 1, true);
    oled_fill_rect(x, y, 1, h, true);
    oled_fill_rect(x + w - 1, y, 1, h, true);
}

static void draw_char(int x, int y, char c, int scale)
{
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    const uint8_t *g = font5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (g[col] & (1u << row)) {
                oled_fill_rect(x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

int oled_text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0; /* no trailing gap */
}

void oled_text(int x, int y, const char *s, int scale)
{
    for (; *s; s++, x += 6 * scale) {
        draw_char(x, y, *s, scale);
    }
}

esp_err_t oled_flush(void)
{
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_W, OLED_H, s_fb);
}
