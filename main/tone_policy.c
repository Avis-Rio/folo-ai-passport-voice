// main/tone_policy.c —— 提示音播放策略实现(见 tone_policy.h)。
#include "tone_policy.h"

bool tone_policy_allow(tone_lvl_t lvl, bool night_mute, int local_min)
{
    if ((int)lvl <= TONE_LVL_OFF || (int)lvl >= TONE_LVL_COUNT) {
        return false;   // OFF 与越界值都不播
    }
    if (!night_mute) {
        return true;
    }
    if (local_min < 0 || local_min >= 24 * 60) {
        return true;   // 未校时/越界:夜窗无从判断,不静音(宁响勿哑)
    }
    // 跨午夜窗口:start(21:30) > end(07:00)。
    // 静音 = 从 start 到午夜(含 start),或从午夜到 end(不含 end)。
    if (local_min >= TONE_NIGHT_START_MIN || local_min < TONE_NIGHT_END_MIN) {
        return false;
    }
    return true;
}

uint8_t tone_policy_volume(tone_lvl_t lvl)
{
    switch (lvl) {
    case TONE_LVL_LOW:  return 30;   // 柔和确认,夜间白天都可用
    case TONE_LVL_HIGH: return 80;   // 与 2026-10 之前的固定值一致(缺省不变)
    default:            return 0;    // OFF(播放已被 allow 拦下,值不会用到)
    }
}

tone_lvl_t tone_policy_next(tone_lvl_t lvl)
{
    switch (lvl) {
    case TONE_LVL_HIGH: return TONE_LVL_LOW;
    case TONE_LVL_LOW:  return TONE_LVL_OFF;
    default:            return TONE_LVL_HIGH;   // OFF → HIGH(关到底再开)
    }
}

const char *tone_policy_name(tone_lvl_t lvl)
{
    switch (lvl) {
    case TONE_LVL_LOW:  return "LOW";
    case TONE_LVL_HIGH: return "HIGH";
    default:            return "OFF";
    }
}
