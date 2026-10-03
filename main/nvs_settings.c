// main/nvs_settings.c —— 设置存储实现。
#include "nvs_settings.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "settings";
#define APP_NS "app"

// 注:rf_mode 键已随双通道常开架构退役(2026-08-28,不再有互斥模式);旧键
// 残留于 NVS 无读取方,无害(factory 清空可除)。
static const char *K_TZ_HOUR   = "tz_hour";
static const char *K_TONE_LVL  = "tone_lvl";    // 提示音档位(0/1/2,缺省 2)
static const char *K_NIGHT_MUTE = "night_mute"; // 夜间静音开关(0/1,缺省 0)

esp_err_t nvs_settings_init(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) nvs_close(h);
    return e;
}

// 时区偏移小时(int8 支持负偏移,±12);缺省 8(Asia/Shanghai)
esp_err_t nvs_settings_get_tz_hour(int8_t *hour) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) { if (hour) *hour = 8; return ESP_OK; }   // 打不开按默认兜底
    int8_t v = 8;
    e = nvs_get_i8(h, K_TZ_HOUR, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) v = 8;   // 首次使用:默认 +8
    if (hour) *hour = (v >= -12 && v <= 12) ? v : 8;   // 损坏值兜底
    return ESP_OK;
}

esp_err_t nvs_settings_set_tz_hour(int8_t hour) {
    if (hour < -12 || hour > 12) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_i8(h, K_TZ_HOUR, hour);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "tz_hour = %d", (int)hour);
    return e;
}

void nvs_settings_factory_reset(void) {
    nvs_handle_t h;
    if (nvs_open(APP_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "app 命名空间已清空");
    }
}

// ---- 提示音档位 / 夜间静音(2026-10-03)----
// 读侧缺省兜底与 tz_hour 同风格:打不开/未写/损坏 → 缺省值,固件永不因缺键卡死。

esp_err_t nvs_settings_get_tone_level(uint8_t *lvl) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) { if (lvl) *lvl = 2; return ESP_OK; }
    uint8_t v = 2;
    e = nvs_get_u8(h, K_TONE_LVL, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) v = 2;
    if (lvl) *lvl = (v < 3) ? v : 2;   // 越界兜底
    return ESP_OK;
}

esp_err_t nvs_settings_set_tone_level(uint8_t lvl) {
    if (lvl >= 3) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, K_TONE_LVL, lvl);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "tone_lvl = %u", (unsigned)lvl);
    return e;
}

esp_err_t nvs_settings_get_night_mute(uint8_t *on) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READONLY, &h);
    if (e != ESP_OK) { if (on) *on = 0; return ESP_OK; }
    uint8_t v = 0;
    e = nvs_get_u8(h, K_NIGHT_MUTE, &v);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND) v = 0;
    if (on) *on = v ? 1 : 0;
    return ESP_OK;
}

esp_err_t nvs_settings_set_night_mute(uint8_t on) {
    if (on > 1) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open(APP_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, K_NIGHT_MUTE, on);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "night_mute = %u", (unsigned)on);
    return e;
}
