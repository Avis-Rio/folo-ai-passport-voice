// main/app_state.h —— 纯 C 状态机归约器(无 IDF 依赖,宿主机可测)。
// 模式借鉴 origin/demo/claude-buddy-port 的 buddy_state.c:状态 + 事件 → 动作。
#pragma once

#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    app_stage_t    state;
    bool           link_up;       // 当前通道已通(BLE:EVENT 已订阅 / USB:会话已 up);false → 禁 PTT + OFFLINE
    bool           net_busy;
    bool           screen_on;
    bool           panel_on;      // 面板供电(20s 背光灭后 60s 级 SLPIN 断电)
    bool           locked;        // 锁定息屏(仅 OK 长按解锁;叠加态,state 保持原值)
    bool           ble_connected; // BLE 连接已建立(与 link_up 区别:连接但未订阅)
    uint8_t        link_channel;  // 0=BLE / 2=USB(link_up 时是当前通道;断后保留供横幅显示)
    uint64_t       last_key_ms;     // 最近按键时刻(息屏计时)
    uint64_t       state_since_ms;  // 进入当前状态的时刻(超时计时)
    uint64_t       wake_ms;         // 最近一次息屏唤醒时刻(OK_LONG_GUARD:防"唤醒即锁")
    // 清空已从"UP 双击"改为"DOWN 长按"(2026-08-29),随之删掉的字段:
    // ptt_end_ms / up_tap_ms / ptt_pending_ms / up_press_ms / clear_ms ——
    // 它们全部只服务于"同一颗键上长按与双击互相干扰"的补偿逻辑(自建轻点链、
    // 挂起确认、回弹假双击防御、驱动补报去重)。UP 上不再有双击语义,DOWN 上
    // 单击与长按靠驱动阈值天然互斥,补偿逻辑整块作废,详见 app_state.c 键位注释。
    uint16_t       last_up_press_mv; // UP 键最近 PRESS 回调读数:真实按压 3-5mV
                                     // (手指在键上),假按(射频腐蚀)2890mV(无人
                                     // 按键)。CLICK 回调时刻用户已松手(mv 恒
                                     // 2890),真假只能从 PRESS 读数区分 ——
                                     // TRANSCRIBING 态单击退出判定用(见 state.c)
    uint64_t       ble_connect_ms;  // BLE 连接时刻(连接握手期密集射频 → 假长按,抑制 PTT)
    uint64_t       toast_until_ms;
    bool           stream_started;  // 本会话采集是否已开(START 音播完后 TONE_DONE 驱动)
    char           toast[APP_TOAST_MAX];
    char           agent_message[APP_AGENT_MSG_MAX];
    bool           transcript_final;   // false → agent_message 是转写预览(未定稿);true = 定稿/非转写
    char           agent_state_name[16];
    char           task_id[APP_TASK_ID_MAX];
    char           approval_title[APP_TITLE_MAX];
    char           approval_target[APP_TARGET_MAX];
    char           approval_diff[APP_DIFF_MAX];
    uint8_t        approval_risk;
    bool           approval_details;   // ▼ 详情视图开关
    bool           mic_hold;           // 常开麦克风模式(console `mic on` 置位):
                                       // LISTENING 下吞掉物理 UP 松开与 60s 兜底,
                                       // 仅 `mic off`/链路断开收束(2026-10-02)
    // ---- 设置(2026-10-03):归约器是纯 C,不读 NVS;开机由 main.c 装载,
    // 变更经 APP_ACT_SAVE_SETTINGS 动作持久化(与执行器边界一致)。----
    uint8_t        tone_level;         // tone_lvl_t(0=OFF/1=LOW/2=HIGH;缺省 HIGH)
    uint8_t        night_mute;         // 夜间静音(21:30-07:00)开关;缺省关
    int8_t         tz_hour;            // 时区 ±12(缺省 8,与 time_sync 同源装载)
    uint8_t        settings_sel;       // 设置页选中项(0=Sound 1=NightMute 2=Timezone)
    uint64_t       settings_last_ms;   // 设置页最近按键时刻(10s 无操作自动退出)
} app_state_t;

void app_state_init(app_state_t *s);
// 开机装载持久化设置(NVS 读出值;越界兜底缺省)。app_task 在 init 后调用一次。
void app_state_load_settings(app_state_t *s, uint8_t tone_level, uint8_t night_mute,
                             int8_t tz_hour);
// 处理一个事件,产出 0..out_max 个动作。now_ms 为毫秒单调时钟。
void app_state_reduce(app_state_t *s, const app_event_t *ev,
                      uint64_t now_ms, app_action_t *out, uint8_t *out_n);

void app_state_snapshot(const app_state_t *s, uint64_t now_ms, app_ui_snapshot_t *snap);

#ifdef __cplusplus
}
#endif
