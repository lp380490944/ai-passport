// main/demo_todo.c —— 像素风 Todo 清单,NVS 持久化。
//
// 交互:
//   上/下 短按   移动选中项(循环,自动滚动窗口)
//   确定  短按   勾选/取消选中项
//   上    长按   从模板池新增一条任务
//   下    长按   删除选中项
//   确定  长按   返回菜单(main.c 统一拦截)
#include "demo.h"
#include "demo_radio.h"   // 复用 NVS 初始化
#include "ui_pixel.h"
#include "bsp_battery.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "todo";

#define TODO_MAX      10
#define TODO_TEXT_LEN 24
#define TODO_VISIBLE  4          // 屏上同时显示的行数
#define TODO_NVS_NS   "todo"
#define TODO_NVS_KEY  "list"
#define TODO_BLOB_VER 1

typedef struct __attribute__((packed)) {
    uint8_t done;
    char    text[TODO_TEXT_LEN];
} todo_item_t;

typedef struct __attribute__((packed)) {
    uint8_t     ver;
    uint8_t     count;
    todo_item_t items[TODO_MAX];
} todo_blob_t;

// 首次使用的种子任务与"新增"模板池(设备无键盘,新增 = 从池中轮流取一条)。
static const char *SEED[] = { "Drink water", "Stretch 5 min", "Read 10 pages" };
static const char *TEMPLATES[] = {
    "Walk outside", "Tidy desk", "Call family", "Write journal",
    "Early sleep",  "Drink water", "Stretch 5 min", "Read 10 pages",
};
#define TEMPLATE_COUNT (sizeof(TEMPLATES) / sizeof(TEMPLATES[0]))

// ---- 数据状态 ----
static todo_blob_t s_data;
static bool s_dirty;             // 有未落盘修改;由 LVGL 定时器批量写 NVS
static int  s_sel;               // 选中项索引
static int  s_top;               // 滚动窗口第一项索引
static int  s_tpl;               // 下一个模板索引

// ---- UI 对象 ----
static lv_obj_t   *s_scr, *s_mascot, *s_batt;
static lv_obj_t   *s_stat_label, *s_bar_fill, *s_empty;
static lv_obj_t   *s_arrow_up, *s_arrow_down;
static lv_obj_t   *s_row_wrap[TODO_VISIBLE];   // 行容器(含阴影,可整体隐藏)
static lv_obj_t   *s_row_panel[TODO_VISIBLE];
static lv_obj_t   *s_row_box[TODO_VISIBLE];    // 勾选框
static lv_obj_t   *s_row_check[TODO_VISIBLE];  // 勾选框里的对号
static lv_obj_t   *s_row_text[TODO_VISIBLE];
static lv_timer_t *s_timer;

// ---- 持久化 ----

static void todo_save(void)
{
    nvs_handle_t h;
    if (nvs_open(TODO_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败,本次修改未保存");
        return;
    }
    esp_err_t err = nvs_set_blob(h, TODO_NVS_KEY, &s_data, sizeof(s_data));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_dirty = false;
    else ESP_LOGW(TAG, "NVS 写入失败: %s", esp_err_to_name(err));
}

static void todo_seed(void)
{
    memset(&s_data, 0, sizeof(s_data));
    s_data.ver = TODO_BLOB_VER;
    s_data.count = sizeof(SEED) / sizeof(SEED[0]);
    for (int i = 0; i < s_data.count; i++) {
        strlcpy(s_data.items[i].text, SEED[i], TODO_TEXT_LEN);
    }
    s_dirty = true;
}

static void todo_load(void)
{
    if (demo_radio_nvs_prepare() != ESP_OK) { todo_seed(); return; }

    nvs_handle_t h;
    if (nvs_open(TODO_NVS_NS, NVS_READONLY, &h) != ESP_OK) { todo_seed(); return; }
    size_t len = sizeof(s_data);
    esp_err_t err = nvs_get_blob(h, TODO_NVS_KEY, &s_data, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(s_data) ||
        s_data.ver != TODO_BLOB_VER || s_data.count > TODO_MAX) {
        todo_seed();
    }
}

// ---- 渲染 ----

static int done_count(void)
{
    int n = 0;
    for (int i = 0; i < s_data.count; i++) n += s_data.items[i].done ? 1 : 0;
    return n;
}

static void refresh(void)
{
    int done = done_count();

    // 进度:数字 + 草绿色填充条
    lv_label_set_text_fmt(s_stat_label, "%d/%d", done, s_data.count);
    int w = s_data.count ? (done * 84) / s_data.count : 0;
    if (w > 0) {
        lv_obj_remove_flag(s_bar_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_bar_fill, w);
    } else {
        lv_obj_add_flag(s_bar_fill, LV_OBJ_FLAG_HIDDEN);
    }

    // 列表窗口
    for (int slot = 0; slot < TODO_VISIBLE; slot++) {
        int idx = s_top + slot;
        if (idx >= s_data.count) {
            lv_obj_add_flag(s_row_wrap[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_row_wrap[slot], LV_OBJ_FLAG_HIDDEN);

        const todo_item_t *it = &s_data.items[idx];
        ui_pixel_set_selected(s_row_panel[slot], idx == s_sel, true);

        lv_obj_set_style_bg_color(s_row_box[slot],
            lv_color_hex(it->done ? UI_GRASS : 0xFFFFFF), 0);
        if (it->done) lv_obj_remove_flag(s_row_check[slot], LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(s_row_check[slot], LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(s_row_text[slot], it->text);
        lv_obj_set_style_text_color(s_row_text[slot],
            lv_color_hex(it->done ? 0x8A94A0 : UI_INK), 0);
        lv_obj_set_style_text_decor(s_row_text[slot],
            it->done ? LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE, 0);
    }

    // 空清单提示与滚动箭头
    if (s_data.count == 0) lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    else                   lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    if (s_top > 0) lv_obj_remove_flag(s_arrow_up, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(s_arrow_up, LV_OBJ_FLAG_HIDDEN);
    if (s_top + TODO_VISIBLE < s_data.count)
        lv_obj_remove_flag(s_arrow_down, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_arrow_down, LV_OBJ_FLAG_HIDDEN);
}

static void clamp_view(void)
{
    if (s_data.count == 0) { s_sel = 0; s_top = 0; return; }
    if (s_sel >= s_data.count) s_sel = s_data.count - 1;
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + TODO_VISIBLE) s_top = s_sel - TODO_VISIBLE + 1;
}

// lv_timer 跑在 LVGL 任务里:刷新电量,并把脏数据批量落盘
// (避免在按键回调所在的 button 任务里做 flash 写入)。
static void tick(lv_timer_t *t)
{
    (void)t;
    int soc = bsp_battery_soc();
    if (soc < 0) lv_label_set_text(s_batt, "");     // 读不到就不画,优雅降级
    else         lv_label_set_text_fmt(s_batt, "%d%%", soc);
    lv_obj_set_style_text_color(s_batt,
        (soc >= 0 && soc < 20) ? lv_color_hex(UI_RED) : lv_color_hex(0xFFFFFF), 0);

    if (s_dirty) todo_save();
}

// ---- 生命周期 ----

void demo_todo_enter(void)
{
    todo_load();
    s_sel = 0;
    s_top = 0;

    s_scr = ui_pixel_screen_create("TODO");

    // 电量:右上角蓝天空闲区,白云(x188,y8)下方,不遮挡云朵。
    s_batt = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -8, 28);

    // 进度面板:左侧数字,右侧像素进度条
    lv_obj_t *stat = ui_pixel_panel_create(s_scr, 11, 50, 190, 30, UI_YELLOW);
    lv_obj_set_style_pad_all(stat, 0, 0);
    s_stat_label = ui_pixel_label(stat, "0/0", &lv_font_montserrat_14, UI_INK);
    lv_obj_align(s_stat_label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *track = lv_obj_create(stat);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(track, 92, 14);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_radius(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(track, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(track, 3, 0);
    s_bar_fill = lv_obj_create(track);
    lv_obj_remove_flag(s_bar_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_bar_fill, 1, 1);
    lv_obj_set_size(s_bar_fill, 1, 6);
    lv_obj_set_style_radius(s_bar_fill, 0, 0);
    lv_obj_set_style_border_width(s_bar_fill, 0, 0);
    lv_obj_set_style_bg_color(s_bar_fill, lv_color_hex(UI_GRASS), 0);

    // 滚动箭头:进度面板右侧的蓝天竖条
    s_arrow_up = ui_pixel_label(s_scr, LV_SYMBOL_UP, &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_set_pos(s_arrow_up, 213, 52);
    s_arrow_down = ui_pixel_label(s_scr, LV_SYMBOL_DOWN, &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_set_pos(s_arrow_down, 213, 68);

    // 任务行:透明容器包住阴影 + 面板,便于整体隐藏
    for (int i = 0; i < TODO_VISIBLE; i++) {
        lv_obj_t *wrap = lv_obj_create(s_scr);
        lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(wrap, 11, 88 + i * 38);
        lv_obj_set_size(wrap, 224, 40);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        s_row_wrap[i] = wrap;

        lv_obj_t *panel = ui_pixel_panel_create(wrap, 0, 0, 218, 34, UI_PAPER);
        lv_obj_set_style_pad_all(panel, 0, 0);
        s_row_panel[i] = panel;

        lv_obj_t *box = lv_obj_create(panel);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(box, 20, 20);
        lv_obj_align(box, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_radius(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_color(box, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_border_width(box, 3, 0);
        s_row_box[i] = box;

        s_row_check[i] = ui_pixel_label(box, LV_SYMBOL_OK, &lv_font_montserrat_14, 0xFFFFFF);
        lv_obj_center(s_row_check[i]);

        s_row_text[i] = ui_pixel_label(panel, "", &lv_font_montserrat_14, UI_INK);
        lv_label_set_long_mode(s_row_text[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_row_text[i], 164);
        lv_obj_align(s_row_text[i], LV_ALIGN_LEFT_MID, 36, 0);
    }

    s_empty = ui_pixel_label(s_scr, "ALL CLEAR!", &lv_font_montserrat_20, 0xFFFFFF);
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, 150);

    // 草地上的操作提示 + 吉祥物
    lv_obj_t *hint = ui_pixel_label(s_scr,
        "OK done  " LV_SYMBOL_UP " add  " LV_SYMBOL_DOWN " del",
        &lv_font_montserrat_14, UI_INK);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    s_mascot = ui_pixel_mascot_create(s_scr, 101, 242);

    refresh();
    tick(NULL);                                    // 立即显示电量,不等 1 秒
    s_timer = lv_timer_create(tick, 1000, NULL);
    lv_screen_load(s_scr);
}

void demo_todo_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    // 退出前把最后的修改落盘。小 blob(≈0.25 KB)提交只有毫秒级,可接受。
    if (s_dirty) todo_save();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = s_mascot = s_batt = NULL;
        s_stat_label = s_bar_fill = s_empty = NULL;
        s_arrow_up = s_arrow_down = NULL;
        memset(s_row_wrap, 0, sizeof(s_row_wrap));
        memset(s_row_panel, 0, sizeof(s_row_panel));
        memset(s_row_box, 0, sizeof(s_row_box));
        memset(s_row_check, 0, sizeof(s_row_check));
        memset(s_row_text, 0, sizeof(s_row_text));
    }
}

// ---- 按键 ----

void demo_todo_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP && s_data.count > 0) {
            s_sel = (s_sel + s_data.count - 1) % s_data.count;
            clamp_view();
            refresh();
        } else if (btn == BSP_BTN_DOWN && s_data.count > 0) {
            s_sel = (s_sel + 1) % s_data.count;
            clamp_view();
            refresh();
        } else if (btn == BSP_BTN_OK && s_data.count > 0) {
            s_data.items[s_sel].done = !s_data.items[s_sel].done;
            s_dirty = true;
            ui_pixel_mascot_jump(s_mascot);
            refresh();
        }
    } else if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_UP && s_data.count < TODO_MAX) {       // 新增
            todo_item_t *it = &s_data.items[s_data.count];
            it->done = 0;
            strlcpy(it->text, TEMPLATES[s_tpl], TODO_TEXT_LEN);
            s_tpl = (s_tpl + 1) % TEMPLATE_COUNT;
            s_sel = s_data.count;
            s_data.count++;
            s_dirty = true;
            clamp_view();
            ui_pixel_mascot_jump(s_mascot);
            refresh();
        } else if (btn == BSP_BTN_DOWN && s_data.count > 0) {     // 删除
            for (int i = s_sel; i < s_data.count - 1; i++) {
                s_data.items[i] = s_data.items[i + 1];
            }
            s_data.count--;
            memset(&s_data.items[s_data.count], 0, sizeof(todo_item_t));
            s_dirty = true;
            clamp_view();
            refresh();
        }
    }
}
