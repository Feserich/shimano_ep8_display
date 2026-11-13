#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "ble_battery.h"
#include "oled.h"

static const char *TAG = "main";

static void draw_ui(ep8_state_t state, int level)
{
    static const char *status[] = {
        [EP8_STATE_SCANNING] = "SCAN",
        [EP8_STATE_CONNECTING] = "CONN",
        [EP8_STATE_PAIRING] = "PAIR",
        [EP8_STATE_CONNECTED] = "BLE",
    };
    bool live = state == EP8_STATE_CONNECTED && level >= 0;

    oled_clear();

    /* Header */
    oled_text(0, 0, "EP8 BATTERY", 1);
    const char *st = status[state];
    oled_text(OLED_W - oled_text_width(st, 1), 0, st, 1);
    oled_fill_rect(0, 10, OLED_W, 1, true);

    /* Big percentage. Last known value stays visible while reconnecting,
     * marked with '?' so you know it's stale. */
    char buf[16];
    if (level < 0) {
        snprintf(buf, sizeof(buf), "--%%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", level);
    }
    int scale = 4;
    int w = oled_text_width(buf, scale);
    oled_text((OLED_W - w) / 2, 15, buf, scale);
    if (!live && level >= 0) {
        oled_text(OLED_W - 6, 15, "?", 1);
    }

    /* Battery bar: body 116x12 + terminal nub */
    const int bx = 2, by = 49, bw = 118, bh = 13;
    oled_rect(bx, by, bw, bh);
    oled_fill_rect(bx + bw, by + 3, 3, bh - 6, true);
    if (level > 0) {
        int fill = (bw - 4) * level / 100;
        oled_fill_rect(bx + 2, by + 2, fill, bh - 4, true);
    }

    if (oled_flush() != ESP_OK) {
        ESP_LOGW(TAG, "oled flush failed");
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (oled_init() != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed - check wiring / I2C address");
    } else {
        draw_ui(EP8_STATE_SCANNING, -1);
    }

    ble_battery_start(draw_ui);
}
