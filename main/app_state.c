// main/app_state.c —— 状态机归约器实现。
// 纯 C,不依赖 IDF。按键语义与超时规则见 prd/design 文档。
// 链路语义(双通道常开,2026-08-28):BLE/USB 同时可用,link_up = 任一通道通;
// link_channel = 会话路由通道(最近连接/使用,活动会话期间另一通道连接不夺路)。
#include "app_state.h"
#include "mode.h"
#include "tone_policy.h"
#include <string.h>
#include <stdio.h>

// 假按判定阈值:on_key 回调时刻 ADC 读数 ≥2000mV = 松开电平 → 假按(射频腐蚀
// 造成的幽灵事件,无人按键)。真实按压回调读的是该键分压档(UP 3-5mV / DOWN
// 150-447mV,事件处理时刻手指仍在键上)。三档键松开态 2890,2000 留足余量
// (RF_GUARD_HIGH_MV 同值)。UP 开录音与 DOWN 清空都拿它挡幽灵长按。
#define PTT_FAKE_LONG_MV 2000

// UP 键"真实按压"上限(2026-08-29,PTT 改按下即录后收紧):UP 分压档是 0-150mV
// (BSP_BTN_MV_TABLE),真实按压回调实测 3-5mV。2000 的通用门只挡"松开电平"型
// 幽灵(2890),挡不住跨档幽灵 —— 真机环里抓到过 `btn=0 ev=0 mv=598`(OK 键按住
// 期间冒出的假 UP 按下,598 落在 OK 档),按下即录的话它会直接开一次录音。
// 故 UP 侧一律用本档上限判真假(DOWN 清空仍用 PTT_FAKE_LONG_MV:DOWN 档 150-447,
// 真实读数 303-305,收到 150 会把自己挡掉)。
#define PTT_UP_PRESS_MAX_MV 150

// 连接握手期(~0.4-1s:参数协商/DLE/PHY 更新)密集射频会把按键 ADC 整段
// 腐蚀成 0mV → 假 UP 长按 → 自动开录音(真机实测: 连接后 130s 内自动触发
// 19 次 PTT 录音)。真实用户连接后 1s 内按住说话的概率极低,此窗口只吞
// 假事件;超窗后正常。
#define BLE_CONNECT_PTT_GUARD_MS 1000

// ---- 键位:按住 UP = 说话(按下即录),长按 DOWN = 清空,单击 DOWN = 回车(2026-08-29)----
// 上一版把"清空"放在 UP 双击上,和同一颗键的"长按说话"根本无法共存:iot_button
// 按住到阈值的当下就报 LONG_PRESS_START,并且从此不再为这一按上报 CLICK/DOUBLE。
// 于是双击的第二按只要多按几十毫秒就先变成录音,清空彻底消失(真机取证:阈值
// 300ms 时用户轻点实测 285~300ms,只剩 5~15ms 余量)。为了救它先后堆了四层补偿
// (自建轻点链 UP_TAP_CHAIN_MS 1800 / 挂起确认 PTT_CONFIRM_MS / 回弹假双击防御
// PTT_REBOUND_GUARD_MS / 驱动补报去重 clear_ms),用户实测仍然"别扭",阈值也被迫
// 一路抬到 500ms。
// 根因是手势组合本身,不是参数:
//   单击 + 长按 → 可共存(阈值分界,OK 键锁屏已真机验证);
//   单击 + 双击 → 必须等双击窗口才敢执行单击,要么迟滞 ~1.8s 要么误发;
//   长按 + 双击 → 最差,长按到点就宣布,提前把双击判死。
// 所以清空改挂 DOWN 长按(0.5s,与 OK 锁屏同阈值):DOWN 上「单击=回车 / 长按=
// 清空」是已验证可共存的那一组,零迟滞、零误发,四层补偿逻辑全部删除。
// 回弹与幽灵事件为什么不用再防:长按要求同一分压档连续保持 500ms —— 松手回弹时
// ADC 从 0 扫到 2890 会掠过 DOWN 档(150-447),但只是掠过,凑不出 500ms;射频腐蚀
// 实测是整段压到 0mV(落在 UP 档),同样到不了 DOWN 长按。剩下的残余风险由回调
// mv 判假兜住(见 PTT_FAKE_LONG_MV)。
// 不足这么久的"录音"不可能有语音(误碰/轻点):丢音频 + 收束会话,不进转写。
// ⚠ 不变量:PTT_MIN_TALK_MS 必须 ≥ 驱动的 long_press_time(bsp_button.c,现 500ms)。
// PTT 改成"按下即录"后(2026-08-29),短于长按阈值的一按事后还会被驱动补报
// SINGLE_CLICK(松开 + 双击窗口 ~180ms),而 UP 单击在 TRANSCRIBING 态是"退出转写"。
// 两者只要能同时成立,一次 400-500ms 的说话就会:进转写 → 紧随的补报单击把它撤掉。
// 取等号即让二者互斥:短于阈值 → 必然收束回 READY(READY 态 UP 单击无语义,补报
// 落地即无害);到达阈值 → 驱动只报 LONG_PRESS_UP,永不补报单击。
// (驱动判长按实测略早于名义阈值:500ms 配置下 428ms 即报 LONG —— 偏早的方向正好
//  落在安全侧,"没报 LONG"必然意味着按压确实短于 500ms。)
#define PTT_MIN_TALK_MS 500

// 唤醒防误锁窗口:息屏后长按 OK 唤醒时,PRESS 唤醒瞬间起 1s 内的 OK LONG
// 不触发锁定 —— 否则"唤醒"会立刻变成"再锁"(PRESS 后 ~500ms 即到 LONG)。
// LONG 长按期间只报一次,guard 只挡第一次;松手后再次长按 OK 正常锁定。
#define OK_LONG_GUARD_MS 1000

// OK 键"真实按压"电压区间(2026-08-29):OK 分压档是 447-1900mV
// (BSP_BTN_MV_TABLE),真机实测按住期间回调读数 598-599。锁定入口拿它判真假 ——
// 与 UP(PTT_UP_PRESS_MAX_MV)、DOWN(PTT_FAKE_LONG_MV)的门禁同一口径:真按压时
// 回调必然读到本档,幽灵事件读到的是别档或松开电平(2890)。
// 目前没有抓到过 OK 档的幽灵(射频腐蚀方向是整段压到 0mV,落在 UP 档),这条是
// 对称性补齐,不是已发生缺陷的修复。
// ⚠ 只用于"上锁",解锁那条路径故意不加门禁:门禁判错的代价不对称 —— 挡掉一次
//   幽灵上锁毫无损失,挡掉一次真解锁等于用户面对一块砖。
#define OK_PRESS_MIN_MV 447
#define OK_PRESS_MAX_MV 1900

// 幽灵事件判定(与各分支自己的 mv 门禁同口径,集中一处便于对账)。
// 只有"手指还在键上"的事件类型可判:PRESS 与 LONG 回调时刻按压仍在,mv 必落
// 本档;RELEASE/LONG_UP/CLICK/DOUBLE 回调时用户已松手,mv 恒 2890,无从判真假
// (真假单击靠 PRESS 时记下的 last_up_press_mv 区分)。
static bool key_ev_is_fake(const app_event_t *ev) {
    const uint8_t b = ev->u.key.btn;
    const uint16_t mv = ev->u.key.mv;
    if (b == APP_BTN_UP && (ev->type == APP_EV_KEY_PRESS || ev->type == APP_EV_KEY_LONG)) {
        return mv > PTT_UP_PRESS_MAX_MV;
    }
    if (b == APP_BTN_DOWN && ev->type == APP_EV_KEY_LONG) {
        return mv >= PTT_FAKE_LONG_MV;
    }
    if (b == APP_BTN_OK && ev->type == APP_EV_KEY_LONG) {
        return mv < OK_PRESS_MIN_MV || mv > OK_PRESS_MAX_MV;
    }
    return false;
}

static const char *const AGENT_STATE_NAMES[APP_AGENT_COUNT] = {
    [APP_AGENT_READY]    = "ready",
    [APP_AGENT_THINKING] = "thinking",
    [APP_AGENT_RUNNING]  = "running",
    [APP_AGENT_ERROR]    = "error",
    [APP_AGENT_DONE]     = "done",
};

void app_state_init(app_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = APP_ST_HOME;
    s->screen_on = true;
    s->panel_on = true;
    // 开机即按"未连接"渲染(无 Mac 时不会收到断开事件,初始 true 会一直误判为
    // 链路通 → PTT 可用但音频全丢且无离线横幅)。Mac 连入订阅 EVENT 后翻 true。
    s->ble_connected = false;
    s->link_up = false;
    s->link_channel = APP_CHAN_BLE;   // 缺省 BLE(首通道连接前 PTT 门禁靠 link_up)
    s->locked = false;             // 开机未锁定
    s->wake_ms = 0;                // 无唤醒史 → 首个 OK LONG 不受 guard 限制
    // 设置缺省(真机开机由 main.c 装载 NVS 值覆盖;宿主测试拿缺省即确定性基准)
    s->tone_level = TONE_LVL_DEFAULT;
    s->night_mute = 0;
    s->tz_hour = 8;                // 与 time_sync/nvs_settings 缺省一致(Asia/Shanghai)
}

void app_state_load_settings(app_state_t *s, uint8_t tone_level, uint8_t night_mute,
                             int8_t tz_hour) {
    s->tone_level = (tone_level < TONE_LVL_COUNT) ? tone_level : TONE_LVL_DEFAULT;
    s->night_mute = night_mute ? 1 : 0;
    s->tz_hour = (tz_hour >= -12 && tz_hour <= 12) ? tz_hour : 8;
}

static void emit(app_action_t *out, uint8_t *n, uint8_t max, app_action_t a) {
    if (*n < max) out[(*n)++] = a;
}

// 字符串安全拷贝:一律截断并保证 NUL。
static void str_cpy(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static void set_toast(app_state_t *s, uint64_t now_ms, const char *msg) {
    str_cpy(s->toast, sizeof(s->toast), msg);
    s->toast_until_ms = now_ms + 4000;
}

void app_state_snapshot(const app_state_t *s, uint64_t now_ms, app_ui_snapshot_t *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->state            = s->state;
    snap->link_up          = s->link_up;
    snap->screen_on        = s->screen_on;
    snap->panel_on         = s->panel_on;
    snap->net_busy         = s->net_busy;
    snap->ble_connected    = s->ble_connected;
    str_cpy(snap->link_name, sizeof(snap->link_name),
            s->link_channel == APP_CHAN_USB ? "USB"
            : "BLE");
    snap->elapsed_ms       = (uint32_t)(now_ms - s->state_since_ms);
    snap->battery_available = true; // 由主循环在快照后补真实值,见 app_ui
    str_cpy(snap->agent_message, sizeof(snap->agent_message), s->agent_message);
    snap->transcript_final = s->transcript_final;
    str_cpy(snap->agent_state_name, sizeof(snap->agent_state_name), s->agent_state_name);
    str_cpy(snap->task_id, sizeof(snap->task_id), s->task_id);
    str_cpy(snap->approval_title, sizeof(snap->approval_title), s->approval_title);
    str_cpy(snap->approval_target, sizeof(snap->approval_target), s->approval_target);
    str_cpy(snap->approval_diff, sizeof(snap->approval_diff), s->approval_diff);
    snap->approval_risk    = s->approval_risk;
    if (s->toast[0] && s->toast_until_ms > 0) {
        str_cpy(snap->toast, sizeof(snap->toast), s->toast);
    }
    // 设置页(2026-10-03):UI 按选中项渲染高亮与档位值
    snap->settings_sel = s->settings_sel;
    snap->tone_level   = s->tone_level;
    snap->night_mute   = s->night_mute;
    // 双固件 v2(2026-10-03):HOME 菜单行 + 槽 B 在位 + 录音中设置浮层
    snap->menu_sel         = s->menu_sel;
    snap->slot_b_present   = s->slot_b_present;
    snap->settings_overlay = s->settings_overlay;
    // 物理审批器 v2.4:选项选择页
    str_cpy(snap->ask_title, sizeof(snap->ask_title), s->ask_title);
    for (uint8_t i = 0; i < s->ask_count && i < APP_OPTS_MAX; i++) {
        str_cpy(snap->ask_opts[i], sizeof(snap->ask_opts[i]), s->ask_opts[i]);
    }
    snap->ask_count = s->ask_count;
    snap->ask_sel   = s->ask_sel;
    // 默认展示名仅在没有真实 agent.status 时兜底,不覆盖已存的 thinking/error 等
    if ((s->state == APP_ST_AGENT_RUNNING || s->state == APP_ST_TRANSCRIBING) &&
        snap->agent_state_name[0] == '\0') {
        str_cpy(snap->agent_state_name, sizeof(snap->agent_state_name),
                AGENT_STATE_NAMES[APP_AGENT_RUNNING]);
    }
}

// 按键动作(enter/clear):上行给 PC client 执行注入。
static void send_key_action(app_state_t *s, app_key_action_t action,
                            app_action_t *out, uint8_t *n, uint8_t max) {
    (void)s;
    app_action_t a = { .type = APP_ACT_SEND_KEY_ACTION };
    a.u.key_action.action = (uint8_t)action;
    emit(out, n, max, a);
}

// PTT 开始(音量加按下即触发):离线 toast+error 音;在线滴声+开流。
static void start_ptt(app_state_t *s, uint64_t now_ms,
                      app_action_t *out, uint8_t *n, uint8_t max) {
    if (!s->link_up) {
        set_toast(s, now_ms, "OFFLINE - PTT blocked");
        app_action_t t = { .type = APP_ACT_PLAY_TONE };
        t.u.tone = APP_TONE_ERROR;
        emit(out, n, max, t);
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, n, max, r);
    } else {
        // 分帧时序:滴声 → voice.start → 开流由 TONE_DONE 驱动(播放完成才采,
        // 避免 codec 分时冲突 + 滴声不被录进音频)。app_task 不同步阻塞播滴声;
        // sound_worker 优先级 5(高于 app_task/LVGL,2026-08-29 由 3 提升)保证
        // 这 80ms 立刻播完 —— 否则它排在 LISTENING 全屏刷新后面,开流要等 ~578ms。
        app_action_t t = { .type = APP_ACT_PLAY_TONE };
        t.u.tone = APP_TONE_START;
        emit(out, n, max, t);
        app_action_t v = { .type = APP_ACT_SEND_VOICE_START };
        emit(out, n, max, v);
        s->stream_started = false;
        s->state = APP_ST_LISTENING;
        s->state_since_ms = now_ms;
    }
}

// PTT 结束(音量加长按松开):停流 → voice.end → 转写。
// 不播发送音:用户要求转写期静音(2026-08-28)。
static void end_ptt(app_state_t *s, uint64_t now_ms,
                    app_action_t *out, uint8_t *n, uint8_t max) {
    app_action_t st = { .type = APP_ACT_STREAM_STOP };
    emit(out, n, max, st);
    app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
    emit(out, n, max, v);
    s->state = APP_ST_TRANSCRIBING;
    s->state_since_ms = now_ms;
    s->agent_state_name[0] = '\0';   // 新会话开始,清除旧 agent 状态
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

static void go_ready(app_state_t *s, uint64_t now_ms, app_action_t *out, uint8_t *n, uint8_t max) {
    s->state = APP_ST_READY;
    s->state_since_ms = now_ms;
    s->settings_overlay = 0;        // 防御:任何进 READY 的路径都收掉浮层(理论不可达)
    s->settings_return_home = 0;    // 设置去向标志随工作现场一起失效
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

// 回菜单首页(2026-10-03 v2.1):READY 单击 OK / 常开麦录音中长按 OK。
// mic_hold 不动 —— 设备端没有请求守护进程停麦的通道,录音会话继续;
// 菜单里迟到的转写文本按 HOME 门禁不上屏,回语音界面即恢复。守护进程
// 重连重发 mic on 时 HOME → READY → start_ptt 自动回到语音界面(闭环)。
static void go_home(app_state_t *s, uint64_t now_ms,
                    app_action_t *out, uint8_t *n, uint8_t max) {
    s->state = APP_ST_HOME;
    s->state_since_ms = now_ms;
    s->settings_overlay = 0;
    s->settings_return_home = 0;
    s->agent_state_name[0] = '\0';
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

// ---- 设置页(2026-10-03)----
// 进入:HOME 菜单选"设置"/READY 下 OK 双击(锁定态不进 —— 锁定 = 盲操作省电
// 模式,进入一个看不见的菜单只会留下"设备为什么不动了"的困惑)。
static void enter_settings(app_state_t *s, uint64_t now_ms,
                           app_action_t *out, uint8_t *n, uint8_t max) {
    s->state = APP_ST_SETTINGS;
    s->state_since_ms = now_ms;
    s->settings_sel = 0;
    s->settings_last_ms = now_ms;
    s->settings_overlay = 0;   // 全状态设置页与浮层互斥(浮层入口只在 LISTENING)
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

// 退出:去向按入口分流(2026-10-03 v2.1)—— 从菜单进的回菜单首页,从 READY
// 双击进的回工作页;浮层(录音中设置)退出 = 只清标志回录音现场。
static void exit_settings(app_state_t *s, uint64_t now_ms,
                          app_action_t *out, uint8_t *n, uint8_t max) {
    if (s->settings_overlay) {
        s->settings_overlay = 0;
    } else if (s->settings_return_home) {
        s->settings_return_home = 0;
        s->state = APP_ST_HOME;
        s->state_since_ms = now_ms;
    } else {
        s->state = APP_ST_READY;
        s->state_since_ms = now_ms;
    }
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

// 持久化动作:三元组整包落地(NVS + tz + 声音策略在 main.c 执行)。
// 幂等:值没变也照发 —— NVS set 同值不磨损新页,换取路径单一。
static void save_settings(app_state_t *s, app_action_t *out, uint8_t *n, uint8_t max) {
    app_action_t a = { .type = APP_ACT_SAVE_SETTINGS };
    a.u.settings.tone_level = s->tone_level;
    a.u.settings.night_mute = s->night_mute;
    a.u.settings.tz_hour    = s->tz_hour;
    emit(out, n, max, a);
}

// 切提示音档位(HIGH→LOW→OFF→HIGH)并持久化 + toast 反馈当前档。
// 两条路径共用:设置页 OK 单击(选中 Sound 项)/ 常开麦时 UP 双击快捷手势。
static void cycle_tone_level(app_state_t *s, uint64_t now_ms,
                             app_action_t *out, uint8_t *n, uint8_t max) {
    s->tone_level = (uint8_t)tone_policy_next((tone_lvl_t)s->tone_level);
    char msg[32];
    snprintf(msg, sizeof(msg), "Sound: %s", tone_policy_name((tone_lvl_t)s->tone_level));
    set_toast(s, now_ms, msg);
    save_settings(s, out, n, max);
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

static void abort_to_ready(app_state_t *s, uint64_t now_ms, const char *toast,
                           app_action_t *out, uint8_t *n, uint8_t max) {
    if (toast) set_toast(s, now_ms, toast);
    s->state = APP_ST_READY;
    s->state_since_ms = now_ms;
    s->settings_overlay = 0;        // 断链/中止收束浮层(录音现场已不存在)
    s->settings_return_home = 0;
    s->agent_state_name[0] = '\0';   // 清除旧的 agent 状态,防止 UI 残留
    app_action_t r = { .type = APP_ACT_UI_REFRESH };
    emit(out, n, max, r);
}

// 物理审批器共享(审批页 + 选项页,2026-10-03):弹窗必须被看见 ——
// 强制解锁 + 面板上电 + 亮背光 + 录音收束(管线不泄漏)。
static void prompt_wake(app_state_t *s, uint64_t now_ms,
                        app_action_t *out, uint8_t *out_n, uint8_t max) {
    if (s->locked) {
        s->locked = false;
    }
    if (!s->panel_on) {
        s->panel_on = true;
        app_action_t p = { .type = APP_ACT_UI_PANEL_ON };
        emit(out, out_n, max, p);
    }
    if (!s->screen_on) {
        s->screen_on = true;
        s->last_key_ms = now_ms;   // 唤醒即重置息屏计时(弹窗态本就常亮)
        app_action_t sc = { .type = APP_ACT_UI_SCREEN_ON };
        emit(out, out_n, max, sc);
    }
    if (s->state == APP_ST_LISTENING) {
        app_action_t st = { .type = APP_ACT_STREAM_STOP };
        emit(out, out_n, max, st);
        app_action_t ve = { .type = APP_ACT_SEND_VOICE_END };
        emit(out, out_n, max, ve);
    }
}

// 设置键公共语义(2026-10-03 v2):全状态设置页与录音浮层共用同一套 ——
// UP/DOWN 单击选行(环绕),OK 单击切选中项值。返回 true = 事件被消费。
// OK 长按退出 / 10s 无操作自动退出由调用方处理。
static bool settings_click(app_state_t *s, const app_event_t *ev, uint64_t now_ms,
                           app_action_t *out, uint8_t *n, uint8_t max) {
    if (ev->type != APP_EV_KEY_CLICK) return false;
    const uint8_t b = ev->u.key.btn;
    bool acted = true;
    if (b == APP_BTN_UP) {
        s->settings_sel = (uint8_t)((s->settings_sel + 2) % 3);   // 环绕上移
    } else if (b == APP_BTN_DOWN) {
        s->settings_sel = (uint8_t)((s->settings_sel + 1) % 3);   // 环绕下移
    } else if (b == APP_BTN_OK) {
        switch (s->settings_sel) {
        case 0:   // Sound: HIGH → LOW → OFF 循环
            cycle_tone_level(s, now_ms, out, n, max);
            break;
        case 1:   // Night Mute 21:30-7: 开 ↔ 关
            s->night_mute = !s->night_mute;
            save_settings(s, out, n, max);
            break;
        default:  // Timezone: +1 环绕 ±12
            s->tz_hour = (int8_t)(s->tz_hour >= 12 ? -12 : s->tz_hour + 1);
            save_settings(s, out, n, max);
            break;
        }
    } else {
        acted = false;
    }
    if (acted) {
        s->settings_last_ms = now_ms;
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, n, max, r);
    }
    return acted;
}

static void handle_key(app_state_t *s, const app_event_t *ev, uint64_t now_ms,
                       app_action_t *out, uint8_t *n, uint8_t max) {
    const uint8_t b = ev->u.key.btn;
    // 记录 UP 键最近一次 PRESS 回调读数(2026-08-28):真实按压 3-5mV(手指在
    // 键上),假按(射频腐蚀)2890mV(无人按键)。CLICK 回调时刻用户已松手,mv
    // 恒 2890 —— 真假单击只能从 PRESS 读数区分(TRANSCRIBING 退出判定用)。
    if (ev->type == APP_EV_KEY_PRESS && b == APP_BTN_UP) {
        s->last_up_press_mv = ev->u.key.mv;
    }
    // 息屏 = 自动息屏的省电显示态(非锁定):按键先恢复显示,事件照常放行执行。
    // 锁定态见下方 locked 门禁:操作照常放行但不亮屏(与自动息屏的唤醒是两回事)。
    const bool screen_was_off = !s->screen_on;

    // 锁定态(2026-08-28 语义变更):长按 OK 解锁亮屏;其余按键照常执行但不
    // 亮屏 —— 锁定 = 屏幕保持关闭的省电模式,不是输入锁(用户要求:息屏后仍
    // 能长按说话/回车/清空,口袋盲操作)。渲染由 snapshot 门禁按 screen_on
    // 跳过,操作产生的动作(语音/按键上行)照常出队。
    if (s->locked) {
        if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK) {
            s->locked = false;
            s->panel_on = true;
            s->screen_on = true;
            s->last_key_ms = now_ms;   // 解锁后息屏计时重新开始
            app_action_t p = { .type = APP_ACT_UI_PANEL_ON };
            emit(out, n, max, p);
            app_action_t a = { .type = APP_ACT_UI_SCREEN_ON };
            emit(out, n, max, a);
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
            return;
        }
        // 其余事件放行:跳过下方唤醒与锁定入口,直接落到长按清空/状态机。
        // 不亮屏不唤醒;锁定仅发生在 HOME/READY,盲操作开录进 LISTENING 后
        // 再长按 OK 仍是解锁(上方分支先行),录音不受影响。
    } else {
        if (screen_was_off) {
            // 幽灵事件不唤醒(2026-08-29):假 UP PRESS 会白亮 20s 屏
            // (下方状态机分支有 mv 门禁不会误开录音,但屏已经亮了)。
            if (ev->type == APP_EV_KEY_PRESS && !key_ev_is_fake(ev)) {
                if (!s->panel_on) {
                    s->panel_on = true;
                    app_action_t p = { .type = APP_ACT_UI_PANEL_ON };
                    emit(out, n, max, p);
                }
                s->screen_on = true;
                s->last_key_ms = now_ms;
                s->wake_ms = now_ms;   // 唤醒时刻(OK_LONG_GUARD:防"唤醒即锁")
                app_action_t a = { .type = APP_ACT_UI_SCREEN_ON };
                emit(out, n, max, a);
                app_action_t r = { .type = APP_ACT_UI_REFRESH };
                emit(out, n, max, r);
            }
            // 不 return:事件继续放行到下方正常逻辑(锁定入口 → 长按清空 → 状态机)。
            // 自动息屏只省显示,不吞按键事件(过修修正:息屏后 UP/DOWN 应唤醒并照常
            // 执行)。放行安全性:
            //  - PRESS 仅 READY 态有分支(= 开录音,2026-08-29 按下即录):息屏下
            //    按住 UP 会"唤醒 + 立刻开录",与锁定态盲操作说话语义一致;
            //  - 息屏只可能发生在 HOME/READY(idle_state 限定;APPROVAL 常亮、
            //    LISTENING 不超时)→ 放行事件只落到 HOME/READY 分支;
            //  - 唤醒后 1s 内 OK LONG 被 OK_LONG_GUARD 挡(防"唤醒即锁");
            //  - 无 PRESS 直接 CLICK(真实事件流不可达,iot_button 任何上报前必有
            //    PRESS)不唤醒但照常执行动作 —— 仅测试可达,非真实交互路径。
        }

        // 锁定入口:亮屏的 READY(或 HOME 菜单非"切固件"行)下长按 OK(0.5s
        // 阈值)立即锁定息屏(背光灭 + 面板 SLPIN 断电,不等 60s 超时)。录音/
        // 转写/审批/Agent 运行中不可锁定;USB 模式手动锁定允许(显式操作,省电
        // 优先)。菜单 FW 行例外:v2 起 OK 长按在该行 = 切固件(重启级操作独占
        // 长按手势,与锁屏二选一;想锁屏先挪一行)。
        // 幽灵长按拦截(2026-08-29 对称性补齐,见 OK_PRESS_MIN_MV):回调时刻
        // mv 不在 OK 档 = 无人按键。上锁挡错的代价是"少锁一次",可以承受。
        if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK &&
            !key_ev_is_fake(ev) &&
            (s->state == APP_ST_READY ||
             (s->state == APP_ST_HOME && s->menu_sel != 2)) &&
            s->screen_on && s->panel_on &&
            now_ms - s->wake_ms > OK_LONG_GUARD_MS) {
            s->locked = true;
            s->screen_on = false;
            s->panel_on = false;
            app_action_t a = { .type = APP_ACT_UI_SCREEN_OFF };
            emit(out, n, max, a);
            app_action_t p = { .type = APP_ACT_UI_PANEL_OFF };
            emit(out, n, max, p);
            return;
        }
    }

    // 录音中设置浮层(2026-10-03 v2):常开麦录音时双击 OK 呼出,录音继续。
    // 拦截放在 DOWN 长按全局分支之前 —— 浮层内 DOWN 长按 = 退出浮层,不是清空。
    // 键位与设置页完全一致(settings_click 公共语义);退出 = 只清标志,state 仍
    // LISTENING,录音/上行全程不动。入口门禁 mic_hold:PTT 录音按住 UP 时另一只
    // 手双击 OK 的场景不存在(PTT 秒级),且浮层若在 PTT 中开启,UP 松开事件会被
    // 浮层吞掉导致录音悬死 —— 入口收紧 + 下方防御双保险。
    if (s->settings_overlay) {
        if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_DOWN) {
            exit_settings(s, now_ms, out, n, max);   // DOWN 长按 = 退出(替代清空)
            return;
        }
        if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK && !key_ev_is_fake(ev)) {
            exit_settings(s, now_ms, out, n, max);   // OK 长按 = 退出
            return;
        }
        // 防御(入口门禁下理论不可达):PTT 录音中浮层未关,UP 松开先收浮层,
        // 松开事件继续落到 LISTENING 正常收口(end_ptt / 误触收束),不 return。
        if (!s->mic_hold && b == APP_BTN_UP &&
            (ev->type == APP_EV_KEY_LONG_UP || ev->type == APP_EV_KEY_RELEASE)) {
            s->settings_overlay = 0;
        } else {
            if (ev->type == APP_EV_KEY_CLICK) {
                settings_click(s, ev, now_ms, out, n, max);
            }
            return;   // 浮层消费其余一切事件(PRESS/DOUBLE/LONG_UP 不漏到下层)
        }
    }

    // DOWN(音量减)长按 0.5s = 清空输入框(全局语义,各态统一,锁定/息屏下也生效)。
    // 设置页内例外(2026-10-03):长按 = 退出设置 —— "清空输入框"对一个菜单页没有
    // 意义,而设置页需要一条低延迟退出路径(与 OK 长按双出口)。
    // 与同一颗键的"单击 = 回车"天然互斥:iot_button 一旦判成长按,这一按就只报
    // LONG_PRESS_START/LONG_PRESS_UP,不再补报 SINGLE_CLICK —— 想清空的时候绝不会
    // 先把内容发出去(这正是"清空放双击"做不到的一点,见文件头键位注释)。
    // 幽灵长按拦截:回调时刻 mv 已回松开电平 = 无人按键(射频腐蚀残留),丢弃。
    // 长按到点即清空并给一声提示音 —— 用户无法凭手感知道 500ms 到了没有,和
    // 开录音的滴声同理(这是阈值确认音,不是状态音,不违反"转写期静音")。
    if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_DOWN) {
        if (key_ev_is_fake(ev)) {   // 等价于 mv >= PTT_FAKE_LONG_MV,口径集中在一处
            return;
        }
        if (s->state == APP_ST_SETTINGS) {
            exit_settings(s, now_ms, out, n, max);
            return;
        }
        send_key_action(s, APP_KEY_CLEAR, out, n, max);
        app_action_t t = { .type = APP_ACT_PLAY_TONE };
        t.u.tone = APP_TONE_REJECT;   // 220Hz/120ms,清空确认
        emit(out, n, max, t);
        return;
    }

    switch (s->state) {
    case APP_ST_HOME: {
        // 菜单首页(2026-10-03 v2):0=语音输入 1=设置 2=切固件(行常驻,
        // 槽 B 空时行值显示 "--",长按给 toast,不隐藏 —— 导航逻辑恒 3 行最简)。
        if (ev->type == APP_EV_KEY_CLICK && (b == APP_BTN_UP || b == APP_BTN_DOWN)) {
            const int dir = (b == APP_BTN_DOWN) ? 1 : 2;   // +1 / -1 (mod 3)
            s->menu_sel = (uint8_t)((s->menu_sel + dir) % 3);
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_OK) {
            if (s->menu_sel == 0) {
                go_ready(s, now_ms, out, n, max);          // 语音输入(原 HOME→READY)
            } else if (s->menu_sel == 1 && !s->locked) {
                s->settings_return_home = 1;               // 退出设置时回菜单
                enter_settings(s, now_ms, out, n, max);    // 设置(锁定态不进,v1 规则)
            }
            // menu_sel==2(切固件)OK 单击故意无动作:切换 = 重启级操作,
            // 只认长按(下方),单击失手不会把用户甩进另一个固件。
        } else if (ev->type == APP_EV_KEY_DOUBLE && b == APP_BTN_OK && !s->locked) {
            // 菜单内 OK 双击 = 直接进设置(2026-10-03 v2.1):保住"双击 OK =
            // 设置"的全态心智 —— READY 双击的第一击先回菜单,双击事件落在
            // 菜单,在这里收口(真实固件流多一次页面切换,键位表零变化)。
            // 锁定态不进(与 READY 双击、菜单行 1 同一规则)。
            enter_settings(s, now_ms, out, n, max);
            s->settings_return_home = 1;
        } else if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK &&
                   !key_ev_is_fake(ev) && s->menu_sel == 2) {
            if (s->slot_b_present) {
                app_action_t f = { .type = APP_ACT_FW_SWITCH };
                f.u.fw_switch.slot = 1;   // → ota_1(main.c 写 otadata + 重启)
                emit(out, n, max, f);
            } else {
                set_toast(s, now_ms, "Slot B: empty");
                app_action_t t = { .type = APP_ACT_PLAY_TONE };
                t.u.tone = APP_TONE_ERROR;
                emit(out, n, max, t);
                app_action_t r = { .type = APP_ACT_UI_REFRESH };
                emit(out, n, max, r);
            }
        }
        break;
    }

    case APP_ST_READY:
        if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_DOWN) {
            send_key_action(s, APP_KEY_ENTER, out, n, max);
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_OK) {
            // OK 单击 = 回菜单首页(2026-10-03 v2.1):菜单成了启动器,READY
            // 必须有回去的路(此前只能开机见一次)。
            go_home(s, now_ms, out, n, max);
        } else if (ev->type == APP_EV_KEY_DOUBLE && b == APP_BTN_OK && !s->locked) {
            // OK 双击进设置(2026-10-03)。宿主直发 DOUBLE 走这里(测试路径);
            // 真实固件流单击先行先回菜单,双击在菜单落地进设置 —— 殊途同归,
            // 设置退出去向两者都标 return_home=0/1 各自入口(见 exit_settings)。
            enter_settings(s, now_ms, out, n, max);
            s->settings_return_home = 0;
        } else if (ev->type == APP_EV_KEY_PRESS && b == APP_BTN_UP) {
            // 音量加"按下即录"(2026-08-29,取代原先等 0.5s 长按判定):真机实测
            // 按下到真正 `采集开始` 要 ~1.01s —— 428ms 花在长按阈值上,另外 578ms
            // 花在"滴声播完才开流"的门禁上(滴声只有 80ms,其余是 sound_worker
            // prio 3 被 app_task/LVGL 全屏刷新挤在后面,已随本次改动提到 prio 5)。
            // 用户张嘴即被吞掉第一秒,故这里不再等阈值:按下就开会话。
            // 短按的收口在 LISTENING 的 PTT_MIN_TALK_MS(与驱动长按阈值取等,见
            // 该常量注释):误碰 <500ms 松手 → 丢音频 + 收束回 READY。
            // 驱动仍保留 500ms 长按阈值 —— UP 单击在 APPROVAL(拒绝)/TRANSCRIBING
            // (退出)两态还有语义,READY 态本就没有单击语义,故互不干扰。
            // 连接握手期假按抑制:密集射频腐蚀 → 假 UP 事件,窗口内吞掉。
            if (s->ble_connected && now_ms - s->ble_connect_ms < BLE_CONNECT_PTT_GUARD_MS) {
                break;
            }
            // 假按抑制(2026-08-28 真机取证,替代固定 3s 窗口):PTT 松开
            // 后 BLE 链路异常期 SAR ADC 被腐蚀 → 假 PRESS→LONG 序列,落在
            // READY 态即触发 PTT(第二声滴 + 假录音)。iot_button 判定按住
            // 是腐蚀中读低压,但 on_key 回调时刻腐蚀已过 → mv 读回松开电平
            // (2890;真实按压 3-5)。环取证 7 例假 LONG 回调 mv 全 2890、真
            // 实 LONG 全 <150 —— 100% 区分。不能用固定窗口:用户连续长按
            // 间隔实测 2-3s,3s 窗口把 43.17s/50.12s 真实长按吞掉("长按没
            // 反应"根因),故以回调 mv 判假,任何间隔的真实长按都不误伤。
            if (ev->u.key.mv > PTT_UP_PRESS_MAX_MV) {
                break;
            }
            start_ptt(s, now_ms, out, n, max);
        }
        break;

    case APP_ST_LISTENING:
        // 常开麦快捷手势(2026-10-03):UP(音量+)双击 = 循环切换提示音档位。
        // 只在 mic_hold 下开放 —— 常开麦期间物理 UP 的全部事件本就被吞(键完全
        // 空闲),双击是白捡的快捷键;普通 PTT 录音中 UP 被按下即录占用,双击的
        // 第一按就会开/收一次录音,此手势必须缺席(下方 break 前的门禁)。
        // 锁定态放行:夜间盲操作切静音正是本功能的主场景(锁定 = 息屏省电,不是输入锁)。
        if (s->mic_hold && ev->type == APP_EV_KEY_DOUBLE && b == APP_BTN_UP) {
            cycle_tone_level(s, now_ms, out, n, max);
            break;
        }
        // 录音中设置浮层入口(2026-10-03 v2):OK 双击呼出设置,录音继续不收口。
        // OK 单击在 LISTENING 全态无动作 → 双击叠加零误触(键位注释例外条款);
        // 锁定态放行:浮层照常呼出,渲染由 screen_on 门禁跳过 —— 盲操作改设置
        // 与"锁定下切音量档"同语义,口袋场景明确要的就是这个。
        if (s->mic_hold && ev->type == APP_EV_KEY_DOUBLE && b == APP_BTN_OK) {
            s->settings_overlay = 1;
            s->settings_sel = 0;
            s->settings_last_ms = now_ms;
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, n, max, r);
            break;
        }
        // OK 长按 = 回菜单首页(2026-10-03 v2.1):录音继续不收口(设备端没有
        // 请求守护进程停麦的通道,mic_hold 不动),菜单行 0 / mic on 重握手随时
        // 回语音界面。浮层打开时 OK 长按已被上方浮层拦截(退出浮层),不竞争。
        if (s->mic_hold && ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK &&
            !key_ev_is_fake(ev)) {
            set_toast(s, now_ms, "Menu (mic on)");
            go_home(s, now_ms, out, n, max);
            break;
        }
        // 松开立即结束并发送(无取消窗口)。按下即录之后两种松开事件都要收:
        // 超过驱动 500ms 阈值的一按报 LONG_PRESS_UP(随后还有一条 RELEASE),
        // 短按只报 RELEASE。谁先到谁结束,后到的那条已落在别的状态里被忽略。
        // 录音期间清空不可达:三档分压键同时按下 UP+DOWN 只会读出更低的那一档,
        // DOWN 长按压根形成不了。
        if ((ev->type == APP_EV_KEY_LONG_UP || ev->type == APP_EV_KEY_RELEASE) &&
            b == APP_BTN_UP) {
            // 常开麦克风模式(mic_hold):物理 UP 松开不收口 —— 守护进程"虚拟
            // 按住"期间用户碰一下实体键不应掐断直播音频;mic 只听 `mic off`。
            if (s->mic_hold) {
                break;
            }
            // 误触收口:不足 PTT_MIN_TALK_MS 就松手 = 误碰或轻点,里面不可能有
            // 语音。丢掉音频、把会话收束回 READY,不进转写。
            if (now_ms - s->state_since_ms < PTT_MIN_TALK_MS) {
                app_action_t c = { .type = APP_ACT_STREAM_CANCEL };
                emit(out, n, max, c);   // 幂等:滴声未播完时甚至还没开流
                app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
                emit(out, n, max, v);   // 会话必须收束,否则 Mac 侧挂着未结束的 voice
                go_ready(s, now_ms, out, n, max);
                break;
            }
            end_ptt(s, now_ms, out, n, max);
        }
        break;

    case APP_ST_APPROVAL:
        if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_OK) {
            app_action_t a = { .type = APP_ACT_SEND_AGENT_ACTION };
            str_cpy(a.u.agent_action.task_id, sizeof(a.u.agent_action.task_id), s->task_id);
            a.u.agent_action.decision = APP_ACTION_APPROVE;
            emit(out, n, max, a);
            if (s->approval_ext) {
                // 物理审批器:外部请求没有 agent 会话,落 AGENT_RUNNING 会
                // 熄麦卡屏(看门狗 mic on 在该态被忽略)→ 回 READY,决策
                // 上行照发,daemon 收到后即解锁等待中的 HTTP 请求。
                s->approval_ext = 0;
                abort_to_ready(s, now_ms, "Approved", out, n, max);
            } else {
                s->state = APP_ST_AGENT_RUNNING;
                s->state_since_ms = now_ms;
                str_cpy(s->agent_message, sizeof(s->agent_message), "Approved, agent continues...");
                s->transcript_final = true;   // 非转写文本,无预览光标
                app_action_t r = { .type = APP_ACT_UI_REFRESH };
                emit(out, n, max, r);
            }
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_UP) {
            app_action_t a = { .type = APP_ACT_SEND_AGENT_ACTION };
            str_cpy(a.u.agent_action.task_id, sizeof(a.u.agent_action.task_id), s->task_id);
            a.u.agent_action.decision = APP_ACTION_REJECT;
            emit(out, n, max, a);
            app_action_t t = { .type = APP_ACT_PLAY_TONE };
            t.u.tone = APP_TONE_REJECT;
            emit(out, n, max, t);
            if (s->approval_ext) {
                s->approval_ext = 0;
                abort_to_ready(s, now_ms, "Rejected", out, n, max);
            } else {
                s->state = APP_ST_AGENT_RUNNING;
                s->state_since_ms = now_ms;
                str_cpy(s->agent_message, sizeof(s->agent_message), "Rejected by user");
                s->transcript_final = true;   // 非转写文本,无预览光标
                app_action_t r = { .type = APP_ACT_UI_REFRESH };
                emit(out, n, max, r);
            }
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_DOWN) {
            send_key_action(s, APP_KEY_ENTER, out, n, max);
        }
        break;

    case APP_ST_ASK:
        // 物理审批器 v2.4:UP/DOWN 移动高亮(夹紧不环绕,选项少时直观),
        // OK 单击确认当前项,OK 长按拒绝整题。决策后一律回 READY(外部请求
        // 无 agent 会话,AGENT_RUNNING 会熄麦卡屏 —— 同审批 src=ext 语义)。
        if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_UP) {
            if (s->ask_sel > 0) {
                s->ask_sel--;
                emit(out, n, max, (app_action_t){ .type = APP_ACT_UI_REFRESH });
            }
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_DOWN) {
            if (s->ask_sel + 1 < s->ask_count) {
                s->ask_sel++;
                emit(out, n, max, (app_action_t){ .type = APP_ACT_UI_REFRESH });
            }
        } else if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_OK) {
            app_action_t a = { .type = APP_ACT_SEND_AGENT_ACTION };
            str_cpy(a.u.agent_action.task_id, sizeof(a.u.agent_action.task_id), s->task_id);
            a.u.agent_action.decision = APP_ACTION_CHOOSE;
            a.u.agent_action.option = s->ask_sel;
            emit(out, n, max, a);
            s->ask_count = 0;
            abort_to_ready(s, now_ms, NULL, out, n, max);
            // toast 显示所选选项文本(中文;UI 侧 toast 字体已换 CJK)
            set_toast(s, now_ms, s->ask_opts[s->ask_sel]);
        } else if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK) {
            app_action_t a = { .type = APP_ACT_SEND_AGENT_ACTION };
            str_cpy(a.u.agent_action.task_id, sizeof(a.u.agent_action.task_id), s->task_id);
            a.u.agent_action.decision = APP_ACTION_REJECT;
            emit(out, n, max, a);
            app_action_t t = { .type = APP_ACT_PLAY_TONE };
            t.u.tone = APP_TONE_REJECT;
            emit(out, n, max, t);
            s->ask_count = 0;
            abort_to_ready(s, now_ms, "Canceled", out, n, max);
        }
        break;

    case APP_ST_TRANSCRIBING:
        // 音量+单击:退出转写场景(2026-08-28 用户需求)——不等识别结果,直接
        // 回 READY。迟到的识别文本由 APP_EV_TRANSCRIPT 的 READY/HOME 门禁丢弃。
        // 真实单击的 PRESS 回调 mv=3-5(手指在键上);假按的 PRESS 读数落在别的
        // 分压档(2890 松开电平 / 598 跨档)→ 用 PRESS 读数区分真假单击(CLICK
        // 回调时刻用户已松手,mv 恒 2890,不可用)。按下即录不影响这里:转写态
        // 没有 PRESS 分支,新会话只能从 READY 起。
        if (ev->type == APP_EV_KEY_CLICK && b == APP_BTN_UP &&
            s->last_up_press_mv <= PTT_UP_PRESS_MAX_MV) {
            abort_to_ready(s, now_ms, NULL, out, n, max);
        }
        break;

    case APP_ST_SETTINGS:
        // 设置页键位(2026-10-03 v2):VOL± 选行、OK 切值(settings_click
        // 公共语义,录音浮层同款);OK 长按 = 退出;DOWN 长按退出在上方全局
        // 分支(浮层块之后,设置全状态仍命中);10s 无操作自动退出(handle_tick)。
        // DOWN 单击在设置内是"向下选行"而非回车上行 —— 设置页不注入按键。
        if (ev->type == APP_EV_KEY_LONG && b == APP_BTN_OK && !key_ev_is_fake(ev)) {
            // OK 长按退出:锁屏入口限 HOME/READY(见上方门禁),设置内长按空闲,
            // 收作退出 —— 幽灵门禁与上锁同口径(挡错代价 = 多按一次,可承受)。
            exit_settings(s, now_ms, out, n, max);
            break;
        }
        settings_click(s, ev, now_ms, out, n, max);
        break;

    default:
        // AGENT_RUNNING:按键全部忽略(仅唤醒已在上面处理)
        break;
    }
}

static void handle_tick(app_state_t *s, uint64_t now_ms, app_action_t *out, uint8_t *n, uint8_t max) {
    // toast 过期
    if (s->toast[0] && now_ms >= s->toast_until_ms) {
        s->toast[0] = '\0';
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, n, max, r);
    }

    // 设置页无操作自动退出(2026-10-03):回 READY 工作页。先于息屏判定 ——
    // 10s 退出 < 20s 息屏,正常路径永远先离开设置再谈省电。
    // 录音浮层(2026-10-03 v2)同计时:超时只收浮层回录音现场(exit_settings
    // 按 settings_overlay 分流),state 保持 LISTENING。
    if ((s->state == APP_ST_SETTINGS || s->settings_overlay) &&
        now_ms - s->settings_last_ms >= APP_SETTINGS_IDLE_EXIT_MS) {
        exit_settings(s, now_ms, out, n, max);
    }

    // 分级息屏:HOME/READY/SETTINGS 下无按键(APPROVAL 保持常亮:安全审批不熄屏)
    // 20s → 关背光(渲染跳过,面板冻结最后一帧);60s → 面板 SLPIN 断电(μA 级)。
    // 背光关后 tick 仍需走到面板判定,故不提前 return。
    // USB 主机在位(有线供电)不熄屏:屏幕常亮,省电只针对无线场景(用户需求)。
    // 锁定息屏为显式操作(locked 时 screen_on/panel_on 已 false,超时分支
    // 本不会触发),此处防御性排除使意图明确。
    const bool idle_state = !mode_wired() &&
                            !s->locked &&
                            (s->state == APP_ST_HOME ||
                             s->state == APP_ST_READY ||
                             s->state == APP_ST_SETTINGS);
    if (idle_state) {
        if (s->screen_on && (now_ms - s->last_key_ms) >= APP_IDLE_BACKLIGHT_OFF_MS) {
            s->screen_on = false;
            app_action_t a = { .type = APP_ACT_UI_SCREEN_OFF };
            emit(out, n, max, a);
        }
        if (s->panel_on && (now_ms - s->last_key_ms) >= APP_IDLE_PANEL_OFF_MS) {
            s->panel_on = false;
            app_action_t a = { .type = APP_ACT_UI_PANEL_OFF };
            emit(out, n, max, a);
        }
    }

    if (!s->screen_on) return;

    // 状态超时
    const uint64_t elapsed = now_ms - s->state_since_ms;
    switch (s->state) {
    case APP_ST_LISTENING:
        // S3 兜底:滴声队列满/声音任务异常 → TONE_DONE 永不来的场景,超时强制开流
        // (宁可无滴声也不让 PTT 卡死无声)。
        if (!s->stream_started && elapsed >= APP_TONE_PENDING_TIMEOUT_MS) {
            app_action_t st = { .type = APP_ACT_STREAM_START };
            emit(out, n, max, st);
            s->stream_started = true;
        }
        // 说话时长兜底(2026-08-29 新增,见 APP_PTT_MAX_TALK_MS):松开事件丢失
        // 时 LISTENING 会永久滞留。到点按"正常松手"处理 —— 已录内容照常进转写,
        // 不给错误提示(真人一口气到不了 60s,正常路径不可达)。
        // 常开麦克风模式(mic_hold,2026-10-02):会话本来就预期无限长,跳过本
        // 兜底;收束只由 `mic off` / 链路断开触发(守护进程有看门狗自动重开)。
        else if (!s->mic_hold && elapsed >= APP_PTT_MAX_TALK_MS) {
            end_ptt(s, now_ms, out, n, max);
        }
        break;
    case APP_ST_TRANSCRIBING:
        if (elapsed >= APP_TRANSCRIBE_TIMEOUT) {
            abort_to_ready(s, now_ms, "STT timeout", out, n, max);
        }
        break;
    case APP_ST_AGENT_RUNNING:
        if (elapsed >= APP_AGENT_RUN_TIMEOUT) {
            abort_to_ready(s, now_ms, "Agent timeout", out, n, max);
        }
        break;
    default:
        break; // APPROVAL 无超时(安全要求)
    }
}

// 链路断开(BLE 断连或 EVENT 订阅取消):兜底停流 + 状态回退。
// 审批保持:等待 Mac 重连后重发请求。
static void handle_link_down(app_state_t *s, uint64_t now_ms, const char *toast,
                             app_action_t *out, uint8_t *n, uint8_t max) {
    // 常开麦克风模式随链路一起收束:链路断 = Mac 端守护进程已不在,
    // mic_hold 残留会让下一次 `mic on` 幂等误判(LISTENING 误吞)。
    s->mic_hold = false;
    // CANCEL:断链时无 Mac 可发,清残留防断链前的帧流入下一次会话
    app_action_t st = { .type = APP_ACT_STREAM_CANCEL };
    emit(out, n, max, st);   // 幂等,未开流时执行器无副作用
    set_toast(s, now_ms, toast);   // 任何状态都提示断开(审批保持等场景也可见)
    switch (s->state) {
    case APP_ST_LISTENING:
        abort_to_ready(s, now_ms, NULL, out, n, max);   // toast 已设,不再覆盖
        break;
    case APP_ST_TRANSCRIBING:
    case APP_ST_AGENT_RUNNING:
        abort_to_ready(s, now_ms, NULL, out, n, max);
        break;
    default: {
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, n, max, r);
        break;
    }
    }
}

void app_state_reduce(app_state_t *s, const app_event_t *ev, uint64_t now_ms,
                      app_action_t *out, uint8_t *out_n) {
    *out_n = 0;
    const uint8_t max = APP_ACT_MAX;

    switch (ev->type) {
    case APP_EV_KEY_PRESS:
    case APP_EV_KEY_RELEASE:
    case APP_EV_KEY_CLICK:
    case APP_EV_KEY_DOUBLE:
    case APP_EV_KEY_LONG:
    case APP_EV_KEY_LONG_UP:
        // 幽灵事件不算"用户活动":射频腐蚀造出的假按会把自动息屏计时一路推后
        // (真机环 130s 内两次),屏幕迟迟不熄 = 白耗电。事件本身照常放行 ——
        // 各分支自己的 mv 门禁负责不让它产生动作(口径见 key_ev_is_fake)。
        if (!key_ev_is_fake(ev)) {
            s->last_key_ms = now_ms;
        }
        handle_key(s, ev, now_ms, out, out_n, max);
        break;

    case APP_EV_AGENT_STATUS: {
        const uint8_t st = ev->u.agent_status.state;
        str_cpy(s->agent_state_name, sizeof(s->agent_state_name),
                AGENT_STATE_NAMES[st < APP_AGENT_COUNT ? st : APP_AGENT_READY]);
        str_cpy(s->agent_message, sizeof(s->agent_message), ev->u.agent_status.message);
        s->transcript_final = true;   // 消息已被 agent.status 替换,不再是转写预览

        if (s->state == APP_ST_TRANSCRIBING || s->state == APP_ST_AGENT_RUNNING) {
            if (st == APP_AGENT_RUNNING || st == APP_AGENT_THINKING) {
                s->state = APP_ST_AGENT_RUNNING;
                s->state_since_ms = now_ms;
            } else if (st == APP_AGENT_DONE) {
                // 无 DONE 页:识别完成直接回 READY 待命(用户:不要 done 提示),
                // 成功音一并取消(用户要求转写→ready 静音,2026-08-28)
                s->state = APP_ST_READY;
                s->state_since_ms = now_ms;
            } else if (st == APP_AGENT_ERROR) {
                abort_to_ready(s, now_ms, "Agent error", out, out_n, max);
            }
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        } else {
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;
    }

    case APP_EV_APPROVAL_REQUEST:
        // 息屏/锁定收到审批:强制解锁 + 亮屏 + 面板上电 —— 审批必须被看见,
        // 防口袋盲批(与"APPROVAL 常亮不熄屏"政策一致)。物理审批器(v2.2)
        // 的外部请求多发生在息屏待机时,curl 一发屏幕就亮。
        prompt_wake(s, now_ms, out, out_n, max);
        // 审批打断录音:必须先停流,否则管线永远泄漏(APPROVAL 下没有按键能停它)
        str_cpy(s->task_id, sizeof(s->task_id), ev->u.approval.task_id);
        str_cpy(s->approval_title, sizeof(s->approval_title), ev->u.approval.title);
        str_cpy(s->approval_target, sizeof(s->approval_target), ev->u.approval.target);
        str_cpy(s->approval_diff, sizeof(s->approval_diff), ev->u.approval.diff_summary);
        s->approval_risk = ev->u.approval.risk < APP_RISK_COUNT ? ev->u.approval.risk : APP_RISK_MEDIUM;
        s->approval_ext = ev->u.approval.ext ? 1 : 0;   // 物理审批器:决策去向分流
        s->state = APP_ST_APPROVAL;
        s->state_since_ms = now_ms;
        app_action_t t = { .type = APP_ACT_PLAY_TONE };
        t.u.tone = APP_TONE_APPROVAL;
        emit(out, out_n, max, t);
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, out_n, max, r);
        break;

    case APP_EV_ASK_REQUEST:
        // 物理审批器 v2.4:选项选择弹窗(多选一)。唤醒/打断语义与审批一致;
        // ASK 态天然继承 APPROVAL 的常亮(息屏 idle_state 不含它)与无超时
        // (handle_tick default 分支)—— 问题必须被回答。
        prompt_wake(s, now_ms, out, out_n, max);
        str_cpy(s->task_id, sizeof(s->task_id), ev->u.ask.task_id);
        str_cpy(s->ask_title, sizeof(s->ask_title), ev->u.ask.title);
        s->ask_count = ev->u.ask.opt_count;
        if (s->ask_count < 1) s->ask_count = 1;
        if (s->ask_count > APP_OPTS_MAX) s->ask_count = APP_OPTS_MAX;
        for (uint8_t i = 0; i < s->ask_count; i++) {
            str_cpy(s->ask_opts[i], sizeof(s->ask_opts[i]), ev->u.ask.opts[i]);
        }
        s->ask_sel = 0;
        s->state = APP_ST_ASK;
        s->state_since_ms = now_ms;
        emit(out, out_n, max, (app_action_t){ .type = APP_ACT_PLAY_TONE,
                                              .u.tone = APP_TONE_APPROVAL });
        emit(out, out_n, max, (app_action_t){ .type = APP_ACT_UI_REFRESH });
        break;

    case APP_EV_TRANSCRIPT:
        // 注入已迁 Mac 端,设备只做转写文本显示。退出转写(音量+单击回 READY)
        // 后迟到的识别文本丢弃:用户已明确退出该场景,预览/定稿不再上屏。
        // 正常路径文本到达时 state=TRANSCRIBING;agent 运行期文本照常显示。
        // final:false = 预览态(未定稿,UI 附光标感);final:true = 定稿落定。
        if (s->state == APP_ST_READY || s->state == APP_ST_HOME) {
            break;
        }
        str_cpy(s->agent_message, sizeof(s->agent_message), ev->u.transcript.text);
        s->transcript_final = ev->u.transcript.final;
        {
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;

    case APP_EV_AUDIO_DROP_START:
        if (!s->net_busy) {
            s->net_busy = true;
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;

    case APP_EV_AUDIO_DROP_END:
        if (s->net_busy) {
            s->net_busy = false;
            app_action_t r = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r);
        }
        break;

    case APP_EV_BLE_CONNECTED:
        // 语义:EVENT 特征已订阅(链路通,PTT 可用)。
        // 双通道常开(2026-08-28):BLE 连接总是链路候选 —— link_up 已通时
        // 只更新图标;空闲态(HOME/READY)记为新会话通道(最近使用),活动
        // 会话中不夺路(音频/voice.end 必须回到发起会话的通道)。
        s->ble_connect_ms = now_ms;   // 握手期假长按抑制计时起点(见 handle_key)
        s->ble_connected = true;
        if (!s->link_up) {
            s->link_channel = APP_CHAN_BLE;
            s->link_up = true;
        } else if (s->state == APP_ST_HOME || s->state == APP_ST_READY) {
            s->link_channel = APP_CHAN_BLE;
        }
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_BLE_DISCONNECTED:
        s->ble_connected = false;
        if (s->link_channel == APP_CHAN_BLE) {
            // 会话通道断开:收束会话(停流回 READY + toast),路由切到仍通
            // 的另一通道(USB)。双通道均断时 link_channel 保留断掉的通道名
            // (横幅显示),link_up=false。另一通道断开不影响本会话(上方
            // 分支不成立时仅刷新图标)。
            s->link_up = false;
            handle_link_down(s, now_ms, "Mac disconnected", out, out_n, max);
            if (mode_channel_up(APP_CHAN_USB)) {
                s->link_channel = APP_CHAN_USB;
                s->link_up = true;
            }
        }
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_BLE_DROP:
        set_toast(s, now_ms, "BLE event dropped");
        {
            app_action_t br = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, br);
        }
        break;

    case APP_EV_AUDIO_ERROR:
        // 采集/发送出错必须先把会话收束掉再回 READY(2026-08-29 修):原先只
        // set_toast + abort_to_ready,留下三个尾巴 ——
        //   ① 音频管线里的残留帧会流进下一次会话(token 失效只挡已开流的部分);
        //   ② Mac 侧挂着一个永不结束的 voice,ASR 流要等到 STT 超时才收;
        //   ③ STREAM_START 取的会话 PM 锁没人放,设备再也回不到低功耗门禁。
        // 两条动作都幂等(执行器侧 pm_session_unlock 亦有幂等守卫):滴声还没播完
        // 就出错时流其实没开,CANCEL 落地无副作用。
        if (s->state == APP_ST_LISTENING) {
            app_action_t c = { .type = APP_ACT_STREAM_CANCEL };
            emit(out, out_n, max, c);
            app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
            emit(out, out_n, max, v);
        }
        set_toast(s, now_ms, "Audio error");
        abort_to_ready(s, now_ms, NULL, out, out_n, max);
        break;

    case APP_EV_TONE_DONE:
        // START 提示音播放完成:开流(幂等)。非 LISTENING(误触收口/断链已离开)
        // 或流已开 → 忽略,保证"滴声先于采集"且取消期间不误开流。
        if (s->state == APP_ST_LISTENING && !s->stream_started) {
            app_action_t st = { .type = APP_ACT_STREAM_START };
            emit(out, out_n, max, st);
            s->stream_started = true;
        }
        break;

    case APP_EV_TICK:
        handle_tick(s, now_ms, out, out_n, max);
        break;

    case APP_EV_TIME_SET:
        // 校时落地:透传 epoch 到动作(app_task 执行 settimeofday)。
        // 与链路事件正交:任何模式下收到即生效,无状态依赖。
        {
            app_action_t t = { .type = APP_ACT_TIME_SET,
                               .u.time_set.epoch = ev->u.time_set.epoch };
            emit(out, out_n, max, t);
        }
        break;

    // ---- USB 有线通道(语义与 BLE 对等;双通道常开下同规则)----

    case APP_EV_USB_CONNECTED:
        // USB 会话通(PC ping 握手)= 链路候选:空闲态记为新会话通道(插线
        // 即用);活动会话中不夺路。与 BLE_CONNECTED 同构。
        if (!s->link_up) {
            s->link_channel = APP_CHAN_USB;
            s->link_up = true;
        } else if (s->state == APP_ST_HOME || s->state == APP_ST_READY) {
            s->link_channel = APP_CHAN_USB;
        }
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    case APP_EV_USB_DISCONNECTED:
        if (s->link_channel == APP_CHAN_USB) {
            s->link_up = false;
            // 状态收束与 BLE 断连同路径;toast 文案区分通道
            handle_link_down(s, now_ms, "USB disconnected", out, out_n, max);
            if (mode_channel_up(APP_CHAN_BLE)) {
                s->link_channel = APP_CHAN_BLE;
                s->link_up = true;
            }
        }
        {
            app_action_t b = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, b);
        }
        break;

    // ---- 常开麦克风模式(console `mic on|off` 经 SYS 帧下行,2026-10-02)----

    case APP_EV_MIC_ON:
        // HOME/SETTINGS 先进 READY(RESET 后设备停在 HOME、设置是低频临时页,
        // 守护进程连上即 mic on,不应等人工退出;设置被抢占后未保存项已随每次
        // 切换即时持久化,无丢失),READY 直接走与 PTT 相同的 start_ptt
        // (OFFLINE toast / 滴声 → voice.start → TONE_DONE 开流,零新路径)。
        // mic_hold 只在真正进入 LISTENING 时置位 —— 离线时 start_ptt 原地
        // 不动,不留"悬空 hold"(链路恢复也不该自动开麦,由看门狗重发)。
        // 已在 mic_hold 的 LISTENING = 重复 mic on,幂等忽略。
        if (s->state == APP_ST_HOME || s->state == APP_ST_SETTINGS) {
            go_ready(s, now_ms, out, out_n, max);
        }
        if (s->state == APP_ST_READY) {
            start_ptt(s, now_ms, out, out_n, max);
            s->mic_hold = (s->state == APP_ST_LISTENING);
            break;
        }
        if (s->state == APP_ST_LISTENING) {
            if (!s->mic_hold) {
                // 物理按住说话进行中:拒绝抢占,提示而不是静默丢命令
                set_toast(s, now_ms, "MIC busy (PTT active)");
                app_action_t t = { .type = APP_ACT_PLAY_TONE };
                t.u.tone = APP_TONE_ERROR;
                emit(out, out_n, max, t);
            }
            break;
        }
        // TRANSCRIBING / AGENT_RUNNING / APPROVAL:不与工作流抢音频
        set_toast(s, now_ms, "MIC busy");
        {
            app_action_t t2 = { .type = APP_ACT_PLAY_TONE };
            t2.u.tone = APP_TONE_ERROR;
            emit(out, out_n, max, t2);
        }
        break;

    case APP_EV_MIC_OFF:
        // mic_hold 中收口:停流 + voice.end + 直接回 READY —— 不进 TRANSCRIBING
        // (常开麦克风没有"等转写"语义,Mac 端守护进程只消费 PCM)。链路断开
        // 已把 mic_hold 清掉(handle_link_down),这里 LISTENING 之外只清残留
        // 标志,不产生任何动作。
        if (s->state == APP_ST_LISTENING && s->mic_hold) {
            s->mic_hold = false;
            app_action_t st = { .type = APP_ACT_STREAM_STOP };
            emit(out, out_n, max, st);
            app_action_t v = { .type = APP_ACT_SEND_VOICE_END };
            emit(out, out_n, max, v);
            go_ready(s, now_ms, out, out_n, max);
        } else {
            s->mic_hold = false;
        }
        break;

    // ---- 设置下发(console `snd`/`night`,2026-10-03)----
    // 三元组整包覆盖(携带方已读当前值改单项,见 app_types.h 注释),与设置页
    // 的 SAVE_SETTINGS 同一条持久化路径 —— 归约器无"部分更新"分支。
    // 任意状态可下发(调试通道,不受页面态约束);值域越界逐项兜底。
    case APP_EV_SETTINGS_SET: {
        uint8_t lvl = ev->u.settings.tone_level;
        if (lvl >= TONE_LVL_COUNT) lvl = TONE_LVL_DEFAULT;
        s->tone_level = lvl;
        s->night_mute = ev->u.settings.night_mute ? 1 : 0;
        if (ev->u.settings.tz_hour >= -12 && ev->u.settings.tz_hour <= 12) {
            s->tz_hour = ev->u.settings.tz_hour;
        }
        save_settings(s, out, out_n, max);
        app_action_t r = { .type = APP_ACT_UI_REFRESH };
        emit(out, out_n, max, r);
        break;
    }

    // ---- 双固件槽位探测落地(2026-10-03 v2)----
    // main.c 开机读 ota_1 头部扇区后投递;归约器只存标志,菜单行据此渲染
    // "FW: B ✓/–",长按切固件时据此放行或 toast 拒绝。
    case APP_EV_SLOT_PROBE:
        s->slot_b_present = ev->u.slot_probe.present ? 1 : 0;
        {
            app_action_t r2 = { .type = APP_ACT_UI_REFRESH };
            emit(out, out_n, max, r2);
        }
        break;
    }
}
