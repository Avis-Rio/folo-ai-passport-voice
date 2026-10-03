// main/nvs_settings.h —— 持久化设置(NVS 命名空间 "app")。
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_settings_init(void);

// 注:rf_mode 键已随双通道常开架构退役(2026-08-28,不再有互斥模式),
// get/set_mode 已删除;旧键残留 NVS 无读取方,无害。

// 时区偏移小时(int8,±12;缺省 8)
esp_err_t nvs_settings_get_tz_hour(int8_t *hour);
esp_err_t nvs_settings_set_tz_hour(int8_t hour);

// 提示音档位(u8,tone_lvl_t:0=OFF/1=LOW/2=HIGH;缺省 2=HIGH,2026-10 前的固定行为)
esp_err_t nvs_settings_get_tone_level(uint8_t *lvl);
esp_err_t nvs_settings_set_tone_level(uint8_t lvl);

// 夜间自动静音(u8,0/1;缺省 0=关。窗口 21:30–07:00 固化在 tone_policy)
esp_err_t nvs_settings_get_night_mute(uint8_t *on);
esp_err_t nvs_settings_set_night_mute(uint8_t on);

void nvs_settings_factory_reset(void);   // 清 "app" 命名空间

#ifdef __cplusplus
}
#endif
