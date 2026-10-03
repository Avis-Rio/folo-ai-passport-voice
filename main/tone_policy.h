// main/tone_policy.h —— 提示音播放策略(纯 C,宿主机可测)。
// 决策与播放解耦:状态机/声音模块只问"这个音现在允不允许播、用多大音量",
// 策略本身集中在这一个文件里 —— 档位语义、夜间静音窗口的单点真源。
// 不 include 任何 ESP-IDF 头(tests/ 直接编译)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 提示音档位(NVS 持久化取值域;数值即存储值,勿重排)
typedef enum {
    TONE_LVL_OFF = 0,    // 完全静音(所有提示音跳过)
    TONE_LVL_LOW,        // 低音量(≈30%)
    TONE_LVL_HIGH,       // 高音量(≈80%,2026-10 前的固定值,缺省档)
} tone_lvl_t;

#define TONE_LVL_COUNT   3
#define TONE_LVL_DEFAULT TONE_LVL_HIGH

// 夜间自动静音窗口(本地时间,分钟粒度,跨午夜):
// 21:30(含)起 → 次日 07:00(不含)止。窗口内所有提示音跳过。
// 依赖校时(Mac 守护进程 time.set);未校时(本地分钟 = -1)时窗口不生效 ——
// 与顶栏 "--:--" 同一语义:没有可信时间就不做时间相关决策。
#define TONE_NIGHT_START_MIN ((int)(21 * 60 + 30))   // 21:30
#define TONE_NIGHT_END_MIN   ((int)(7 * 60))         // 07:00

// 是否允许播放。lvl=OFF 一律否;night_mute 且 local_min 落在夜窗内也否。
// local_min:本地时间自午夜起的分钟数,未校时传 -1(夜窗不生效)。
bool tone_policy_allow(tone_lvl_t lvl, bool night_mute, int local_min);

// 档位对应的播放音量(0-100,%):OFF→0(不会播)/ LOW→30 / HIGH→80。
uint8_t tone_policy_volume(tone_lvl_t lvl);

// 档位循环切换(用户语义:越切越安静,关到底再回到响):
// HIGH → LOW → OFF → HIGH …
tone_lvl_t tone_policy_next(tone_lvl_t lvl);

// 档位短名(UI/console 共用)
const char *tone_policy_name(tone_lvl_t lvl);

#ifdef __cplusplus
}
#endif
