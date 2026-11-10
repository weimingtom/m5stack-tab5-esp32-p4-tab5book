#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "tab5book";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);

    esp_err_t sd = bsp_sdcard_mount();
    ESP_LOGI(TAG, "SD card mount: %s", esp_err_to_name(sd));

    bsp_display_brightness_set(80);
    bsp_display_backlight_on();

    bsp_display_lock(0);
    ui_init(sd == ESP_OK);
    bsp_display_unlock();

    ESP_LOGI(TAG, "Tab5 Book Reader started (free heap=%u, PSRAM=%u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
