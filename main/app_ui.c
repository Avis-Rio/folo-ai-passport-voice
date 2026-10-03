// main/app_ui.c —— 产品 UI 实现。见 app_ui.h 布局说明。
#include "app_ui.h"
#include "bsp_battery.h"     // 电量(主循环已把真实值补进快照,此处仅渲染)
#include "bsp_display.h"
#include "time_sync.h"       // 顶栏 HH:MM(校时源仅电脑客户端,未校时 "--:--");设置页时区
#include "tone_policy.h"     // 设置页档位名(单点真源,UI 不自持一份)
#include "ui_pixel.h"
#include "esp_app_desc.h"    // 开机画面版本号(esp_app_get_description,IDF 5.5 头名)
#include "lvgl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// 物理审批器:审批页内容可能含中文(标题/目标/差异摘要),内置 montserrat
// 只有拉丁字形 → 全变方块。lv_font_conv 生成的 GB2312 全字库(7540 字形,
// bpp2,~1MB flash,由 lv_font_cjk_*.c 提供)。
LV_FONT_DECLARE(lv_font_cjk_20);
LV_FONT_DECLARE(lv_font_cjk_14);

// ---- 布局常量 ----
#define BAR_H        26   // 顶栏高
#define BANNER_Y     28   // OFFLINE / NET BUSY 横幅
#define BANNER_H     18
#define CONTENT_Y    48   // 内容区起点
#define HINT_Y       286  // 各页底部提示行
#define W            240
#define H            320

// 顶栏(无 BLE 点、无 CPU/RAM 监测):右侧对齐组:电池图标(描边框+右缘
// 触点,数字居中)、HH:MM 时间(最右贴 6px)。240px:144+34+4 / 182+52。
#define BATT_X       144
#define BATT_W       34
#define BATT_H       16
#define BATT_NUB_X   (BATT_X + BATT_W)   // 右缘触点
#define BATT_NUB_W   4
#define TIME_X       182
#define TIME_W       52

// ---- 页内部件索引(与 page 切换共用) ----
typedef struct {
    lv_obj_t *root;                       // 本页容器(显隐切换)
    lv_obj_t *rec_label;                  // LISTENING:RECORDING 大字
    lv_obj_t *rec_elapsed;                // LISTENING:计时
    lv_obj_t *tr_message;                 // TRANSCRIBING:消息
    lv_obj_t *run_state;                  // AGENT_RUNNING:状态名
    lv_obj_t *run_message;                // AGENT_RUNNING:消息
    lv_obj_t *ap_risk_banner;             // APPROVAL:风险条
    lv_obj_t *ap_risk_label;              // APPROVAL:风险文本
    lv_obj_t *ap_title;                   // APPROVAL:标题
    lv_obj_t *ap_target;                  // APPROVAL:目标
    lv_obj_t *ap_diff;                    // APPROVAL:摘要/详情
    lv_obj_t *set_panels[3];              // SETTINGS:三行选项面板(高亮=选中)
    lv_obj_t *set_values[3];              // SETTINGS:右侧当前值
    lv_obj_t *menu_panels[3];             // HOME 菜单:三行(语音输入/设置/固件)
    lv_obj_t *menu_values[3];             // HOME 菜单右侧(FW 槽状态)
    lv_obj_t *ask_title;                  // ASK:标题(选项选择,物理审批器 v2.4)
    lv_obj_t *ask_panels[APP_OPTS_MAX];   // ASK:选项行(高亮=选中)
    lv_obj_t *ask_labels[APP_OPTS_MAX];   // ASK:选项文本
} page_t;

static lv_obj_t *s_chrome;                // 顶层容器(lv_layer_top)
static lv_obj_t *s_batt_icon;   // 电池描边框(数字居中在框内)
static lv_obj_t *s_batt_nub;    // 右缘触点
static lv_obj_t *s_batt_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_offline_banner;
static lv_obj_t *s_offline_text;
static lv_obj_t *s_netbusy_banner;
static lv_obj_t *s_netbusy_text;
static lv_obj_t *s_toast;

static page_t s_pages[APP_ST_COUNT];
static app_stage_t s_cur_page = APP_ST_COUNT;
static bool s_last_screen_on = true;
static lv_obj_t *s_bg;   // 基底屏:所有状态页都是它的子对象(单屏方案)

static const char *const RISK_NAMES[APP_RISK_COUNT] = { "LOW RISK", "MEDIUM RISK", "HIGH RISK" };
static const uint32_t RISK_COLORS[APP_RISK_COUNT] = { UI_GRASS, UI_YELLOW, UI_RED };

// ---- 基础块(无 LVGL 样式噪音) ----
static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static lv_obj_t *hint_label(lv_obj_t *parent, const char *text)
{
    return label(parent, text, &lv_font_montserrat_14, UI_MUTED, 0, HINT_Y, W);
}

// ---- 基底屏:天空 + 云 + 草地(复用 ui_pixel 视觉语言) ----
static void build_background(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);  // LVGL 9.5: 创建顶层 screen(旧 lv_screen_create 已移除)
    s_bg = scr;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_SKY), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 云(简版,取自 ui_pixel_screen_create)
    block(scr, 189, 15, 43, 10, UI_INK);
    block(scr, 193, 12, 35, 10, 0xFFFFFF);
    block(scr, 200, 8, 10, 9, 0xFFFFFF);
    block(scr, 215, 9, 9, 8, 0xFFFFFF);

    // 草地 + 纹理
    block(scr, 0, 286, 240, 34, UI_GRASS);
    block(scr, 0, 286, 240, 4, 0xA7D93E);
    for (int x = 0; x < 240; x += 30) {
        block(scr, x, 312, 18, 8, UI_GRASS_DARK);
        block(scr, x + 18, 316, 12, 4, 0x75452E);
    }
    lv_screen_load(scr);
}

// ---- chrome:常驻顶栏 / 横幅 / Toast(顶层,所有页共用) ----
static void build_chrome(void)
{
    s_chrome = lv_display_get_layer_top(lv_display_get_default());  // LVGL 9.5 改名
    lv_obj_remove_flag(s_chrome, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_chrome, 0, 0);

    // 顶栏(无 BLE 点:USB 模式下 BLE 连接态无意义,用户要求删除)
    lv_obj_t *bar = block(s_chrome, 0, 0, W, BAR_H, UI_INK);
    (void)bar;
    // 电池图标:描边框 + 右缘触点,数字居中(无 % 后缀)
    s_batt_icon = block(s_chrome, BATT_X, 5, BATT_W, BATT_H, UI_INK);
    lv_obj_set_style_border_width(s_batt_icon, 2, 0);
    lv_obj_set_style_border_color(s_batt_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(s_batt_icon, 3, 0);
    s_batt_nub = block(s_chrome, BATT_NUB_X, 10, BATT_NUB_W, 6, 0xFFFFFF);
    // 数字位置在 render 按真实字宽/字形高精确计算(batt_label_reposition):
    // 实测 montserrat_14 数字字形高 10px,label 行高 16 → y=1 靠上视觉居中
    s_batt_label = label(s_batt_icon, "--", &lv_font_montserrat_14, 0xFFFFFF,
                         0, -2, BATT_W - 4);
    s_time_label = label(s_chrome, "--:--", &lv_font_montserrat_14, 0xFFFFFF,
                         TIME_X, 5, TIME_W);

    // BLE 断线横幅(链路断时整宽显示;link_up = EVENT 特征已订阅)。
    // 文本是子 label(render 按通道名改写文案);banner 本体是 block,不能
    // 对 block 调 label_set_*(会按 label 布局读越界内存 → Load access fault)。
    s_offline_banner = block(s_chrome, 0, BANNER_Y, W, BANNER_H, UI_RED);
    s_offline_text = label(s_offline_banner, "BLE DISCONNECTED - reconnecting...",
                           &lv_font_montserrat_14, 0xFFFFFF, 0, 0, W);

    // BLE BUSY(音频丢帧中)
    s_netbusy_banner = block(s_chrome, 0, BANNER_Y, W, BANNER_H, UI_ORANGE);
    s_netbusy_text = label(s_netbusy_banner, "BLE BUSY - dropping frames",
                           &lv_font_montserrat_14, UI_INK, 0, 0, W);

    // Toast(底部浮层,空文本即隐藏)
    lv_obj_t *tbg = block(s_chrome, 30, 272, 180, 30, UI_INK);
    // toast 用 CJK 字库(含 ASCII):v2.4 起选项确认会把中文选项文本放进 toast
    s_toast = label(tbg, "", &lv_font_cjk_14, 0xFFFFFF, 0, 5, 180);
}

// ---- 各状态页 ----
static void build_home(void)
{
    // 菜单首页(2026-10-03 v2):HOME 从"待机语"变成真正的启动器 ——
    // 0=Voice Input(进语音输入法)/ 1=Settings / 2=Firmware B(长按切槽)。
    // 行样式与设置页同一语言(纸面板 + 选中黄底),键位心智零新增。
    page_t *p = &s_pages[APP_ST_HOME];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    label(p->root, "MENU", &lv_font_montserrat_20, UI_INK, 0, CONTENT_Y + 8, W);

    static const char *const names[3] = {
        "Voice Input", "Settings", "Firmware B",
    };
    for (int i = 0; i < 3; i++) {
        const int y = 104 + i * 48;
        p->menu_panels[i] = ui_pixel_panel_create(p->root, 20, y, 200, 36, UI_PAPER);
        lv_obj_t *name = label(p->menu_panels[i], names[i], &lv_font_montserrat_14,
                               UI_INK, 0, 6, 118);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
        p->menu_values[i] = label(p->menu_panels[i], i < 2 ? ">" : "--",
                                  &lv_font_montserrat_14, UI_INK, 118, 6, 68);
        lv_obj_set_style_text_align(p->menu_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    // 键位提示:切固件是重启级操作,只认 OK 长按(单击在菜单第 2 行故意无动作)。
    hint_label(p->root, "VOL: SELECT   OK: OPEN   HOLD OK: FW");
}

// ---- 选项选择页(物理审批器 v2.4):标题(两行)+ 1~4 行选项,VOL± 移动高亮,
// OK 确认,OK 长按整题拒绝。空选项行隐藏(count 不足时)。
static void build_ask(void)
{
    page_t *p = &s_pages[APP_ST_ASK];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    p->ask_title = label(p->root, "", &lv_font_cjk_20, UI_INK, 12, CONTENT_Y + 4, 216);
    lv_obj_set_style_text_align(p->ask_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(p->ask_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(p->ask_title, 54);

    for (int i = 0; i < APP_OPTS_MAX; i++) {
        const int y = 116 + i * 46;
        p->ask_panels[i] = ui_pixel_panel_create(p->root, 12, y, 216, 40, UI_PAPER);
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        label(p->ask_panels[i], num, &lv_font_montserrat_14, UI_MUTED, 6, 10, 16);
        lv_obj_t *txt = label(p->ask_panels[i], "", &lv_font_cjk_14, UI_INK, 26, 10, 182);
        lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_LEFT, 0);
        p->ask_labels[i] = txt;
    }
    hint_label(p->root, "VOL: MOVE   OK: PICK   HOLD OK: CANCEL");
}

static void build_ready(void)
{
    page_t *p = &s_pages[APP_ST_READY];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    // 工作流切换已取消(固定 build),READY 为简单就绪页
    label(p->root, "READY", &lv_font_montserrat_20, UI_INK, 0, CONTENT_Y + 24, W);
    // 键位速查(2026-10-03):旧单行 hint 塞不下真实键位还带着过时的
    // "DBL-VOL+: CLEAR"(清空自 2026-08-29 起在 DOWN 长按)—— 改为四行速查,
    // 与实际状态机语义一一对应(app_state.c handle_key)。
    label(p->root, "HOLD VOL+ : TALK", &lv_font_montserrat_14, UI_MUTED, 0, 168, W);
    label(p->root, "TAP VOL- : ENTER", &lv_font_montserrat_14, UI_MUTED, 0, 192, W);
    label(p->root, "HOLD VOL- : CLEAR", &lv_font_montserrat_14, UI_MUTED, 0, 216, W);
    label(p->root, "DOUBLE OK : SETTINGS", &lv_font_montserrat_14, UI_MUTED, 0, 240, W);
    hint_label(p->root, "HOLD VOL+ TO SPEAK");
}

static void build_listening(void)
{
    page_t *p = &s_pages[APP_ST_LISTENING];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    // 录音中不画图标, 直接文字:RECORDING 大字居中(原图标区 y58-131 的中心),
    // 简洁直观; 下方 "0s" 计时与 hint 保持。
    p->rec_label = label(p->root, "RECORDING", &lv_font_montserrat_20, UI_INK, 0, 86, W);

    p->rec_elapsed = label(p->root, "0s", &lv_font_montserrat_20, UI_INK, 0, 200, W);
    hint_label(p->root, "RELEASE VOL+: SEND");
}

static void build_transcribing(void)
{
    page_t *p = &s_pages[APP_ST_TRANSCRIBING];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    label(p->root, "Transcribing...", &lv_font_montserrat_20, UI_INK, 0, 96, W);
    p->tr_message = label(p->root, "", &lv_font_montserrat_14, UI_MUTED, 20, 140, 200);
    hint_label(p->root, "PLEASE WAIT");
}

static void build_running(void)
{
    page_t *p = &s_pages[APP_ST_AGENT_RUNNING];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    block(p->root, 108, 76, 24, 24, UI_SKY_DARK);              // 静态"spinner"块
    p->run_state = label(p->root, "running", &lv_font_montserrat_20, UI_INK,
                         0, 112, W);
    p->run_message = label(p->root, "", &lv_font_montserrat_14, UI_MUTED,
                           20, 148, 200);
    hint_label(p->root, "AGENT WORKING...");
}

static void build_approval(void)
{
    page_t *p = &s_pages[APP_ST_APPROVAL];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    p->ap_risk_banner = block(p->root, 20, CONTENT_Y + 8, 200, 28, UI_GRASS);
    p->ap_risk_label = label(p->ap_risk_banner, "", &lv_font_montserrat_14, UI_INK,
                             0, 5, 200);

    p->ap_title = label(p->root, "", &lv_font_cjk_20, UI_INK, 20, 108, 200);
    lv_obj_set_style_text_align(p->ap_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(p->ap_title, LV_LABEL_LONG_WRAP);

    p->ap_target = label(p->root, "", &lv_font_cjk_14, UI_SKY_DARK, 20, 150, 200);
    p->ap_diff = label(p->root, "", &lv_font_cjk_14, UI_MUTED, 20, 176, 200);
    lv_obj_set_style_text_align(p->ap_diff, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(p->ap_diff, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(p->ap_diff, 88);

    hint_label(p->root, "OK: APPROVE   VOL+: REJECT   DOWN: ENTER");
}

// ---- 设置页(2026-10-03):三行选项,选中高亮,右侧当前值 ----
// 行内布局:name 左对齐 118px | value 右对齐 68px(面板 pad 7,内容区 186px)。
static void build_settings(void)
{
    page_t *p = &s_pages[APP_ST_SETTINGS];
    p->root = lv_obj_create(s_bg);   // 基底屏的子对象:切换只显隐,不动活动屏
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_set_size(p->root, W, H);
    lv_obj_set_pos(p->root, 0, 0);

    label(p->root, "SETTINGS", &lv_font_montserrat_20, UI_INK, 0, CONTENT_Y + 8, W);

    static const char *const names[3] = {
        "Sound", "Night 21:30-7", "Timezone",
    };
    for (int i = 0; i < 3; i++) {
        const int y = 104 + i * 48;
        p->set_panels[i] = ui_pixel_panel_create(p->root, 20, y, 200, 36, UI_PAPER);
        lv_obj_t *name = label(p->set_panels[i], names[i], &lv_font_montserrat_14,
                               UI_INK, 0, 6, 118);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
        p->set_values[i] = label(p->set_panels[i], "-", &lv_font_montserrat_14,
                                 UI_INK, 118, 6, 68);
        lv_obj_set_style_text_align(p->set_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    hint_label(p->root, "VOL: MOVE   OK: CHANGE   HOLD OK: EXIT");
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ---- 开机画面(2026-10-03):品牌 + mascot + 版本号,1.2s 单次自灭 ----
// 挂在 layer_top(chrome 同层,创建在后 → 盖住顶栏/横幅/所有页);LVGL timer
// 回调跑在 LVGL 任务上下文,删对象/删定时器不需要 app_task 的锁。
// 状态机不参与:启动前 3s 按键本就被 ADC 防腐蚀门禁忽略(button_adc_set_ignore_until),
// splash 期间用户按键无副作用;到点自灭,无需事件。
static lv_obj_t *s_splash;
#define SPLASH_MS 1200

static void splash_dismiss(lv_timer_t *t)
{
    if (s_splash) {
        lv_obj_delete(s_splash);
        s_splash = NULL;
    }
    lv_timer_delete(t);   // 单次:自删
}

static void build_splash(void)
{
    s_splash = lv_obj_create(s_chrome);
    lv_obj_remove_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_size(s_splash, W, H);
    lv_obj_set_style_radius(s_splash, 0, 0);
    lv_obj_set_style_border_width(s_splash, 0, 0);
    lv_obj_set_style_pad_all(s_splash, 0, 0);
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(UI_SKY), 0);

    // 云(与基底屏同一视觉语言,ui_pixel_screen_create 同款)
    block(s_splash, 189, 15, 43, 10, UI_INK);
    block(s_splash, 193, 12, 35, 10, 0xFFFFFF);
    block(s_splash, 200, 8, 10, 9, 0xFFFFFF);
    block(s_splash, 215, 9, 9, 8, 0xFFFFFF);

    label(s_splash, "AI PASSPORT", &lv_font_montserrat_20, UI_INK, 0, 92, W);
    ui_pixel_mascot_create(s_splash, 101, 140);   // 38x48,水平居中
    {
        char ver[24];
        // 精度限定 %.20s:GCC -Wformat-truncation 可证明输出 ≤21B < 24,
        // 版本串异常超长时截断显示(而非构建告警炸掉 -Werror)。
        snprintf(ver, sizeof(ver), "v%.20s",
                 esp_app_get_description()->version);
        label(s_splash, ver, &lv_font_montserrat_14, UI_MUTED, 0, 210, W);
    }
    lv_timer_create(splash_dismiss, SPLASH_MS, NULL);
}

// U1 dirty-check:文本未变则跳过 set_text。set_text 会重排标签并标记整行
// 无效重绘——LISTENING 10fps 渲染下反复写相同文本(计时秒数、电量、CPU/RAM、
// agent 消息)是无谓开销。lv_label_get_text 返回当前文本,比较后决定是否写。
static void label_set_if_changed(lv_obj_t *l, const char *text)
{
    if (!l || !text) return;
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(l, text);
}

static void label_set_fmt_if_changed(lv_obj_t *l, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    label_set_if_changed(l, buf);
}

// 转写预览态:文本尾部附一个光标感字符 '_'(未定稿视觉);定稿后移除。
// 改动最小方案:不动样式,只改文本。栈缓冲覆盖 满长文本 + 光标。
static void set_agent_message(lv_obj_t *label_obj, const char *msg, bool preview)
{
    char buf[APP_AGENT_MSG_MAX + 2];
    if (preview && msg && msg[0]) {
        snprintf(buf, sizeof(buf), "%s_", msg);
        label_set_if_changed(label_obj, buf);
    } else {
        label_set_if_changed(label_obj, msg ? msg : "");
    }
}

// ---- 页切换 ----
static void show_page(app_stage_t st)
{
    if (st == s_cur_page) return;
    if (s_cur_page < APP_ST_COUNT) lv_obj_add_flag(s_pages[s_cur_page].root, LV_OBJ_FLAG_HIDDEN);
    if (st < APP_ST_COUNT) lv_obj_remove_flag(s_pages[st].root, LV_OBJ_FLAG_HIDDEN);
    s_cur_page = st;
}

esp_err_t app_ui_init(void)
{
    build_background();
    build_chrome();
    build_home();
    build_ready();
    build_listening();
    build_transcribing();
    build_running();
    build_approval();
    build_ask();
    build_settings();
    for (int i = 0; i < APP_ST_COUNT; i++) {
        lv_obj_add_flag(s_pages[i].root, LV_OBJ_FLAG_HIDDEN);
    }
    show_page(APP_ST_HOME);
    build_splash();   // 最后建:盖在 chrome 之上,1.2s 自灭
    return ESP_OK;
}

// ---- 渲染 ----
void app_ui_render(const app_ui_snapshot_t *snap)
{
    // 息屏/唤醒:背光切换(内容照常更新,唤醒后即为最新)
    if (snap->screen_on != s_last_screen_on) {
        bsp_display_backlight(snap->screen_on ? 100 : 0);
        s_last_screen_on = snap->screen_on;
    }
    if (!snap->screen_on) return;

    // ---- chrome ----
    if (snap->battery_available) {
        label_set_fmt_if_changed(s_batt_label, "%d", snap->battery_soc);
    } else {
        label_set_if_changed(s_batt_label, "--");
    }
    {
        char t[16];
        time_sync_format_local(t, sizeof(t));
        label_set_if_changed(s_time_label, t);
    }

    // 横幅互斥:OFFLINE(通道断线)> BUSY(同位置 BANNER_Y)
    // 文案按当前链路通道渲染(BLE/USB;断线横幅显示 link_name 字样)
    label_set_fmt_if_changed(s_offline_text, "%s DISCONNECTED - reconnecting...",
                             snap->link_name);
    label_set_fmt_if_changed(s_netbusy_text, "%s BUSY - dropping frames",
                             snap->link_name);
    set_hidden(s_offline_banner, snap->link_up);
    set_hidden(s_netbusy_banner, !snap->net_busy || !snap->link_up);

    if (snap->toast[0]) {
        label_set_if_changed(s_toast, snap->toast);
        set_hidden(lv_obj_get_parent(s_toast), false);
    } else {
        set_hidden(lv_obj_get_parent(s_toast), true);
    }

    // ---- 页内容 ----
    // 录音中设置浮层(2026-10-03 v2):state 仍 LISTENING,页切到设置并按设置
    // 渲染 —— 与全状态设置页共用同一组控件;浮层退出后 show_page 回录音页。
    const app_stage_t page = snap->settings_overlay ? APP_ST_SETTINGS : snap->state;
    show_page(page);
    switch (page) {
    case APP_ST_HOME: {
        // 菜单首页:选中高亮 + 固件槽状态(开机探测落地,见 APP_EV_SLOT_PROBE)
        page_t *hp = &s_pages[APP_ST_HOME];
        for (int i = 0; i < 3; i++) {
            ui_pixel_set_selected(hp->menu_panels[i], i == snap->menu_sel, true);
        }
        label_set_if_changed(hp->menu_values[0], ">");
        label_set_if_changed(hp->menu_values[1], ">");
        label_set_if_changed(hp->menu_values[2], snap->slot_b_present ? "ready" : "--");
        break;
    }
    case APP_ST_LISTENING:
        // 录音中只显示麦克风图标(静态), 无音量可视化; 仅计时实时刷新
        label_set_fmt_if_changed(s_pages[APP_ST_LISTENING].rec_elapsed, "%ds",
                                 snap->elapsed_ms / 1000);
        break;
    case APP_ST_TRANSCRIBING:
        set_agent_message(s_pages[APP_ST_TRANSCRIBING].tr_message,
                          snap->agent_message, !snap->transcript_final);
        break;
    case APP_ST_AGENT_RUNNING:
        label_set_if_changed(s_pages[APP_ST_AGENT_RUNNING].run_state,
                             snap->agent_state_name);
        set_agent_message(s_pages[APP_ST_AGENT_RUNNING].run_message,
                          snap->agent_message, !snap->transcript_final);
        break;
    case APP_ST_APPROVAL: {
        uint8_t r = snap->approval_risk < APP_RISK_COUNT ? snap->approval_risk : APP_RISK_MEDIUM;
        lv_obj_set_style_bg_color(s_pages[APP_ST_APPROVAL].ap_risk_banner,
                                  lv_color_hex(RISK_COLORS[r]), 0);
        label_set_if_changed(s_pages[APP_ST_APPROVAL].ap_risk_label, RISK_NAMES[r]);
        label_set_if_changed(s_pages[APP_ST_APPROVAL].ap_title, snap->approval_title);
        label_set_fmt_if_changed(s_pages[APP_ST_APPROVAL].ap_target, "target: %s",
                                 snap->approval_target);
        label_set_if_changed(s_pages[APP_ST_APPROVAL].ap_diff, snap->approval_diff);
        break;
    }
    case APP_ST_ASK: {
        // 选项选择(物理审批器 v2.4):标题 + 选项行显隐 + 高亮选中行。
        page_t *kp = &s_pages[APP_ST_ASK];
        label_set_if_changed(kp->ask_title, snap->ask_title);
        for (int i = 0; i < APP_OPTS_MAX; i++) {
            const bool has = i < snap->ask_count;
            set_hidden(kp->ask_panels[i], !has);
            if (has) {
                label_set_if_changed(kp->ask_labels[i], snap->ask_opts[i]);
            }
            ui_pixel_set_selected(kp->ask_panels[i], has && i == snap->ask_sel, true);
        }
        break;
    }
    case APP_ST_SETTINGS: {
        // 选中高亮(ui_pixel_set_selected:黄底=选中,纸底=未选)+ 右侧当前值。
        // 档位名走 tone_policy 单点真源;时区直接读 time_sync(app_ui 已依赖)。
        page_t *sp = &s_pages[APP_ST_SETTINGS];
        const uint8_t lvl = snap->tone_level < TONE_LVL_COUNT ? snap->tone_level
                                                              : TONE_LVL_DEFAULT;
        for (int i = 0; i < 3; i++) {
            ui_pixel_set_selected(sp->set_panels[i], i == snap->settings_sel, true);
        }
        label_set_if_changed(sp->set_values[0], tone_policy_name((tone_lvl_t)lvl));
        label_set_if_changed(sp->set_values[1], snap->night_mute ? "ON" : "OFF");
        label_set_fmt_if_changed(sp->set_values[2], "UTC%+d", time_sync_tz_hour());
        break;
    }
    default:
        break;
    }
}
