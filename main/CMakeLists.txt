idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "app_state.c"
         "app_protocol.c"
         "app_events.c"
         "app_sound.c"
         "app_ui.c"
         "nvs_settings.c"
         "adpcm.c"
         "audio_streamer.c"
         "ble_audio.c"
         "ble_dtm.c"      # 自 ESP-IDF 5.5.3 拷入:host 层 DTM API 未编入 libbt.a
         "console_cmds.c"
         "time_sync.c"
         "tone_policy.c"
         "mode.c"
         "usb_link.c"
         "usb_link_framing.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
             json
             nvs_flash
             console  # IDF >= 5.5: esp_console 组件已重命名为 console
             bt
             esp_pm       # 空闲 light sleep + CPU 变频(录音会话锁见 app_main)
             esp_driver_usb_serial_jtag
             esp_app_format   # 开机画面版本号 esp_app_get_description(2026-10-03;头文件 esp_app_desc.h)
             app_update       # 双固件 v2:esp_ota_set_boot_partition(菜单切槽 2026-10-03)
             esp_partition    # 槽位探测 esp_partition_read/find_first(菜单槽状态)
)
