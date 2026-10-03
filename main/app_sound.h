// main/app_sound.h —— 提示音模块。
// 方波提示音经 bsp_audio_write 输出(移植自 demo_audio.c 的 play_tone 模式)。
// 与录音严格分时:START 音由 sound_worker 播完 post TONE_DONE,app_task 收到才开流
// (S3 事件化,app_task 不再同步阻塞);其余提示音同样异步走队列。
// 播放门禁:app_sound_play 先过 tone_policy(档位=OFF 或夜间静音窗口内 → 不播,
// 返回 false)。START 音的调用方(main.c)对 false 已有"立即开流"兜底 —— 静音时
// 开麦反而比等滴声更快,无需额外路径。
#pragma once

#include "app_types.h"
#include "esp_err.h"
#include "tone_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

// 建静态队列 + sound_worker 任务(prio 3, 栈 2KB),并初始化 codec 为 16k/16/1。
esp_err_t app_sound_init(void);

// 配置播放策略(档位 + 夜间静音开关)。app_task 单写、sound_worker 读,
// 两个 volatile 标量,单核无需锁。开机(main.c)装载 NVS 值,设置变更时重配。
void app_sound_configure(tone_lvl_t lvl, bool night_mute);

// 当前档位(console `st` 展示;app_task 写侧的镜像读,撕裂可容忍)。
tone_lvl_t app_sound_level(void);

// 异步播放:入队由 sound_worker 播出,返回是否入队成功。
// 策略拒绝(档位 OFF / 夜窗内静音)同样返回 false —— START 音调用方须自行兜底
// (main.c run_actions 已有:false → 立即开流,不等 TONE_DONE)。
bool app_sound_play(app_tone_t tone);

// 同步播放:在调用者上下文中阻塞播完。仅保留给非 S3 场景的确定性播放
// (当前无调用方,防回归保留;START 已改异步)。同样受策略门禁。
void app_sound_play_sync(app_tone_t tone);

#ifdef __cplusplus
}
#endif
