// 状态机全转移主机测试(纯 C,assert 断言)。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "fake_mode.h"

// 通道常量契约(双通道常开,2026-08-28):BLE=0/USB=2 —— 与旧 NVS 存储语义
// 保持一致(只删中间值不重排),状态机 link_channel 直接沿用。
_Static_assert(APP_CHAN_BLE == 0, "APP_CHAN_BLE must be 0");
_Static_assert(APP_CHAN_USB == 2, "APP_CHAN_USB must be 2");

static uint64_t now = 1000000;              // 单调递增假时钟
static app_state_t s;
static app_action_t out[APP_ACT_MAX];
static uint8_t on;

static void reduce(app_event_type_t t, uint64_t ts) {
    app_event_t ev = { .type = t };
    if (t == APP_EV_KEY_PRESS || t == APP_EV_KEY_RELEASE ||
        t == APP_EV_KEY_CLICK || t == APP_EV_KEY_DOUBLE) {
        ev.u.key.btn = APP_BTN_OK;
    }
    now = ts;
    app_state_reduce(&s, &ev, now, out, &on);
}

// 按下期间的真实 ADC 读数(BSP_BTN_MV_TABLE 各档实测中值:UP 3-5 / DOWN 303-305
// / OK 598-599)。reduce_btn 默认填本档值 —— app_state 的幽灵门禁按"回调 mv 是否
// 落在本档"判真假(见 key_ev_is_fake),留 0 会让 OK 长按被判成幽灵。
static uint16_t btn_real_mv(app_btn_t b) {
    switch (b) {
    case APP_BTN_UP:   return 5;
    case APP_BTN_DOWN: return 305;
    default:           return 598;   // APP_BTN_OK
    }
}

static void mic_ev(app_event_type_t type, uint64_t ts);   // 定义在后面;审批唤醒用例前向引用

static void reduce_btn(app_event_type_t t, app_btn_t b, uint64_t ts) {
    app_event_t ev = { .type = t };
    ev.u.key.btn = b;
    ev.u.key.mv = btn_real_mv(b);
    now = ts;
    app_state_reduce(&s, &ev, now, out, &on);
}

// 带 ADC 读数的按键事件(mv 默认 0 = 真实按压;≥2000 = 松开电平 = 幽灵事件)
static void reduce_btn_mv(app_event_type_t t, app_btn_t b, uint16_t mv, uint64_t ts) {
    app_event_t ev = { .type = t };
    ev.u.key.btn = b;
    ev.u.key.mv = mv;
    now = ts;
    app_state_reduce(&s, &ev, now, out, &on);
}

static int has_action(app_action_type_t t) {
    for (uint8_t i = 0; i < on; i++) if (out[i].type == t) return 1;
    return 0;
}

static app_action_t *find_action(app_action_type_t t) {
    for (uint8_t i = 0; i < on; i++) if (out[i].type == t) return &out[i];
    return NULL;
}

// 断言 first 动作严格先于 second(顺序契约:STREAM_STOP 先于 SEND_VOICE_END 等)
static void assert_action_order(app_action_type_t first, app_action_type_t second) {
    int a = -1, b = -1;
    for (uint8_t i = 0; i < on; i++) {
        if (a < 0 && out[i].type == first) a = i;
        if (b < 0 && out[i].type == second) b = i;
    }
    assert(a >= 0 && b > a);
}

static void reset(void) {
    app_state_init(&s);
    now = 1000000;
    s.last_key_ms = now;
}

// ---- HOME:● 单击进入 READY;▼ 单击=回车 / 长按=清空;▲ 无空闲语义 ----
// ---- HOME(菜单首页 v2):VOL± = 选行;OK 单击 = 进选中行;FW 行长按 = 切固件 ----
static void test_home_nav(void) {
    // 行 0(语音输入):OK 单击 → READY(与 v1 日常路径一致)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);

    // VOL± = 选行(不切状态、不上行):▼ 0→1,▲ 回 0,▲ 环绕 0→2,▲ 2→1
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 10);
    assert(s.state == APP_ST_HOME && s.menu_sel == 1);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));     // 菜单内 DOWN 单击不再注入回车
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 20);
    assert(s.menu_sel == 0);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 30);
    assert(s.menu_sel == 2);                          // 环绕
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 40);
    assert(s.menu_sel == 1);

    // 行 1(设置):OK 单击 → SETTINGS
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 10);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 20);
    assert(s.state == APP_ST_SETTINGS);

    // 行 2(切固件):OK 单击故意无动作(重启级操作只认长按);
    // 槽 B 空 → 长按给 toast + ERROR 音,不切;SLOT_PROBE 落地后 → FW_SWITCH
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 10);   // 0 → 2(环绕)
    assert(s.menu_sel == 2);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 20);
    assert(s.state == APP_ST_HOME);                        // 单击无动作
    assert(!has_action(APP_ACT_FW_SWITCH));
    s.wake_ms = now;                                       // 过 OK_LONG_GUARD(1s)
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 2000);   // 槽 B 空
    assert(!has_action(APP_ACT_FW_SWITCH));
    assert(strstr(s.toast, "empty") != NULL);
    {
        app_event_t ev = { .type = APP_EV_SLOT_PROBE };
        ev.u.slot_probe.present = 1;
        app_state_reduce(&s, &ev, now + 2100, out, &on);
    }
    assert(s.slot_b_present == 1);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 3000);   // 槽 B 在位 → 切换
    {
        app_action_t *a = find_action(APP_ACT_FW_SWITCH);
        assert(a && a->u.fw_switch.slot == 1);
    }

    // DOWN 长按仍是全局清空语义(菜单页无输入框,CLEAR 上行无害,键位表不破)
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 10);
    assert(s.state == APP_ST_HOME);
    {
        app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
        assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    }

    // ▲/▼ 双击在菜单无语义(选行只认单击,双击不产生上行)
    for (int i = 0; i < 2; i++) {
        const app_btn_t b[] = { APP_BTN_UP, APP_BTN_DOWN };
        reset();
        reduce_btn(APP_EV_KEY_DOUBLE, b[i], now + 10);
        assert(s.state == APP_ST_HOME);
        assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    }
}

// ---- READY:OK 已退出 PTT;▲ 长按=说话;▼ 单击=回车 / 长按=清空 ----
static void test_down_enter_clear(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);   // → READY (BUILD)
    assert(s.state == APP_ST_READY);

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 20);
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 30);  // DOWN 长按 = 清空
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    // 长按态松开只报 LONG_UP,不补 CLICK —— 清空之后绝不会再补一次回车
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_DOWN, now + 40);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    assert(s.state == APP_ST_READY);

    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_DOWN, now + 30);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));    // DOWN 双击无语义

    // UP 空闲
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 40);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // PTT 不受影响(基线缺陷:离线检查引入后此段未设 link_up,PTT 被拒;修正)
    s.link_up = true;
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 50);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
}

// ---- 键位改版回归(2026-08-29:清空 UP 双击 → DOWN 长按)----
// 旧版为了让"双击清空"和"长按说话"挤在 UP 一颗键上,堆了四层补偿:自建轻点链
// (UP_TAP_CHAIN_MS 1800)、挂起确认(PTT_CONFIRM_MS 250)、回弹假双击防御、驱动
// 补报去重。清空搬到 DOWN 长按后这四层全部删除,对应的五个用例
// (test_double_click_not_ptt / test_tap_chain_clear / test_slow_double_clear /
// test_real_device_rhythm_clear / test_single_tap_no_clear)一并删除,换成下面两个
// 反向断言:UP 上再没有轻点语义,长按说话不再被任何前置轻点拖慢。

// UP 的轻点/单击/双击一律无清空语义。按下即录之后(2026-08-29)轻点会开一次
// 会话并在松开时当场收束(< PTT_MIN_TALK_MS),但绝不产生任何按键上行动作
static void test_up_taps_never_clear(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);      // → READY

    // 连续三次轻点(旧版第二次就会判双击清空)
    for (int i = 0; i < 3; i++) {
        reduce_btn(APP_EV_KEY_PRESS,   APP_BTN_UP, now + 20);
        reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 120);
        assert(!has_action(APP_ACT_SEND_KEY_ACTION));
        assert(s.state == APP_ST_READY);                 // 轻点开的会话已收束
        reduce_btn(APP_EV_KEY_CLICK,   APP_BTN_UP, now + 180);
        assert(!has_action(APP_ACT_SEND_KEY_ACTION));
        assert(on == 0);                                  // 补报单击在 READY 无语义
    }
    // 驱动补报 DOUBLE 也不清空
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 50);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    assert(s.state == APP_ST_READY);
}

// 轻点之后立刻按住说话:必须当场开录音(按下即录,更不存在 250ms 挂起确认)
static void test_ptt_no_confirm_delay(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);      // → READY

    reduce_btn(APP_EV_KEY_PRESS,   APP_BTN_UP, now + 20);    // 轻点
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 120);
    reduce_btn(APP_EV_KEY_PRESS,   APP_BTN_UP, now + 60);    // 紧接着按住(旧版判"链内")
    assert(s.state == APP_ST_LISTENING);                      // 按下当场开录
    assert(has_action(APP_ACT_SEND_VOICE_START));
    reduce_btn(APP_EV_KEY_LONG,    APP_BTN_UP, now + 500);    // 驱动到点补报,已在录音中
    assert(s.state == APP_ST_LISTENING);
    assert(on == 0);                                          // 补报长按不再产生动作
    // 松手正常发送(按住够久,不触发误触收口)
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000);
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_SEND_VOICE_END));

    // 误碰级短按(< PTT_MIN_TALK_MS)收口:丢音频、不进转写。短按不到长按阈值,
    // 松开只报 RELEASE(没有 LONG_UP),结束分支必须同时认这一种。
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS,   APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 120);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    // 驱动随后补报的单击落在 READY:无语义,零动作
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 180);
    assert(on == 0);
}

// ---- PTT:离线被拒 + 错误音;在线开流 ----
static void test_ptt_offline_online(void) {
    reset();
    s.link_up = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 离线按下 ▲
    assert(s.state == APP_ST_READY);                       // 原地不动
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_ERROR);
    assert(strstr(s.toast, "OFFLINE"));

    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 在线按下 ▲
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_PLAY_TONE));                 // 440Hz 就绪音
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_STREAM_START));             // S3:开流移出 PRESS 产出,由 TONE_DONE 驱动
    assert(!s.stream_started);
    // 顺序契约:就绪音先于 voice.start(动作顺序保证)
    assert_action_order(APP_ACT_PLAY_TONE, APP_ACT_SEND_VOICE_START);
    // 滴声播完 → TONE_DONE → 开流
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 100, out, &on);
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
}

// ---- S3:READY+▲ 长按只产出滴声 + voice.start,开流等 TONE_DONE ----
static void test_tone_press_produce(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);
    assert(s.stream_started == false);
    assert(!has_action(APP_ACT_STREAM_START));
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_START);
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert_action_order(APP_ACT_PLAY_TONE, APP_ACT_SEND_VOICE_START);
}

// ---- S3:TONE_DONE 在 LISTENING 且流未开时开流;重复到达幂等忽略 ----
static void test_tone_done_idempotent(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录,未开流
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 110, out, &on);        // 滴声播完
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    app_state_reduce(&s, &ev, now + 120, out, &on);        // 重复(异常重复投递)
    assert(on == 0);                                       // 幂等:无动作
}

// ---- S3:松开即发后,滴声期间迟到的 TONE_DONE 必须忽略(不误开流) ----
static void test_tone_done_late_after_send(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 按下 #1:入 LISTENING,未开流
    assert(!has_action(APP_ACT_STREAM_START));
    // 松开:立即发送(≥ PTT_MIN_TALK_MS,否则算阈值误触被取消,见 test_ptt_no_confirm_delay)
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 500);
    assert(s.state == APP_ST_TRANSCRIBING);
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 210, out, &on);        // 滴声播完事件此刻才到
    assert(on == 0);                                       // 已离开 LISTENING → 忽略
    assert(!has_action(APP_ACT_STREAM_START));
}

// ---- S3:松开即发完整序列——先 TONE_DONE 开流,RELEASE 立即停流 ----
static void test_tone_release_sends_order(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 110, out, &on);        // 滴声播完 → 开流
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000); // 松开:立即停流发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));               // 停流由 RELEASE 产出
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
}

// ---- S3:兜底——TICK 满 APP_TONE_PENDING_TIMEOUT_MS 未收 TONE_DONE → 强制开流 ----
// 阈值用常量表达(2026-08-29 由 500 降到 200):这条断言卡的是"到点才开、早一毫秒
// 不开",与具体毫秒值无关,常量再调本测试自动跟随。
static void test_tone_tick_fallback(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
    reduce(APP_EV_TICK, s.state_since_ms + APP_TONE_PENDING_TIMEOUT_MS - 1);   // 差 1ms
    assert(!has_action(APP_ACT_STREAM_START));
    assert(!s.stream_started);
    reduce(APP_EV_TICK, s.state_since_ms + APP_TONE_PENDING_TIMEOUT_MS);       // 到点兜底
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
    // 兜底后迟到的 TONE_DONE:幂等忽略
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 600, out, &on);
    assert(on == 0);
}

// ---- S3:TONE_DONE 在非 LISTENING 状态(HOME/READY)无动作 ----
static void test_tone_done_ignored_elsewhere(void) {
    reset();
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 10, out, &on);         // HOME
    assert(on == 0);
    s.state = APP_ST_READY;
    s.state_since_ms = now;
    app_state_reduce(&s, &ev, now + 20, out, &on);         // READY
    assert(on == 0);
}

// ---- LISTENING:松开立即结束并发送 ----
static void test_listening_release_sends(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000); // 松开:立即发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
    // 转写期静音(用户需求 2026-08-28):松开不再播发送音
    assert(!has_action(APP_ACT_PLAY_TONE));
}

// ---- DOWN(音量减)长按 0.5s = 清空输入框(2026-08-29 从 UP 双击迁来)----
static void test_down_long_clears_input(void) {
    // READY:长按判定当场清空 + 一声确认音;不碰会话状态
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_DOWN, now + 20);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));          // 按下还不算
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 500);
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    app_action_t *tone = find_action(APP_ACT_PLAY_TONE);
    assert(tone && tone->u.tone == APP_TONE_REJECT);       // 阈值到点的确认音
    assert(!has_action(APP_ACT_SEND_VOICE_START));         // 不开录音
    assert(s.state == APP_ST_READY);
    // 松开:长按态只报 LONG_UP,不补 CLICK → 不会再发一次回车
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_DOWN, now + 300);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // 全局语义:转写中 / Agent 运行中同样清空,且不动会话
    const app_stage_t st_list[] = { APP_ST_TRANSCRIBING, APP_ST_AGENT_RUNNING,
                                    APP_ST_HOME, APP_ST_APPROVAL };
    for (unsigned i = 0; i < sizeof(st_list) / sizeof(st_list[0]); i++) {
        reset();
        s.link_up = true;
        s.state = st_list[i];
        s.state_since_ms = now;
        reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 10);
        a = find_action(APP_ACT_SEND_KEY_ACTION);
        assert(a && a->u.key_action.action == APP_KEY_CLEAR);
        assert(!has_action(APP_ACT_STREAM_CANCEL));        // 不清会话
        assert(!has_action(APP_ACT_STREAM_STOP));
        assert(s.state == st_list[i]);                     // 状态不变
    }

    // 幽灵长按(回调 mv 回到松开电平 2890 = 无人按键,射频腐蚀残留)必须丢弃 ——
    // 清空是破坏性动作,不能被假事件触发
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_DOWN, 2890, now + 500);
    assert(on == 0);                                       // 零动作
    // 真实按压读数(DOWN 档 150-447mV)照常清空
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_DOWN, 300, now + 500);
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);

    // UP 双击不再清空(语义已迁走);OK 双击本来就没有
    reset();
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 10);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 10);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // PTT 结束后立刻长按 DOWN 清空:不再有回弹防御窗口把它吞掉(回弹扫过 DOWN 档
    // 只是掠过,凑不出 500ms 长按,不需要靠时间窗防)
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 500);     // 开录
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3500); // 松开 → TRANSCRIBING
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 100);   // 松开 100ms 后就清空
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(s.state == APP_ST_TRANSCRIBING);                 // 会话不受影响
}

// ---- UP(音量加)= 按住说话:PRESS 当场开录(滴声同时响),松开发送 ----
// 2026-08-29:开录事件由 LONG(0.5s 阈值)改为 PRESS —— 真机实测按下到 `采集开始`
// 要 ~1.01s(428ms 阈值 + 578ms 滴声门禁),用户第一秒的话被吞掉。
static void test_up_press_ptt(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 500);    // 按下即录
    assert(s.state == APP_ST_LISTENING);
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_START);              // 滴声在按下时响
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_STREAM_STOP));              // 未结束
    assert(!has_action(APP_ACT_STREAM_CANCEL));
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3500); // 松开:发送
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
    // 转写期静音(用户需求 2026-08-28):松开不再播发送音
    assert(!has_action(APP_ACT_PLAY_TONE));

    // 离线:按下 UP 被拒,error 音 + toast,不进 LISTENING
    reset();
    s.link_up = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 500);
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_START));
    t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_ERROR);
    assert(strstr(s.toast, "OFFLINE"));

    // 只有补报单击、没有 PRESS(测试构造)时不触发任何动作、无提示音
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 100);
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_PLAY_TONE));
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));

    // OK 键已退出 PTT:按住/松开都不再开录音
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 100);   // 按住 OK
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_PLAY_TONE));
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_OK, now + 200); // 松开 OK
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_VOICE_END));
    assert(!has_action(APP_ACT_STREAM_STOP));
}

// ---- 按下即录的三道防线(2026-08-29)----
// 1) 幽灵 PRESS 判假:UP 侧用分压档上限(≤150mV)而不是通用的 2000mV —— 真机环里
//    抓到过 `btn=0 ev=0 mv=598`(OK 键按住期间冒出的跨档假 UP 按下),2000 放它过去。
// 2) 短按收口:< PTT_MIN_TALK_MS 松手 → 丢音频 + 收束回 READY。
// 3) 不变量 PTT_MIN_TALK_MS ≥ 驱动 long_press_time(500ms):短于阈值的一按事后会被
//    驱动补报 SINGLE_CLICK,而 UP 单击在 TRANSCRIBING 是"退出转写"。取等号让二者互斥
//    —— 能进转写的一按必然到过阈值,到过阈值的一按永不补报单击。
static void test_ptt_press_start_guards(void) {
    // 幽灵按下(松开电平 2890)不开会话
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);          // → READY
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 2890, now + 20);
    assert(s.state == APP_ST_READY);
    assert(on == 0);

    // 跨档幽灵按下(598mV 落在 OK 档)同样不开会话
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 598, now + 30);
    assert(s.state == APP_ST_READY);
    assert(on == 0);

    // 真实按下(3mV)当场开会话
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 3, now + 40);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));

    // 不变量:差一毫秒到阈值(499ms)必须收束回 READY,绝不能进 TRANSCRIBING ——
    // 否则紧随的补报单击会把这次转写撤掉。
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 499);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 180);         // 驱动补报
    assert(s.state == APP_ST_READY);
    assert(on == 0);

    // 刚好到阈值(500ms):正常发送进转写,此时驱动只报 LONG_UP,不补单击
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 3, now + 20);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 500);
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(has_action(APP_ACT_SEND_VOICE_END));
    // 长按态松开后驱动还会补一条 RELEASE:已不在 LISTENING,零动作
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 1);
    assert(on == 0);
    assert(s.state == APP_ST_TRANSCRIBING);
}

// ---- LISTENING 端:见 test_listening_release_sends / test_down_long_clears_input ----
static void test_listening_end(void) {
    test_listening_release_sends();
    test_down_long_clears_input();
}

// ---- 超时:TRANSCRIBING 30s / AGENT_RUNNING 90s → READY ----
static void test_timeouts(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 3000);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 3300);  // 单击窗口到期 → TRANSCRIBING
    assert(s.state == APP_ST_TRANSCRIBING);
    reduce(APP_EV_TICK, s.state_since_ms + 29 * 1000);     // 进入后 29s 未到
    assert(s.state == APP_ST_TRANSCRIBING);
    reduce(APP_EV_TICK, s.state_since_ms + 31 * 1000);     // 进入后 31s 超时
    assert(s.state == APP_ST_READY);
    assert(strstr(s.toast, "timeout"));
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));          // 中止路径不该产生上行
}

static void test_agent_running_timeout(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    reduce(APP_EV_TICK, now + 89 * 1000);
    assert(s.state == APP_ST_AGENT_RUNNING);
    reduce(APP_EV_TICK, now + 91 * 1000);
    assert(s.state == APP_ST_READY);
    assert(strstr(s.toast, "timeout"));
}

// ---- Agent 状态流转:TRANSCRIBING → AGENT_RUNNING → DONE ----
static void test_agent_status_flow(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;

    app_event_t ev = { .type = APP_EV_AGENT_STATUS,
                       .u.agent_status = { .state = APP_AGENT_RUNNING, .message = "unit tests..." } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_AGENT_RUNNING);
    assert(strcmp(s.agent_message, "unit tests...") == 0);
    assert(strcmp(s.agent_state_name, "running") == 0);

    ev = (app_event_t){ .type = APP_EV_AGENT_STATUS,
                        .u.agent_status = { .state = APP_AGENT_DONE, .message = "24 passed" } };
    app_state_reduce(&s, &ev, now, out, &on);
    // 无 DONE 页:done 直接回 READY 待命(用户:不要 done 提示),
    // 成功音一并取消(用户要求转写→ready 静音,2026-08-28)
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_PLAY_TONE));
}

// ---- 审批闭环 ----
static void test_approval(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;

    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "task_9821", .title = "Modify 3 files",
                                       .target = "OrderService.java", .diff_summary = "+128 / -37",
                                       .risk = APP_RISK_HIGH } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    app_action_t *t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_APPROVAL);

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 100);   // ● 批准
    app_action_t *a = find_action(APP_ACT_SEND_AGENT_ACTION);
    assert(a && a->u.agent_action.decision == APP_ACTION_APPROVE);
    assert(strcmp(a->u.agent_action.task_id, "task_9821") == 0);
    assert(s.state == APP_ST_AGENT_RUNNING);

    // 再来一次,▲ 拒绝
    s.state = APP_ST_AGENT_RUNNING;
    ev.u.approval.task_id[0] = 'x'; ev.u.approval.task_id[1] = '\0';
    app_state_reduce(&s, &ev, now, out, &on);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 200);
    a = find_action(APP_ACT_SEND_AGENT_ACTION);
    assert(a && a->u.agent_action.decision == APP_ACTION_REJECT);
    t = find_action(APP_ACT_PLAY_TONE);
    assert(t && t->u.tone == APP_TONE_REJECT);

    // ▼ 在 APPROVAL 下 = 回车(与全局 DOWN 语义统一;旧 approval_details
    // 详情视图切换已随状态机演进移除 —— 基线测试过期,按当前语义修正)
    s.state = APP_ST_AGENT_RUNNING;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 300);
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);
}

// ---- 审批唤醒(物理审批器 v2.2):息屏/锁定收到审批 → 强制亮屏 + 面板上电 ----
static void test_approval_wake(void) {
    // 20s 息屏(背光灭,面板仍通电):审批到达 → 只补背光
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);
    assert(s.screen_on == false && s.panel_on == true);
    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "ext-1", .title = "git push",
                                       .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ev, now + 10, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(s.screen_on == true && s.panel_on == true);
    assert(s.locked == false);
    assert(has_action(APP_ACT_UI_SCREEN_ON));
    assert(!has_action(APP_ACT_UI_PANEL_ON));       // 面板本就通电

    // 60s 深息屏(面板断电):审批到达 → 面板上电 + 背光亮
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_PANEL_OFF_MS + 1);
    assert(s.screen_on == false && s.panel_on == false);
    app_state_reduce(&s, &ev, now + 10, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(s.screen_on == true && s.panel_on == true);
    assert(has_action(APP_ACT_UI_PANEL_ON));
    assert(has_action(APP_ACT_UI_SCREEN_ON));

    // 锁定态:解锁 + 亮屏(防口袋盲批)
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 10);
    assert(s.locked == true && s.screen_on == false);
    app_state_reduce(&s, &ev, now + 20, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(s.locked == false);
    assert(s.screen_on == true && s.panel_on == true);

    // 已亮屏(RECORDING 中):审批照常打断,不重复发上电动作
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    assert(s.state == APP_ST_LISTENING && s.screen_on);
    on = 0;
    app_state_reduce(&s, &ev, now + 300, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(has_action(APP_ACT_STREAM_STOP));        // 录音收束(管线不泄漏)
    assert(!has_action(APP_ACT_UI_SCREEN_ON));      // 无冗余上电
    assert(!has_action(APP_ACT_UI_PANEL_ON));
}

// ---- 两级息屏/唤醒 ----
// 20s 无键 → 关背光(screen_on=false, 面板仍通电);60s → 面板 SLPIN 断电;
// PRESS 任意键 → 面板上电 + 背光亮。自动息屏只是省电显示态:唤醒后事件
// 照常放行执行(不吞按键;锁定态的全键忽略见锁屏用例,两回事)。
static void test_screen_off_wake(void) {
    reset();
    const uint64_t t0 = now;   // 息屏计时基准(= last_key_ms;reduce 会改 now,须先取)
    // 20s 级:关背光,面板不动
    reduce(APP_EV_TICK, t0 + APP_IDLE_BACKLIGHT_OFF_MS + 1);
    assert(s.screen_on == false);
    assert(s.panel_on == true);
    assert(has_action(APP_ACT_UI_SCREEN_OFF));
    assert(!has_action(APP_ACT_UI_PANEL_OFF));
    // 60s 边界:59.999s 不触发,恰好 60s(>=)触发面板断电
    reduce(APP_EV_TICK, t0 + APP_IDLE_PANEL_OFF_MS - 1);
    assert(s.panel_on == true);
    reduce(APP_EV_TICK, t0 + APP_IDLE_PANEL_OFF_MS);
    assert(s.panel_on == false);
    assert(has_action(APP_ACT_UI_PANEL_OFF));

    // 任意键唤醒:面板 + 背光一起恢复
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, t0 + APP_IDLE_PANEL_OFF_MS + 2);
    assert(s.panel_on == true);
    assert(s.screen_on == true);
    assert(has_action(APP_ACT_UI_PANEL_ON));
    assert(has_action(APP_ACT_UI_SCREEN_ON));

    // 自动息屏只省显示,不吞按键(过修修正):PRESS 唤醒后事件照常放行执行。
    // 真实事件流 CLICK 前必有 PRESS,设备在 CLICK 到达时已亮屏。
    s.screen_on = false;
    s.panel_on = true;
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 10);    // PRESS:恢复背光
    assert(s.screen_on == true);
    assert(!has_action(APP_ACT_UI_PANEL_ON));              // 面板已通电,不发 PANEL_ON
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 20);    // 同一次按键的 CLICK
    assert(s.state == APP_ST_READY);                       // 照常执行:HOME ● 单击 → READY
}

// ---- transcript:注入已迁 Mac 端,设备只更新显示(任意状态);final 区分预览/定稿 ----
static void test_transcript_display(void) {
    reset();
    s.state = APP_ST_TRANSCRIBING;
    app_event_t ev = { .type = APP_EV_TRANSCRIPT,
                       .u.transcript = { .text = "fix the bug", .inject_mode = APP_INJECT_TYPE,
                                         .final = false } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(strcmp(s.agent_message, "fix the bug") == 0);
    assert(s.transcript_final == false);                     // final:false = 预览态
    assert(has_action(APP_ACT_UI_REFRESH));
    // 注意:不再产出 INJECT_TEXT 动作(该动作已随 HID 注入退役)

    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.agent_message, "fix the bug") == 0);
    assert(snap.transcript_final == false);                  // 快照区分预览态(UI 加光标)

    // 定稿:final:true → 落定显示,快照标记定稿
    ev.u.transcript.final = true;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(strcmp(s.agent_message, "fix the bug") == 0);
    assert(s.transcript_final == true);
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.agent_message, "fix the bug") == 0);
    assert(snap.transcript_final == true);                   // 定稿态(UI 移除光标)

    // AGENT_RUNNING 同样显示(预览态)
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    ev.u.transcript.text[0] = 'x'; ev.u.transcript.text[1] = '\0';
    ev.u.transcript.final = false;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(strcmp(s.agent_message, "x") == 0);
    assert(s.transcript_final == false);

    // agent.status 替换消息后不再是转写预览(UI 无光标)
    app_event_t st = { .type = APP_EV_AGENT_STATUS,
                       .u.agent_status = { .state = APP_AGENT_RUNNING, .message = "deploying" } };
    app_state_reduce(&s, &st, now, out, &on);
    assert(strcmp(s.agent_message, "deploying") == 0);
    assert(s.transcript_final == true);

    // 审批决策写入的消息同样非预览
    reset();
    s.state = APP_ST_APPROVAL;
    s.state_since_ms = now;
    app_event_t ap = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1",
                                       .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ap, now, out, &on);
    assert(s.transcript_final == false);                     // 默认未定稿(preview 无从谈起)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 100);     // ● 批准
    assert(strcmp(s.agent_message, "Approved, agent continues...") == 0);
    assert(s.transcript_final == true);                      // 非转写文本,无预览光标
}

// ---- TRANSCRIBING:音量+单击退出转写场景(2026-08-28 用户需求);迟到文本丢弃 ----
static void reduce_up_press(uint16_t mv, uint64_t ts) {
    app_event_t ev = { .type = APP_EV_KEY_PRESS };
    ev.u.key.btn = APP_BTN_UP;
    ev.u.key.mv = mv;
    now = ts;
    app_state_reduce(&s, &ev, now, out, &on);
}

static void test_transcribing_exit(void) {
    // 真实单击退出:PRESS(mv=3,手指在键上)→ RELEASE → CLICK → READY
    reset();
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;
    reduce_up_press(3, now + 100);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 200);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 250);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_UI_REFRESH));

    // 退出后(READY)迟到的识别结果丢弃,不再上屏
    app_event_t ev = { .type = APP_EV_TRANSCRIPT,
                       .u.transcript = { .text = "late result", .inject_mode = APP_INJECT_TYPE,
                                         .final = true } };
    app_state_reduce(&s, &ev, now + 300, out, &on);
    assert(strcmp(s.agent_message, "late result") != 0);

    // 假按风暴不退出:PRESS(mv=2890,无人按键)→ RELEASE → CLICK → 状态不变
    reset();
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;
    reduce_up_press(2890, now + 100);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 200);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 250);
    assert(s.state == APP_ST_TRANSCRIBING);

    // HOME 态迟到文本同样丢弃
    reset();
    s.state = APP_ST_HOME;
    ev.u.transcript.text[0] = 'x'; ev.u.transcript.text[1] = '\0';
    ev.u.transcript.final = false;
    app_state_reduce(&s, &ev, now + 10, out, &on);
    assert(strcmp(s.agent_message, "x") != 0);

    // TRANSCRIBING 态文本照常显示(正常路径不受门禁影响)
    reset();
    s.state = APP_ST_TRANSCRIBING;
    app_state_reduce(&s, &ev, now + 10, out, &on);
    assert(strcmp(s.agent_message, "x") == 0);
}

// ---- BLE 链路断开:录音中停流回 READY ----
static void test_ble_link_down(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.link_up == false);
    assert(s.ble_connected == false);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));             // 断链无 Mac 可发,清残留防流入下次会话
    assert(!has_action(APP_ACT_SEND_VOICE_END));           // 会话中止,不发 end
    assert(strstr(s.toast, "Mac disconnected"));

    // APPROVAL 下断开:保持状态等待重连后 Mac 重发请求
    reset();
    s.link_up = true;
    s.state = APP_ST_APPROVAL;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
}

// ---- 文本安全:恰好填满缓冲也保证 NUL 结尾(截断上限由协议层测试覆盖) ----
static void test_bounded(void) {
    reset();
    s.state = APP_ST_TRANSCRIBING;   // 文本只在转写中显示(READY/HOME 迟到丢弃)
    app_event_t ev = { .type = APP_EV_TRANSCRIPT };
    memset(ev.u.transcript.text, 'x', sizeof(ev.u.transcript.text) - 1);
    ev.u.transcript.text[sizeof(ev.u.transcript.text) - 1] = '\0';
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.agent_message[sizeof(s.agent_message) - 1] == '\0');
    assert(has_action(APP_ACT_UI_REFRESH));
}

// ---- 录音中收到审批请求:必须停流,管线不能泄漏 ----
static void test_approval_during_listening(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);    // 开录
    assert(s.state == APP_ST_LISTENING);

    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1", .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ev, now + 100, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));            // 结束被截断的会话,Mac 侧关闭 ASR
    // APPROVAL 下再松开/双击:忽略,不再触碰管线
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 200);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 300);
    assert(s.state == APP_ST_APPROVAL);
}

// ---- 转写/执行中断开:回 READY,兜底停流 ----
static void test_ble_disconnect_transcribing(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));             // 兜底:停流 + 清残留(幂等)
    assert(strstr(s.toast, "Mac disconnected"));

    // 重连后回 ONLINE
    reset();
    app_event_t c = { .type = APP_EV_BLE_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.link_up == true);
    assert(s.ble_connected == true);
}

// ---- TRANSCRIBING / AGENT_RUNNING 下按键全部忽略 ----
static void test_keys_ignored_in_running(void) {
    reset();
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 20);
    assert(s.state == APP_ST_AGENT_RUNNING);
    assert(on == 0);                                       // PRESS/CLICK 无动作

    // DOWN 长按=全局清空(各态统一,AGENT_RUNNING 也不例外)—— 基线测试过期:
    // 旧断言"任意键全部忽略"未涵盖全局清空语义(先 OK 双击 → UP 双击 → DOWN 长按)
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 30);
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(s.state == APP_ST_AGENT_RUNNING);

    s.state = APP_ST_TRANSCRIBING;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 40);
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(on == 0);                                       // 本次 CLICK 无动作
}

// ---- LISTENING 中 ▲/▼ 无效(不打断录音) ----
static void test_listening_arrows_ignored(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 30);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 40);
    assert(s.state == APP_ST_LISTENING);
}

// ---- READY 下无按键同样分级息屏,任意键唤醒 ----
static void test_screen_off_ready(void) {
    reset();
    s.state = APP_ST_READY;
    s.state_since_ms = now;
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);
    assert(s.screen_on == false);
    assert(has_action(APP_ACT_UI_SCREEN_OFF));
    reduce(APP_EV_TICK, now + APP_IDLE_PANEL_OFF_MS + 1);
    assert(s.panel_on == false);
    assert(has_action(APP_ACT_UI_PANEL_OFF));

    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + APP_IDLE_PANEL_OFF_MS + 2); // 唤醒
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(s.state == APP_ST_READY);                        // 只唤醒,不离开 READY
}

// ---- 长按 OK 锁定息屏 ----
// 锁定:亮屏 HOME/READY 下长按 OK(0.5s 阈值)→ locked + 背光灭 + 面板 SLPIN 断电
static void test_ok_long_locks(void) {
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 10);    // 长按 OK 锁定
    assert(s.locked == true);
    assert(s.screen_on == false);
    assert(s.panel_on == false);
    assert(has_action(APP_ACT_UI_SCREEN_OFF));
    assert(has_action(APP_ACT_UI_PANEL_OFF));
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.screen_on == false);
    assert(snap.panel_on == false);

    // READY 态同样可锁定(单击 OK 先进 READY)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 20);
    assert(s.locked == true);
    assert(s.screen_on == false);
    assert(s.panel_on == false);
}

// 锁定态按键照常执行但不亮屏(2026-08-28 语义变更:锁定=省电模式,不是输入锁)
// 长按说话/回车/清空全部照常;任何按键不唤醒(口袋盲操作);仅 OK 长按解锁亮屏
static void test_locked_keys_operate_screen_off(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 20);     // 锁定
    assert(s.locked == true);
    assert(s.screen_on == false);

    // UP 按下即录(2026-08-29):锁定态照常开录,且不亮屏(口袋盲操作)
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 30);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(s.screen_on == false);                          // 保持息屏
    assert(s.panel_on == false);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 100); // 误碰级短按:收束回 READY
    assert(s.state == APP_ST_READY);
    assert(s.screen_on == false);

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 40);   // DOWN 长按:照常清空
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(s.screen_on == false);                          // 执行但不亮屏

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 50);  // DOWN 单击:照常回车
    a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_ENTER);
    assert(s.screen_on == false);

    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 60);    // UP 单击(READY 无分支)
    assert(on == 0);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 70);    // OK 单击:回菜单(盲切页)
    assert(s.state == APP_ST_HOME);
    assert(s.locked == true);
    assert(s.screen_on == false);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 75);    // 菜单行 0 单击:回 READY(盲)
    assert(s.state == APP_ST_READY);

    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 80);     // 再按 UP:照常开录
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(s.screen_on == false);                          // 口袋说话,屏幕仍关

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 90);     // 长按 OK:解锁亮屏
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(s.state == APP_ST_LISTENING);                   // 录音态不受解锁影响
}

// 锁定态收到审批请求:强制解锁亮屏(防口袋盲批;与"APPROVAL 常亮"政策一致)
static void test_locked_approval_forces_wake(void) {
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 10);     // 锁定
    assert(s.locked == true);
    assert(s.screen_on == false);

    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1",
                                       .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ev, now + 20, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(has_action(APP_ACT_UI_SCREEN_ON));
    assert(has_action(APP_ACT_UI_PANEL_ON));

    // 解锁后 OK 单击照常批准(锁定遗留不吞按键)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 30);
    app_action_t *a = find_action(APP_ACT_SEND_AGENT_ACTION);
    assert(a && a->u.agent_action.decision == APP_ACTION_APPROVE);
}

// 锁定态长按 OK 解锁:亮屏 + 全屏重绘,state 保持原值
static void test_ok_long_unlocks(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);    // → READY
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 20);     // 锁定
    assert(s.locked == true);
    assert(s.state == APP_ST_READY);                       // 锁定不切走状态

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 30);     // 再长按解锁
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(has_action(APP_ACT_UI_PANEL_ON));
    assert(has_action(APP_ACT_UI_SCREEN_ON));
    assert(has_action(APP_ACT_UI_REFRESH));
    assert(s.state == APP_ST_READY);                       // state 仍为原状态
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.screen_on == true);
    assert(snap.panel_on == true);
}

// 唤醒防误锁:自动息屏 → OK PRESS 唤醒 → 1s 内 OK LONG 不锁定(防"唤醒即锁")
static void test_wake_no_relock(void) {
    reset();
    const uint64_t t0 = now;
    reduce(APP_EV_TICK, t0 + APP_IDLE_BACKLIGHT_OFF_MS + 1);  // 20s 关背光
    assert(s.screen_on == false);
    reduce(APP_EV_TICK, t0 + APP_IDLE_PANEL_OFF_MS + 1);      // 60s 面板断电
    assert(s.panel_on == false);

    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, t0 + APP_IDLE_PANEL_OFF_MS + 2); // 长按唤醒的 PRESS
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(s.wake_ms == now);                                 // 唤醒时刻已记录

    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 500);       // 500ms 后 LONG 到达
    assert(s.locked == false);                                // guard 挡住:不锁定
    assert(s.screen_on == true);                              // 保持亮屏
    assert(on == 0);                                          // LONG 零动作
}

// ---- 面板断电(60s)后按 PTT:最长归约路径不得丢动作 ----
// 回归 2026-08-29:这条路径产 5 个动作(UI_PANEL_ON + UI_SCREEN_ON + UI_REFRESH
// + PLAY_TONE + SEND_VOICE_START),APP_ACT_MAX=4 时 emit() 静默丢掉最后一条
// —— 设备照样滴一声、照样开采集,而 Mac 侧从没收到 voice.start,整段话进不了
// ASR。只关背光(20s)的那一档才 4 条动作、恰好不溢出,所以旧测试(b 分支)
// 抓不到。同长的另两条路径一并锁住。
static void test_panel_off_longest_paths(void) {
    // a. 面板断电 + ▲ 按下:唤醒三连 + 滴声 + voice.start 五条都在
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);              // → READY
    reduce(APP_EV_TICK, s.last_key_ms + APP_IDLE_PANEL_OFF_MS + 1);  // 60s:面板断电
    assert(s.screen_on == false && s.panel_on == false);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 10);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_UI_PANEL_ON));
    assert(has_action(APP_ACT_UI_SCREEN_ON));
    assert(has_action(APP_ACT_PLAY_TONE));
    assert(has_action(APP_ACT_SEND_VOICE_START));   // ← APP_ACT_MAX=4 时被丢
    assert_action_order(APP_ACT_PLAY_TONE, APP_ACT_SEND_VOICE_START);

    // b. 面板断电 + ▼ 长按清空:唤醒三连 + 清空上行 + 确认音
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_PANEL_OFF_MS + 1);
    assert(s.panel_on == false);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_DOWN, now + 10);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 500);
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
    assert(has_action(APP_ACT_PLAY_TONE));

    // c. 面板断电 + 离线按 ▲:唤醒三连 + error 音 + 刷新(toast 要能显示出来)
    reset();
    s.link_up = false;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);              // → READY
    reduce(APP_EV_TICK, s.last_key_ms + APP_IDLE_PANEL_OFF_MS + 1);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 10);
    assert(s.state == APP_ST_READY);                                 // 离线不进 LISTENING
    assert(has_action(APP_ACT_UI_PANEL_ON));
    assert(has_action(APP_ACT_PLAY_TONE));
    assert(has_action(APP_ACT_UI_REFRESH));
    assert(s.toast[0] != '\0');
}

// ---- AUDIO_ERROR 必须收束会话(2026-08-29 修 F-2)----
// 旧版只 toast + 回 READY:管线残留帧流进下一会话、Mac 侧 voice 永不收束(挂到
// STT 超时)、STREAM_START 取的 PM 会话锁没人放(设备再回不到低功耗)。
static void test_audio_error_closes_session(void) {
    // a. LISTENING(已开流):CANCEL + voice.end + 回 READY + toast
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    app_event_t td = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &td, now + 100, out, &on);
    assert(s.state == APP_ST_LISTENING && s.stream_started);
    app_event_t ae = { .type = APP_EV_AUDIO_ERROR };
    app_state_reduce(&s, &ae, now + 200, out, &on);
    assert(has_action(APP_ACT_STREAM_CANCEL));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert(s.state == APP_ST_READY);
    assert(s.toast[0] != '\0');

    // b. LISTENING 但滴声未播完(流还没开):同样收束 —— 两条动作幂等,
    //    执行器侧 CANCEL 无副作用,voice.start 已经发出去了必须配 voice.end
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    assert(!s.stream_started);
    app_state_reduce(&s, &ae, now + 50, out, &on);
    assert(has_action(APP_ACT_STREAM_CANCEL));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert(s.state == APP_ST_READY);

    // c. 非 LISTENING(READY):不发收束动作(没有会话可收)
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    app_state_reduce(&s, &ae, now + 20, out, &on);
    assert(!has_action(APP_ACT_STREAM_CANCEL));
    assert(!has_action(APP_ACT_SEND_VOICE_END));
    assert(s.state == APP_ST_READY);
    assert(s.toast[0] != '\0');
}

// ---- LISTENING 兜底超时(2026-08-29 新增):松开事件丢失不得永久滞留 ----
static void test_ptt_max_talk_watchdog(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);   // 开录,不再给松开事件
    const uint64_t t0 = s.state_since_ms;
    app_event_t td = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &td, now + 100, out, &on);
    assert(s.state == APP_ST_LISTENING);

    // 到点前一刻:仍在录
    reduce(APP_EV_TICK, t0 + APP_PTT_MAX_TALK_MS - 1);
    assert(s.state == APP_ST_LISTENING);
    assert(!has_action(APP_ACT_STREAM_STOP));

    // 到点:按"正常松手"收束 —— 已录内容进转写,不是错误路径(无 toast)
    reduce(APP_EV_TICK, t0 + APP_PTT_MAX_TALK_MS);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert(s.state == APP_ST_TRANSCRIBING);
    assert(s.toast[0] == '\0');
}

// ---- 幽灵事件不算用户活动(2026-08-29 修 F-3)----
static void test_fake_key_is_not_activity(void) {
    // a. 假 UP 按下(mv=2890 松开电平)不刷新息屏计时:原定时刻照旧息屏
    reset();
    const uint64_t base = s.last_key_ms;
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 2890, base + 10000);
    assert(s.last_key_ms == base);                      // 计时未被推后
    reduce(APP_EV_TICK, base + APP_IDLE_BACKLIGHT_OFF_MS);
    assert(s.screen_on == false);                       // 照原定时刻息屏

    // b. 真 UP 按下会刷新(对照组)
    reset();
    const uint64_t base2 = s.last_key_ms;
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, base2 + 10000);
    assert(s.last_key_ms == base2 + 10000);

    // c. 假 UP 按下不唤醒屏幕(息屏态)
    reset();
    reduce(APP_EV_TICK, s.last_key_ms + APP_IDLE_PANEL_OFF_MS + 1);
    assert(s.screen_on == false && s.panel_on == false);
    reduce_btn_mv(APP_EV_KEY_PRESS, APP_BTN_UP, 2890, now + 10);
    assert(s.screen_on == false);                       // 白亮 20s 屏的洞
    assert(s.panel_on == false);
    assert(!has_action(APP_ACT_UI_SCREEN_ON));
    assert(!has_action(APP_ACT_UI_PANEL_ON));
}

// ---- OK 长按锁屏的 mv 门禁(2026-08-29 对称性补齐)----
static void test_ok_long_lock_mv_gate(void) {
    // a. 幽灵 OK 长按(mv=2890 松开电平)不上锁
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);          // → READY
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_OK, 2890, now + 20);
    assert(s.locked == false);
    assert(!has_action(APP_ACT_UI_PANEL_OFF));

    // b. 真 OK 长按(mv 落在 OK 档 447-1900)照常上锁
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_OK, 598, now + 20);
    assert(s.locked == true);
    assert(has_action(APP_ACT_UI_PANEL_OFF));

    // c. 解锁那条路径故意不加门禁:代价不对称(挡掉真解锁 = 用户面对一块砖)
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_OK, 2890, now + 5000);
    assert(s.locked == false);
    assert(has_action(APP_ACT_UI_PANEL_ON));
}

// ---- 息屏后按键放行(过修修正 2026-08-28):自动息屏只是省电显示态 ----
// 息屏(超时 tick 驱动)后按键应唤醒并照常执行操作,不再被吞;锁定态的
// "照常执行但不亮屏"由 locked 门禁负责(见 test_locked_keys_operate_screen_off)。
static void test_screen_off_keys_pass_through(void) {
    // a. ▼ DOWN 单击(息屏):唤醒 + 菜单选行(v2:HOME DOWN 单击不再注入回车)
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);   // 20s 无键:关背光
    assert(s.screen_on == false);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_DOWN, now + 10);       // 按下:唤醒
    assert(s.screen_on == true);
    assert(has_action(APP_ACT_UI_SCREEN_ON));
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 20);       // 单击:菜单选行
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    assert(s.menu_sel == 1);                                    // 0 → 1

    // b. ▲ UP 按下(READY,息屏):唤醒 + start_ptt 进 LISTENING(同一个 PRESS)
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);         // → READY
    reduce(APP_EV_TICK, s.last_key_ms + APP_IDLE_BACKLIGHT_OFF_MS + 1); // 息屏
    assert(s.screen_on == false);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 10);         // 按下:唤醒 + 开录
    assert(s.screen_on == true);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));

    // c. ● OK 长按(息屏):PRESS 唤醒后 1s 内 LONG 被 OK_LONG_GUARD 挡,不锁定
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);   // 息屏
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_OK, now + 10);         // 按下:唤醒
    assert(s.screen_on == true);
    assert(s.wake_ms == now);                                   // 唤醒时刻已记录
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 500);         // 500ms 后 LONG
    assert(s.locked == false);                                  // 防"唤醒即锁"
    assert(s.screen_on == true);
    assert(on == 0);                                            // LONG 零动作

    // d. ▼ DOWN 长按(息屏):唤醒 + 清空上行
    reset();
    reduce(APP_EV_TICK, now + APP_IDLE_BACKLIGHT_OFF_MS + 1);   // 息屏
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_DOWN, now + 10);       // 按下:唤醒
    assert(s.screen_on == true);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 500);       // 0.5s 长按:清空
    app_action_t *a = find_action(APP_ACT_SEND_KEY_ACTION);
    assert(a && a->u.key_action.action == APP_KEY_CLEAR);
}

// 重复锁定/解锁循环:解锁后(距唤醒 >1s)再长按 OK 正常锁定
static void test_relock_cycle(void) {
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 10);        // 锁定
    assert(s.locked == true);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 20);        // 解锁
    assert(s.locked == false);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 30);        // 再锁定
    assert(s.locked == true);
    assert(s.screen_on == false);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 40);        // 再解锁
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.panel_on == true);
    assert(s.state == APP_ST_HOME);                           // 状态自始至终不变
}

// 回归:LISTENING 中长按 OK 不锁定;APPROVAL 中 OK 单击仍批准、长按不锁定
static void test_lock_guards_listening_approval(void) {
    // LISTENING(录音中):长按 OK 不锁定,录音不受影响
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);       // → READY
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);        // 开录
    assert(s.state == APP_ST_LISTENING);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 30);
    assert(s.locked == false);
    assert(s.screen_on == true);
    assert(s.state == APP_ST_LISTENING);

    // APPROVAL:OK 长按不锁定;OK 单击仍批准
    reset();
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_APPROVAL_REQUEST,
                       .u.approval = { .task_id = "t1", .title = "Deploy",
                                       .target = "app.js", .diff_summary = "+1",
                                       .risk = APP_RISK_MEDIUM } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 100);
    assert(s.locked == false);
    assert(s.state == APP_ST_APPROVAL);                       // 不锁定不切换
    assert(s.screen_on == true);                              // 审批常亮保持
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 200);      // 批准
    app_action_t *a = find_action(APP_ACT_SEND_AGENT_ACTION);
    assert(a && a->u.agent_action.decision == APP_ACTION_APPROVE);
    assert(s.state == APP_ST_AGENT_RUNNING);
}

// ---- APPROVAL 无超时(安全审批必须等物理按键) ----
static void test_approval_no_timeout(void) {
    reset();
    s.state = APP_ST_APPROVAL;
    s.state_since_ms = now;
    reduce(APP_EV_TICK, now + 120 * 1000);                 // 远超 AGENT_RUNNING 的 90s
    assert(s.state == APP_ST_APPROVAL);
    assert(s.screen_on == true);                           // 也不熄屏
}

// ---- agent.status(error) → READY + 错误提示 ----
static void test_agent_error(void) {
    reset();
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_AGENT_STATUS,
                       .u.agent_status = { .state = APP_AGENT_ERROR, .message = "build failed" } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_READY);
    assert(strstr(s.toast, "error"));
}

// ---- NET BUSY 横幅:丢帧起、恢复止,边沿触发 ----
static void test_audio_drop_netbusy(void) {
    reset();
    app_event_t st = { .type = APP_EV_AUDIO_DROP_START };
    app_state_reduce(&s, &st, now, out, &on);
    assert(s.net_busy == true);

    reset();
    app_event_t en = { .type = APP_EV_AUDIO_DROP_END };
    app_state_reduce(&s, &en, now, out, &on);
    assert(s.net_busy == false);
}

// ---- BLE 链路事件:订阅=通,断开=断 + toast ----
static void test_ble_events(void) {
    reset();
    app_event_t d = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &d, now, out, &on);
    assert(s.ble_connected == false);
    assert(s.link_up == false);
    assert(strstr(s.toast, "Mac disconnected"));
    app_event_t c = { .type = APP_EV_BLE_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.ble_connected == true);
    assert(s.link_up == true);

    // 事件行丢弃:toast 提示
    app_event_t drop = { .type = APP_EV_BLE_DROP };
    app_state_reduce(&s, &drop, now, out, &on);
    assert(strstr(s.toast, "dropped"));
}

// ---- 快照:agent_state_name 保留真实 status,仅空时兜底 ----
static void test_snapshot_agent_name(void) {
    reset();
    s.state = APP_ST_AGENT_RUNNING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_AGENT_STATUS,
                       .u.agent_status = { .state = APP_AGENT_THINKING, .message = "" } };
    app_state_reduce(&s, &ev, now, out, &on);
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.agent_state_name, "thinking") == 0);  // 不被覆盖成 "running"

    // 新会话:进入 TRANSCRIBING 时清空,快照兜底显示 "running"
    reset();
    s.state = APP_ST_TRANSCRIBING;
    s.state_since_ms = now;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.agent_state_name, "running") == 0);
}

// ---- 快照:link_up 驱动 OFFLINE 横幅 ----
static void test_snapshot_link_up(void) {
    reset();
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.link_up == false);                          // init 默认断(开机无 Mac 不会收到断开事件,不能假设通)

    app_event_t d = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &d, now, out, &on);
    app_state_snapshot(&s, now, &snap);
    assert(snap.link_up == false);
    assert(snap.ble_connected == false);
}

// ---- USB 有线通道:USB_CONNECTED = 链路通(与 BLE 对等;双通道常开无门禁) ----
static void test_usb_link_up(void) {
    reset();
    app_event_t c = { .type = APP_EV_USB_CONNECTED };
    app_state_reduce(&s, &c, now, out, &on);
    assert(s.link_up == true);
    assert(s.ble_connected == false);
    assert(s.link_channel == APP_CHAN_USB);                 // 首个连接通道 = 会话路由
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "USB") == 0);              // 横幅按通道渲染

    // USB 链路下 PTT 可用(HOME 单击进 READY,再按下开录)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING);
    assert(has_action(APP_ACT_SEND_VOICE_START));
}

// ---- USB 断开:停流回 READY + 通道名保留(与 BLE 断连同路径) ----
static void test_usb_link_down(void) {
    reset();
    s.link_up = true;
    s.link_channel = APP_CHAN_USB;
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;
    app_event_t ev = { .type = APP_EV_USB_DISCONNECTED };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.link_up == false);
    assert(s.state == APP_ST_READY);
    assert(has_action(APP_ACT_STREAM_CANCEL));               // 兜底停流,同 BLE 断连
    assert(!has_action(APP_ACT_SEND_VOICE_END));             // 会话中止,不发 end
    assert(strstr(s.toast, "USB disconnected"));

    // APPROVAL 下断开:保持状态等待重连
    reset();
    s.link_up = true;
    s.link_channel = APP_CHAN_USB;
    s.state = APP_ST_APPROVAL;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.state == APP_ST_APPROVAL);

    // 快照横幅名:USB 断线后仍显示 USB
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "USB") == 0);
}

// ---- 校时下行:透传动作,状态不变(与链路状态正交) ----
static void test_time_set(void) {
    reset();
    s.link_up = true;
    s.state = APP_ST_READY;
    app_event_t ev = { .type = APP_EV_TIME_SET, .u.time_set.epoch = 1767225600LL };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(on == 1);                                        // 仅 TIME_SET 动作
    app_action_t *t = find_action(APP_ACT_TIME_SET);
    assert(t && t->u.time_set.epoch == 1767225600LL);       // epoch 透传无损
    assert(s.state == APP_ST_READY);                        // 状态机不受影响
    assert(s.link_up == true);

    // 负 epoch 同样透传(校时值域不含正负限制)
    reset();
    ev = (app_event_t){ .type = APP_EV_TIME_SET, .u.time_set.epoch = -1 };
    app_state_reduce(&s, &ev, now, out, &on);
    t = find_action(APP_ACT_TIME_SET);
    assert(t && t->u.time_set.epoch == -1);
}


// ---- 双通道常开(2026-08-28):会话粘性 —— 活动会话期间另一通道连/断不夺路 ----
// BLE 会话进行中:USB 连接只刷新(不夺路),USB 断开不掐 BLE 音频流。
static void test_dual_session_survives_other_channel(void) {
    reset();
    s.link_up = true;
    s.link_channel = APP_CHAN_BLE;         // BLE 会话路由中
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;

    // USB 连接:空闲态才夺路;活动会话中只当候选,link_channel 保持 BLE
    app_event_t uc = { .type = APP_EV_USB_CONNECTED };
    app_state_reduce(&s, &uc, now, out, &on);
    assert(s.link_channel == APP_CHAN_BLE);
    assert(s.link_up == true);
    assert(s.state == APP_ST_LISTENING);

    // USB 断开:非会话通道,不影响本会话
    app_event_t ud = { .type = APP_EV_USB_DISCONNECTED };
    app_state_reduce(&s, &ud, now, out, &on);
    assert(s.link_up == true);
    assert(s.state == APP_ST_LISTENING);
    assert(!has_action(APP_ACT_STREAM_CANCEL));
    assert(!has_action(APP_ACT_SEND_VOICE_END));
}

// ---- 双通道常开:会话通道断开 → 收束会话 + 自动切到仍通通道 ----
static void test_dual_active_channel_down_fails_over(void) {
    reset();
    fake_mode_set_channel_up(APP_CHAN_BLE, true);   // BLE 仍连
    s.link_up = true;
    s.link_channel = APP_CHAN_USB;         // USB 会话路由中
    s.state = APP_ST_LISTENING;
    s.state_since_ms = now;

    app_event_t ud = { .type = APP_EV_USB_DISCONNECTED };
    app_state_reduce(&s, &ud, now, out, &on);
    assert(s.link_up == true);             // BLE 兜底:链路仍通
    assert(s.link_channel == APP_CHAN_BLE);
    assert(s.state == APP_ST_READY);       // 会话收束(停流回 READY + toast)
    assert(has_action(APP_ACT_STREAM_CANCEL));

    // 双通道均断:link_up=false,通道名保留(横幅显示断掉的通道)
    fake_mode_set_channel_up(APP_CHAN_BLE, false);
    reset();
    s.link_up = true;
    s.link_channel = APP_CHAN_BLE;
    app_event_t bd = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &bd, now, out, &on);
    assert(s.link_up == false);
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(strcmp(snap.link_name, "BLE") == 0);
}

// ---- 双通道常开:空闲态"最近连接/使用"胜出 —— 先 BLE 后 USB,PTT 走 USB ----
static void test_dual_idle_last_connect_wins(void) {
    reset();
    app_event_t bc = { .type = APP_EV_BLE_CONNECTED };
    app_state_reduce(&s, &bc, now, out, &on);
    assert(s.link_channel == APP_CHAN_BLE);
    assert(s.link_up == true);

    // 空闲(READY)时 USB 连入:夺路为新会话通道
    app_event_t uc = { .type = APP_EV_USB_CONNECTED };
    app_state_reduce(&s, &uc, now, out, &on);
    assert(s.link_channel == APP_CHAN_USB);

    // PTT 开录:link_channel 保持 USB;期间 BLE 断开不掐流
    // (时间推进 ≥1s:绕过 BLE 连接握手期假长按抑制窗口 BLE_CONNECT_PTT_GUARD)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 1100);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 1200);
    assert(s.state == APP_ST_LISTENING);
    assert(s.link_channel == APP_CHAN_USB);
    app_event_t bd = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &bd, now, out, &on);
    assert(s.link_up == true);
    assert(s.state == APP_ST_LISTENING);
}

// ---- 双通道常开:有线主机在位(USB 供电)不自动息屏 ----
static void test_wired_no_screen_off(void) {
    reset();
    fake_mode_set_wired(true);             // USB 主机在位:屏幕常亮
    app_event_t t = { .type = APP_EV_TICK };
    s.last_key_ms = now - 70000;           // 远超 20s 背光 / 60s 面板超时
    app_state_reduce(&s, &t, now, out, &on);
    assert(s.screen_on == true);
    assert(s.panel_on == true);

    fake_mode_set_wired(false);            // 无线:恢复自动息屏
    app_state_reduce(&s, &t, now, out, &on);
    assert(s.screen_on == false);
    assert(s.panel_on == false);
}

// ---- 常开麦克风模式(console `mic on|off`,2026-10-02)----
static void mic_ev(app_event_type_t type, uint64_t ts) {
    app_event_t ev = { .type = type };
    app_state_reduce(&s, &ev, ts, out, &on);
}

// HOME 下 mic on:先进 READY 再 start_ptt,与 PTT 同路径(滴声 → voice.start,
// 开流仍由 TONE_DONE 驱动),mic_hold 置位。
static void test_mic_on_from_home(void) {
    reset();                               // HOME
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    assert(s.state == APP_ST_LISTENING);
    assert(s.mic_hold == true);
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(!has_action(APP_ACT_STREAM_START));
    app_event_t ev = { .type = APP_EV_TONE_DONE };
    app_state_reduce(&s, &ev, now + 200, out, &on);
    assert(has_action(APP_ACT_STREAM_START));
    assert(s.stream_started == true);
}

// 离线 mic on:start_ptt 原地不动(OFFLINE toast),不留悬空 hold。
static void test_mic_on_offline_no_residue(void) {
    reset();                               // link_up = false
    mic_ev(APP_EV_MIC_ON, now + 10);
    assert(s.state == APP_ST_READY);
    assert(s.mic_hold == false);
    assert(strstr(s.toast, "OFFLINE"));
}

// mic_hold 下 60s 说话时长兜底不生效(会话预期无限长)。
static void test_mic_60s_not_forced(void) {
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    reduce(APP_EV_TICK, now + 61500);      // 超过 APP_PTT_MAX_TALK_MS
    assert(s.state == APP_ST_LISTENING);
    assert(!has_action(APP_ACT_SEND_VOICE_END));
    assert(!has_action(APP_ACT_STREAM_STOP));
}

// mic off:停流 + voice.end + 直接回 READY(不进 TRANSCRIBING,无转写语义)。
static void test_mic_off_returns_ready(void) {
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    mic_ev(APP_EV_MIC_OFF, now + 5000);
    assert(s.state == APP_ST_READY);
    assert(s.mic_hold == false);
    assert(has_action(APP_ACT_STREAM_STOP));
    assert(has_action(APP_ACT_SEND_VOICE_END));
    assert_action_order(APP_ACT_STREAM_STOP, APP_ACT_SEND_VOICE_END);
}

// mic_hold 期间物理 UP 松开被吞掉:直播音频不因误碰实体键被掐断。
static void test_mic_swallows_release(void) {
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    reduce_btn(APP_EV_KEY_LONG_UP, APP_BTN_UP, now + 9000);
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 9050);
    assert(s.state == APP_ST_LISTENING);
    assert(s.mic_hold == true);
    assert(!has_action(APP_ACT_SEND_VOICE_END));
    mic_ev(APP_EV_MIC_OFF, now + 9100);
    assert(s.state == APP_ST_READY);
}

// 链路断开收束 mic 会话并清掉 hold 残留(否则下次 mic on 幂等误判)。
static void test_mic_link_down_clears_hold(void) {
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    assert(s.mic_hold == true);
    app_event_t ev = { .type = APP_EV_BLE_DISCONNECTED };
    app_state_reduce(&s, &ev, now + 90000, out, &on);
    assert(s.state == APP_ST_READY);
    assert(s.mic_hold == false);
    assert(has_action(APP_ACT_STREAM_CANCEL));
}

// 物理 PTT 进行中 mic on:拒绝抢占(MIC busy toast),原会话不受影响。
static void test_mic_busy_during_ptt(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING && !s.mic_hold);
    mic_ev(APP_EV_MIC_ON, now + 40);
    assert(s.state == APP_ST_LISTENING && !s.mic_hold);
    assert(strstr(s.toast, "MIC busy"));
}

// ---- 设置页(2026-10-03:OK 双击进入 / VOL± 选项 / OK 切档 / 长按退出)----

// 进入与退出:READY 双击进入(主路径);HOME 双击进入(防御路径);DOWN 长按在
// 设置内 = 退出(不得触发全局清空);OK 长按 = 退出(不得触发锁屏)。
static void test_settings_enter_exit(void) {
    // 主路径:READY 下 OK 双击(OK 单击在 READY 本无语义,零误触)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);   // HOME → READY
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    assert(s.state == APP_ST_SETTINGS);
    assert(s.settings_sel == 0);
    assert(has_action(APP_ACT_UI_REFRESH));

    // DOWN 长按:退出设置,绝不发 CLEAR 上行(设置页清空输入框没有意义)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 600);
    assert(s.state == APP_ST_READY);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));         // 无清空、无回车
    assert(!has_action(APP_ACT_PLAY_TONE));               // 无清空确认音

    // OK 长按:退出设置,不锁屏(锁屏入口限 HOME/READY)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    assert(s.state == APP_ST_SETTINGS);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 600);
    assert(s.state == APP_ST_READY);
    assert(s.locked == false);

    // 幽灵 OK 长按(松开电平 2890)在设置内:既不退出也不产生动作
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_OK, 2890, now + 600);
    assert(s.state == APP_ST_SETTINGS);

    // 锁定态不进设置(盲操作省电模式,不该进一个看不见的菜单)
    reset();
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 10);    // HOME 下锁定
    assert(s.locked == true);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    assert(s.state == APP_ST_HOME);                       // 未进入
    assert(s.locked == true);                             // 仍锁定
}

// 导航与切档:VOL± 环绕三行;OK 切选中项的值;SAVE_SETTINGS 携带三元组。
static void test_settings_navigate_adjust(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    assert(s.state == APP_ST_SETTINGS);

    // 选中下移 0→1→2→0(环绕)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 600);
    assert(s.settings_sel == 1);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 700);
    assert(s.settings_sel == 2);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 800);
    assert(s.settings_sel == 0);
    // 上移环绕 0→2
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_UP, now + 900);
    assert(s.settings_sel == 2);

    // Timezone(+1 环绕):缺省 8 → OK 单击 → 9;拨到 +12 再点 → -12
    assert(s.tz_hour == 8);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 1000);   // 切 tz
    assert(s.tz_hour == 9);
    s.tz_hour = 12;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 1100);
    assert(s.tz_hour == -12);
    app_action_t *a = find_action(APP_ACT_SAVE_SETTINGS);
    assert(a && a->u.settings.tz_hour == -12);
    assert(a->u.settings.tone_level == s.tone_level);
    assert(a->u.settings.night_mute == s.night_mute);

    // Night Mute:开 ↔ 关
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 1200);   // sel → 0
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 1300);   // sel → 1
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 1400);
    assert(s.night_mute == 1);
    a = find_action(APP_ACT_SAVE_SETTINGS);
    assert(a && a->u.settings.night_mute == 1);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 1500);
    assert(s.night_mute == 0);

    // Sound(选中第 0 行):HIGH → LOW → OFF → HIGH,每次 SAVE + toast
    // (此刻 sel=1(Night),DOWN×2:1→2→0)
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 1600);    // sel → 2
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 1700);    // sel → 0
    assert(s.settings_sel == 0);
    assert(s.tone_level == 2);                                // 缺省 HIGH
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 1900);
    assert(s.tone_level == 1);
    assert(strstr(s.toast, "LOW"));
    a = find_action(APP_ACT_SAVE_SETTINGS);
    assert(a && a->u.settings.tone_level == 1);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 2000);
    assert(s.tone_level == 0);
    assert(strstr(s.toast, "OFF"));
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 2100);
    assert(s.tone_level == 2);
    assert(strstr(s.toast, "HIGH"));

    // DOWN 单击在设置内 = 移动选中,不是回车上行(设置页不注入按键)
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
}

// 10s 无操作自动退出;差 1ms 不退;退出前的最后一次按键刷新计时。
static void test_settings_timeout(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    const uint64_t t0 = s.settings_last_ms;
    assert(t0 > 0);
    reduce(APP_EV_TICK, t0 + APP_SETTINGS_IDLE_EXIT_MS - 1);  // 差 1ms
    assert(s.state == APP_ST_SETTINGS);
    reduce(APP_EV_TICK, t0 + APP_SETTINGS_IDLE_EXIT_MS);      // 到点
    assert(s.state == APP_ST_READY);
}

// 设置页息屏门禁:SETTINGS 计入 idle_state(20s 背光/60s 面板,与 HOME/READY 同)。
// 直接推进 settings_last_ms 构造"仍在设置页但早过了按键时刻"的窗口(自动退出
// 在 settings_last_ms+10s,背光在 last_key_ms+20s —— 两个计时器独立可分辨)。
static void test_settings_idle_screen_off(void) {
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    const uint64_t k0 = s.last_key_ms;
    s.settings_last_ms = k0 + 15000;   // 模拟 15s 时又按过设置键(刷新了退出计时)
    reduce(APP_EV_TICK, k0 + APP_IDLE_BACKLIGHT_OFF_MS + 1);
    assert(s.state == APP_ST_SETTINGS);   // 未到 settings_last_ms+10s,仍在设置
    assert(s.screen_on == false);         // 但背光按 last_key_ms 计时已熄
}

// 常开麦优先:设置页收到 mic on → 放弃设置直接开录(守护进程连上即 mic on)。
static void test_settings_mic_on_priority(void) {
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    assert(s.state == APP_ST_SETTINGS);
    mic_ev(APP_EV_MIC_ON, now + 600);
    assert(s.state == APP_ST_LISTENING);
    assert(s.mic_hold == true);
    assert(has_action(APP_ACT_SEND_VOICE_START));
    assert(!strstr(s.toast, "MIC busy"));   // 不是拒绝路径
}

// 常开麦快捷手势:UP(音量+)双击循环切档 + SAVE + toast;
// 普通 PTT 录音中同手势必须缺席(第一按已开录,双击会变成两次录音误触)。
static void test_mic_hold_double_cycles_tone(void) {
    // 常开麦中:HIGH → LOW → OFF,toast 反馈,持久化
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    assert(s.state == APP_ST_LISTENING && s.mic_hold);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 1000);
    assert(s.tone_level == 1);
    assert(strstr(s.toast, "LOW"));
    assert(has_action(APP_ACT_SAVE_SETTINGS));
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 2000);
    assert(s.tone_level == 0);
    assert(strstr(s.toast, "OFF"));
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 3000);
    assert(s.tone_level == 2);
    assert(strstr(s.toast, "HIGH"));
    assert(s.state == APP_ST_LISTENING);   // 录音不受切档影响
    assert(s.mic_hold == true);

    // 锁定 + 常开麦:盲操作切静音(夜间主场景)同样生效,且不亮屏
    // (锁定 = 息屏省电模式;LISTENING 中 OK LONG 本就不可锁,直接构造锁定态)
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    s.locked = true;
    s.screen_on = false;
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 1000);
    assert(s.tone_level == 1);               // 切档生效
    assert(s.screen_on == false);            // 不亮屏(锁定语义保持)
    assert(s.state == APP_ST_LISTENING);

    // 普通 PTT(非 mic_hold):UP 双击不切档(手势缺席,防误触)
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING && !s.mic_hold);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 1000);
    assert(s.tone_level == 2);                     // 未变
    assert(!has_action(APP_ACT_SAVE_SETTINGS));
}

// console 下行(SETTINGS_SET):三元组整包覆盖,越界兜底,产出 SAVE_SETTINGS。
static void test_settings_set_event(void) {
    reset();
    app_event_t ev = { .type = APP_EV_SETTINGS_SET,
                       .u.settings = { .tone_level = 0, .night_mute = 1, .tz_hour = 9 } };
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.tone_level == 0);
    assert(s.night_mute == 1);
    assert(s.tz_hour == 9);
    app_action_t *a = find_action(APP_ACT_SAVE_SETTINGS);
    assert(a && a->u.settings.tone_level == 0);
    assert(a && a->u.settings.night_mute == 1);
    assert(a && a->u.settings.tz_hour == 9);
    assert(has_action(APP_ACT_UI_REFRESH));

    // 越界值:档位兜底 HIGH,tz 越界保留原值,night 任意非 0 规整为 1
    ev.u.settings.tone_level = 7;
    ev.u.settings.night_mute = 3;
    ev.u.settings.tz_hour = 13;
    app_state_reduce(&s, &ev, now, out, &on);
    assert(s.tone_level == 2);
    assert(s.night_mute == 1);
    assert(s.tz_hour == 9);
}

// 快照:设置页字段随快照输出(UI 渲染契约)。
static void test_settings_snapshot(void) {
    reset();
    s.state = APP_ST_SETTINGS;
    s.settings_sel = 2;
    s.tone_level = 1;
    s.night_mute = 1;
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.settings_sel == 2);
    assert(snap.tone_level == 1);
    assert(snap.night_mute == 1);
}

// ---- 录音中设置浮层(2026-10-03 v2):常开麦 OK 双击呼出,录音不断,退出回录音现场 ----
static void test_settings_overlay_listening(void) {
    // 入口 + 键位走设置语义 + OK 长按退出:录音现场全程不动
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    assert(s.state == APP_ST_LISTENING && s.mic_hold);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 1000);
    assert(s.settings_overlay == 1);
    assert(s.state == APP_ST_LISTENING);        // 状态不动
    assert(s.mic_hold == true);                 // 录音不受影响
    assert(!has_action(APP_ACT_STREAM_CANCEL));
    assert(!has_action(APP_ACT_STREAM_STOP));

    // 浮层内 DOWN 选行 → OK 切夜间静音(SAVE 落地),仍在浮层
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 1100);
    assert(s.settings_sel == 1);
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 1200);
    assert(s.night_mute == 1);
    assert(has_action(APP_ACT_SAVE_SETTINGS));
    assert(s.state == APP_ST_LISTENING && s.settings_overlay == 1);

    // OK 长按退出:只清浮层,state 仍 LISTENING
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 1300);
    assert(s.settings_overlay == 0);
    assert(s.state == APP_ST_LISTENING);
    assert(s.mic_hold == true);

    // 浮层内 DOWN 长按 = 退出浮层,拦截先于全局清空分支(无 CLEAR 上行/确认音)
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 1000);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_DOWN, now + 1100);
    assert(s.settings_overlay == 0);
    assert(s.state == APP_ST_LISTENING);
    assert(!has_action(APP_ACT_SEND_KEY_ACTION));
    assert(!has_action(APP_ACT_PLAY_TONE));

    // 10s 无操作:浮层自动收起,回录音现场(不是 READY)
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 1000);
    assert(s.settings_overlay == 1);
    reduce(APP_EV_TICK, now + 1000 + APP_SETTINGS_IDLE_EXIT_MS + 1);
    assert(s.settings_overlay == 0);
    assert(s.state == APP_ST_LISTENING);

    // mic off 断链收束:浮层随 abort_to_ready 清掉
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 1000);
    mic_ev(APP_EV_MIC_OFF, now + 2000);
    assert(s.settings_overlay == 0);
    assert(s.state == APP_ST_READY);

    // UP 双击切档手势在浮层内让位(选行优先),浮层外不变
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 1000);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_UP, now + 1100);
    assert(s.tone_level == 2);                  // 未切档(浮层消费)
    assert(s.settings_overlay == 1);

    // 防御(真实流不可达,构造异常态):PTT 中浮层未关,UP 松开先收浮层再收口
    reset();
    s.link_up = true;
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    reduce_btn(APP_EV_KEY_PRESS, APP_BTN_UP, now + 20);
    assert(s.state == APP_ST_LISTENING && !s.mic_hold);
    s.settings_overlay = 1;
    reduce_btn(APP_EV_KEY_RELEASE, APP_BTN_UP, now + 1000);
    assert(s.settings_overlay == 0);
    assert(s.state == APP_ST_TRANSCRIBING);     // 原收口路径完好
}

// 双固件快照字段:菜单行/槽 B 在位/浮层标志逐项透传(渲染分流依据)。
static void test_menu_overlay_snapshot(void) {
    reset();
    s.menu_sel = 2;
    s.slot_b_present = 1;
    s.settings_overlay = 1;
    app_ui_snapshot_t snap;
    app_state_snapshot(&s, now, &snap);
    assert(snap.menu_sel == 2);
    assert(snap.slot_b_present == 1);
    assert(snap.settings_overlay == 1);
}

// ---- 回菜单首页(2026-10-03 v2.1):READY 单击 OK / 录音中长按 OK ----
static void test_back_to_menu(void) {
    // READY 单击 OK → 菜单;再单击行 0 → READY(往返闭合)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);   // HOME row0 → READY
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 20);   // READY → HOME
    assert(s.state == APP_ST_HOME);

    // READY 双击 OK:宿主直发 DOUBLE → 设置,退出回 READY(非菜单入口)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_READY);
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 500);
    assert(s.state == APP_ST_SETTINGS);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 600);
    assert(s.state == APP_ST_READY);                      // return_home=0

    // 菜单内进设置(行 1):退出回菜单(return_home=1)
    reset();
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_DOWN, now + 10); // → 行 1
    reduce_btn(APP_EV_KEY_CLICK, APP_BTN_OK, now + 20);
    assert(s.state == APP_ST_SETTINGS);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 600);
    assert(s.state == APP_ST_HOME);                       // 回菜单,不落 READY

    // 菜单内 OK 双击:直接进设置,退出回菜单(双击 = 设置的全态心智)
    reset();
    reduce_btn(APP_EV_KEY_DOUBLE, APP_BTN_OK, now + 10);
    assert(s.state == APP_ST_SETTINGS);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 600);
    assert(s.state == APP_ST_HOME);

    // 常开麦录音中 OK 长按 → 菜单,录音不动(mic_hold 保持、无收口动作)
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    assert(s.state == APP_ST_LISTENING && s.mic_hold);
    reduce_btn(APP_EV_KEY_LONG, APP_BTN_OK, now + 1000);
    assert(s.state == APP_ST_HOME);
    assert(s.mic_hold == true);
    assert(!has_action(APP_ACT_STREAM_CANCEL));
    assert(!has_action(APP_ACT_STREAM_STOP));
    assert(strstr(s.toast, "Menu") != NULL);
    // 守护进程重发 mic on:HOME → READY → LISTENING 自动回语音界面
    mic_ev(APP_EV_MIC_ON, now + 2000);
    assert(s.state == APP_ST_LISTENING && s.mic_hold);

    // 幽灵 OK 长按(松开电平)在录音中不回菜单
    reset();
    s.link_up = true;
    mic_ev(APP_EV_MIC_ON, now + 10);
    reduce(APP_EV_TONE_DONE, now + 200);
    reduce_btn_mv(APP_EV_KEY_LONG, APP_BTN_OK, 2890, now + 1000);
    assert(s.state == APP_ST_LISTENING);
}

int main(void) {
    test_home_nav();
    test_down_enter_clear();
    test_ptt_offline_online();
    test_tone_press_produce();
    test_tone_done_idempotent();
    test_tone_done_late_after_send();
    test_tone_release_sends_order();
    test_tone_tick_fallback();
    test_tone_done_ignored_elsewhere();
    test_listening_end();
    test_up_press_ptt();
    test_ptt_press_start_guards();
    test_timeouts();
    test_agent_running_timeout();
    test_agent_status_flow();
    test_approval();
    test_approval_wake();
    test_approval_during_listening();
    test_screen_off_wake();
    test_screen_off_ready();
    test_ok_long_locks();
    test_locked_keys_operate_screen_off();
    test_locked_approval_forces_wake();
    test_ok_long_unlocks();
    test_wake_no_relock();
    test_screen_off_keys_pass_through();
    test_panel_off_longest_paths();
    test_audio_error_closes_session();
    test_ptt_max_talk_watchdog();
    test_mic_on_from_home();
    test_mic_on_offline_no_residue();
    test_mic_60s_not_forced();
    test_mic_off_returns_ready();
    test_mic_swallows_release();
    test_mic_link_down_clears_hold();
    test_mic_busy_during_ptt();
    test_settings_enter_exit();
    test_settings_navigate_adjust();
    test_settings_timeout();
    test_settings_idle_screen_off();
    test_settings_mic_on_priority();
    test_mic_hold_double_cycles_tone();
    test_settings_set_event();
    test_settings_snapshot();
    test_settings_overlay_listening();
    test_menu_overlay_snapshot();
    test_back_to_menu();
    test_fake_key_is_not_activity();
    test_ok_long_lock_mv_gate();
    test_up_taps_never_clear();
    test_ptt_no_confirm_delay();
    test_relock_cycle();
    test_lock_guards_listening_approval();
    test_transcript_display();
    test_transcribing_exit();
    test_ble_link_down();
    test_ble_disconnect_transcribing();
    test_keys_ignored_in_running();
    test_listening_arrows_ignored();
    test_approval_no_timeout();
    test_agent_error();
    test_audio_drop_netbusy();
    test_ble_events();
    test_snapshot_agent_name();
    test_snapshot_link_up();
    test_usb_link_up();
    test_usb_link_down();
    test_time_set();
    test_bounded();
    test_dual_session_survives_other_channel();
    test_dual_active_channel_down_fails_over();
    test_dual_idle_last_connect_wins();
    test_wired_no_screen_off();
    printf("test_app_state: all assertions passed\n");
    return 0;
}
