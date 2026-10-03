// tone_policy 单元测试(纯 C,assert 断言)。
// 覆盖:三档门禁、夜间静音窗口边界(21:30 含 / 07:00 不含)、未校时不静音、
// 音量映射、档位循环、档位名。
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tone_policy.h"

static void test_allow_levels(void) {
    // OFF:白天黑夜一律不播
    assert(!tone_policy_allow(TONE_LVL_OFF, false, 12 * 60));
    assert(!tone_policy_allow(TONE_LVL_OFF, true, 23 * 60));

    // HIGH/LOW:白天(夜窗外)照播
    assert(tone_policy_allow(TONE_LVL_HIGH, false, 12 * 60));
    assert(tone_policy_allow(TONE_LVL_LOW, false, 12 * 60));
    // 夜间静音关闭时,深夜也播(窗口由开关控制,不由钟点暗控)
    assert(tone_policy_allow(TONE_LVL_HIGH, false, 23 * 60));

    // 越界档位值:按不播处理(防御)
    assert(!tone_policy_allow((tone_lvl_t)7, false, 12 * 60));
    assert(!tone_policy_allow((tone_lvl_t)99, true, 12 * 60));
}

static void test_night_window(void) {
    // 窗口 21:30(含)– 07:00(不含),跨午夜
    assert(!tone_policy_allow(TONE_LVL_HIGH, true, 21 * 60 + 30));   // 21:30 边界:静音
    assert(tone_policy_allow(TONE_LVL_HIGH, true, 21 * 60 + 29));    // 21:29:还播
    assert(!tone_policy_allow(TONE_LVL_HIGH, true, 23 * 60));        // 深夜:静音
    assert(!tone_policy_allow(TONE_LVL_HIGH, true, 0));              // 午夜:静音
    assert(!tone_policy_allow(TONE_LVL_HIGH, true, 6 * 60 + 59));    // 06:59:静音
    assert(tone_policy_allow(TONE_LVL_HIGH, true, 7 * 60));          // 07:00 边界:恢复
    assert(tone_policy_allow(TONE_LVL_HIGH, true, 12 * 60));         // 正午:照播
    assert(tone_policy_allow(TONE_LVL_HIGH, true, 21 * 60 + 29));    // 窗口前:照播
}

static void test_unsynced_time(void) {
    // 未校时(local_min=-1):夜窗无从判断,不静音 —— 宁响勿哑
    assert(tone_policy_allow(TONE_LVL_HIGH, true, -1));
    assert(tone_policy_allow(TONE_LVL_LOW, true, -1));
    // 越界分钟数同(-1 / ≥1440)同语义防御
    assert(tone_policy_allow(TONE_LVL_HIGH, true, 24 * 60));
    assert(!tone_policy_allow(TONE_LVL_OFF, true, -1));   // OFF 仍不播(档位优先)
}

static void test_volume_and_cycle(void) {
    // 音量映射:HIGH=80(2026-10 前固定值),LOW=30,OFF=0(不播,值不消费)
    assert(tone_policy_volume(TONE_LVL_HIGH) == 80);
    assert(tone_policy_volume(TONE_LVL_LOW) == 30);
    assert(tone_policy_volume(TONE_LVL_OFF) == 0);

    // 循环:HIGH → LOW → OFF → HIGH(越切越安静,关到底再回响)
    assert(tone_policy_next(TONE_LVL_HIGH) == TONE_LVL_LOW);
    assert(tone_policy_next(TONE_LVL_LOW) == TONE_LVL_OFF);
    assert(tone_policy_next(TONE_LVL_OFF) == TONE_LVL_HIGH);

    // 档位名(UI/console)
    assert(strcmp(tone_policy_name(TONE_LVL_HIGH), "HIGH") == 0);
    assert(strcmp(tone_policy_name(TONE_LVL_LOW), "LOW") == 0);
    assert(strcmp(tone_policy_name(TONE_LVL_OFF), "OFF") == 0);
}

int main(void) {
    test_allow_levels();
    test_night_window();
    test_unsynced_time();
    test_volume_and_cycle();
    printf("test_tone_policy: all assertions passed\n");
    return 0;
}
