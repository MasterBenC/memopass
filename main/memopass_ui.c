#include "memopass_ui.h"

#include "bsp_display.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(mp_digits_28);
LV_FONT_DECLARE(mp_ui_18);
LV_FONT_DECLARE(mp_sub_14);
LV_FONT_DECLARE(mp_hint_12);

#define CANVAS 0x0B0F14
#define SURFACE 0x161D26
#define BORDER 0x263241
#define CORAL 0xF04438
#define BLUE 0x3370FF
#define MINT 0x12B76A
#define AMBER 0xF79009
#define TEXT 0xF8FAFC
#define SECOND 0x94A3B8
#define MUTED 0x64748B
#define WAVE_DIM 0x475569

static lv_obj_t *s_prov;
static lv_obj_t *s_home;
static lv_obj_t *s_rec;
static lv_obj_t *s_result;
static lv_obj_t *s_settings;
static lv_obj_t *s_hist;
static lv_obj_t *s_qr;
static lv_obj_t *s_prov_ssid;
static lv_obj_t *s_prov_ip;
static lv_obj_t *s_prov_guide;
static lv_obj_t *s_home_last;
static lv_obj_t *s_home_status;
static lv_obj_t *s_timer;
static lv_obj_t *s_sub;
static lv_obj_t *s_mark;
static lv_obj_t *s_rec_left;
static lv_obj_t *s_rec_right;
static lv_obj_t *s_rec_guide;
static lv_obj_t *s_bars[9];
static lv_obj_t *s_hold;
static lv_obj_t *s_dot;
static lv_obj_t *s_result_title;
static lv_obj_t *s_result_sub;
static lv_obj_t *s_result_hint;
static lv_obj_t *s_result_guide;
static lv_obj_t *s_set_cards[MP_SETTINGS_COUNT];
static lv_obj_t *s_set_labels[MP_SETTINGS_COUNT];
static lv_obj_t *s_hist_body;
static bool s_dot_on;

static lv_obj_t *screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_style_bg_color(scr, lv_color_hex(CANVAS), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return scr;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *guide(lv_obj_t *parent, const char *text) {
    lv_obj_t *bar = box(parent, 12, 284, 216, 36, SURFACE, 12);
    lv_obj_t *lab = label(bar, text, &mp_hint_12, SECOND);
    lv_obj_center(lab);
    return lab;
}

static void corner(lv_obj_t *parent, int x, int y, int dx, int dy) {
    box(parent, x, y, 16, 2, BLUE, 0);
    box(parent, x + (dx < 0 ? 14 : 0), y, 2, 16, BLUE, 0);
    (void)dy;
}

static void build_provision(void) {
    s_prov = screen();
    label(s_prov, "扫码配网", &mp_ui_18, TEXT);
    lv_obj_align(lv_obj_get_child(s_prov, 0), LV_ALIGN_TOP_MID, 0, 36);
    s_qr = lv_qrcode_create(s_prov);
    lv_qrcode_set_size(s_qr, 144);
    lv_qrcode_set_dark_color(s_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_qr, lv_color_hex(0xFFFFFF));
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, 72);
    corner(s_prov, 40, 64, 1, 1);
    corner(s_prov, 184, 64, -1, 1);
    corner(s_prov, 40, 216, 1, -1);
    corner(s_prov, 184, 216, -1, -1);
    s_prov_ssid = label(s_prov, "MemoPass", &mp_hint_12, SECOND);
    lv_obj_align(s_prov_ssid, LV_ALIGN_TOP_MID, 0, 236);
    s_prov_ip = label(s_prov, "192.168.4.1", &mp_hint_12, MUTED);
    lv_obj_align(s_prov_ip, LV_ALIGN_TOP_MID, 0, 254);
    s_prov_guide = guide(s_prov, "");
}

static void build_home(void) {
    s_home = screen();
    label(s_home, "MemoPass", &mp_hint_12, TEXT);
    lv_obj_align(lv_obj_get_child(s_home, 0), LV_ALIGN_TOP_LEFT, 12, 6);
    s_home_status = label(s_home, "", &mp_hint_12, SECOND);
    lv_obj_align(s_home_status, LV_ALIGN_TOP_RIGHT, -12, 6);
    lv_obj_t *card = box(s_home, 12, 40, 216, 164, SURFACE, 12);
    lv_obj_t *ring = box(card, 92, 28, 32, 32, CANVAS, 16);
    lv_obj_set_style_border_color(ring, lv_color_hex(CORAL), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    box(ring, 8, 8, 16, 16, CORAL, 8);
    lv_obj_t *cta = label(card, "按确认开始记录", &mp_ui_18, TEXT);
    lv_obj_align(cta, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_t *sub = label(card, "实时转写 · 自动同步飞书", &mp_hint_12, SECOND);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_t *last = box(s_home, 12, 214, 216, 44, SURFACE, 10);
    box(last, 12, 16, 8, 8, MINT, 4);
    s_home_last = label(last, "上次 · 0分钟 · 未推送", &mp_hint_12, SECOND);
    lv_obj_align(s_home_last, LV_ALIGN_LEFT_MID, 28, 0);
    guide(s_home, "上:设置 | 确认:录音 | 下:记录");
}

static void build_record(void) {
    s_rec = screen();
    s_dot = box(s_rec, 12, 10, 8, 8, CORAL, 4);
    s_rec_left = label(s_rec, "录音中", &mp_hint_12, TEXT);
    lv_obj_align(s_rec_left, LV_ALIGN_TOP_LEFT, 26, 6);
    s_rec_right = label(s_rec, "已推 0 段", &mp_hint_12, SECOND);
    lv_obj_align(s_rec_right, LV_ALIGN_TOP_RIGHT, -12, 6);
    s_timer = label(s_rec, "00:00:00", &mp_digits_28, TEXT);
    lv_obj_align(s_timer, LV_ALIGN_TOP_MID, 0, 36);
    for (int i = 0; i < 9; i++) {
        s_bars[i] = box(s_rec, 48 + i * 16, 92, 8, 4, WAVE_DIM, 2);
    }
    lv_obj_t *card = box(s_rec, 12, 112, 216, 152, SURFACE, 10);
    s_mark = label(card, "已标记重点", &mp_hint_12, AMBER);
    lv_obj_align(s_mark, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_hidden(s_mark, true);
    s_sub = label(card, "", &mp_sub_14, TEXT);
    lv_obj_set_width(s_sub, 196);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_sub, LV_ALIGN_TOP_LEFT, 10, 28);
    s_hold = box(s_rec, 12, 272, 0, 4, CORAL, 2);
    s_rec_guide = guide(s_rec, "上:标重点 | 确认:暂停 | 下:静音");
}

static void build_result(void) {
    s_result = screen();
    lv_obj_t *mark = box(s_result, 92, 88, 56, 56, MINT, 28);
    label(mark, "OK", &mp_ui_18, CANVAS);
    lv_obj_center(lv_obj_get_child(mark, 0));
    s_result_title = label(s_result, "已同步至飞书", &mp_ui_18, TEXT);
    lv_obj_align(s_result_title, LV_ALIGN_TOP_MID, 0, 160);
    s_result_sub = label(s_result, "", &mp_hint_12, SECOND);
    lv_obj_align(s_result_sub, LV_ALIGN_TOP_MID, 0, 190);
    s_result_hint = label(s_result, "3秒后自动返回", &mp_hint_12, MUTED);
    lv_obj_set_width(s_result_hint, 200);
    lv_label_set_long_mode(s_result_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_result_hint, LV_ALIGN_TOP_MID, 0, 214);
    s_result_guide = guide(s_result, "确认:回主页");
}

static void build_settings(void) {
    static const char *names[] = {
        "重新扫码配网", "测试飞书推送", "提示音：关闭", "恢复出厂设置",
    };
    s_settings = screen();
    lv_obj_t *title = label(s_settings, "设置", &mp_ui_18, TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    for (int i = 0; i < MP_SETTINGS_COUNT; i++) {
        s_set_cards[i] = box(s_settings, 12, 40 + i * 52, 216, 44, SURFACE, 10);
        s_set_labels[i] = label(s_set_cards[i], names[i], &mp_ui_18, TEXT);
        lv_obj_align(s_set_labels[i], LV_ALIGN_LEFT_MID, 10, 0);
    }
    guide(s_settings, "上:上移 | 确认:执行 | 下:下移");
}

static void build_history(void) {
    s_hist = screen();
    lv_obj_t *title = label(s_hist, "记录", &mp_ui_18, TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    s_hist_body = label(s_hist, "没有记录", &mp_hint_12, SECOND);
    lv_obj_set_width(s_hist_body, 216);
    lv_label_set_long_mode(s_hist_body, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_hist_body, LV_ALIGN_TOP_LEFT, 12, 40);
    guide(s_hist, "确认:返回");
}

void memopass_ui_init(void) {
    build_provision();
    build_home();
    build_record();
    build_result();
    build_settings();
    build_history();
    lv_screen_load(s_home);
}

void memopass_ui_provision(const char *qr, const char *ssid, bool can_back) {
    if (qr) lv_qrcode_update(s_qr, qr, (uint32_t)strlen(qr));
    if (ssid) lv_label_set_text(s_prov_ssid, ssid);
    lv_label_set_text(s_prov_guide, can_back ? "确认:返回" : "");
    if (lv_screen_active() != s_prov) lv_screen_load(s_prov);
}

void memopass_ui_home(const char *last_line, bool wifi, bool feishu, int battery) {
    char right[32];
    if (battery >= 0) snprintf(right, sizeof(right), "%s%s %d%%", wifi ? "W " : "", feishu ? "F" : "", battery);
    else snprintf(right, sizeof(right), "%s%s", wifi ? "W" : "", feishu ? " F" : "");
    lv_label_set_text(s_home_status, right);
    if (last_line) lv_label_set_text(s_home_last, last_line);
    if (lv_screen_active() != s_home) lv_screen_load(s_home);
}

void memopass_ui_record(const meeting_fsm_t *st, const char *timer, const char *subtitle,
                        uint8_t level, int pushed, bool weak, uint16_t hold_ms) {
    char right[24];
    s_dot_on = !s_dot_on;
    lv_obj_set_style_bg_opa(s_dot, s_dot_on ? LV_OPA_COVER : LV_OPA_40, 0);
    if (weak || (st && st->paused)) lv_obj_set_style_bg_color(s_dot, lv_color_hex(AMBER), 0);
    else lv_obj_set_style_bg_color(s_dot, lv_color_hex(CORAL), 0);
    lv_label_set_text(s_rec_left, weak ? "弱网" : (st && st->paused) ? "已暂停" : "录音中");
    snprintf(right, sizeof(right), "已推 %d 段", pushed);
    lv_label_set_text(s_rec_right, right);
    if (timer) lv_label_set_text(s_timer, timer);
    if (subtitle) lv_label_set_text(s_sub, subtitle);
    if (st && st->mark_on) lv_obj_set_hidden(s_mark, false);
    else lv_obj_set_hidden(s_mark, true);
    lv_label_set_text(s_rec_guide, meeting_fsm_key_guide(st));
    for (int i = 0; i < 9; i++) {
        int dist = i < 4 ? 4 - i : i - 4;
        int h = 4;
        if (!st || !st->muted) {
            int peak = (int)level / 16;
            h = 4 + (peak > dist ? (peak - dist) * 3 : 0);
            if (h > 28) h = 28;
        }
        lv_obj_set_height(s_bars[i], h);
        lv_obj_set_y(s_bars[i], 96 - h);
        uint32_t color = (dist <= 2 && level > 40) ? CORAL : WAVE_DIM;
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(color), 0);
    }
    lv_obj_set_width(s_hold, hold_ms >= MP_HOLD_END_MS ? 216 : (int)(hold_ms * 216 / MP_HOLD_END_MS));
    if (lv_screen_active() != s_rec) lv_screen_load(s_rec);
}

void memopass_ui_result(bool ok, const char *detail, int duration_sec, int segments, bool detail_on) {
    char line[48];
    int mm = duration_sec / 60;
    int ss = duration_sec % 60;
    lv_label_set_text(s_result_title, ok ? "已同步至飞书" : "同步失败");
    snprintf(line, sizeof(line), "时长 %02d:%02d · 共 %d 段", mm, ss, segments);
    lv_label_set_text(s_result_sub, line);
    if (!ok && detail_on && detail) lv_label_set_text(s_result_hint, detail);
    else if (ok) lv_label_set_text(s_result_hint, "3秒后自动返回");
    else lv_label_set_text(s_result_hint, "确认:重试");
    lv_label_set_text(s_result_guide, ok ? "确认:回主页" : "上:放弃 | 确认:重试 | 下:详情");
    if (lv_screen_active() != s_result) lv_screen_load(s_result);
}

void memopass_ui_settings(int index, bool beep, bool factory_armed) {
    lv_label_set_text(s_set_labels[2], beep ? "提示音：开启" : "提示音：关闭");
    lv_label_set_text(s_set_labels[3], factory_armed ? "再按确认恢复" : "恢复出厂设置");
    for (int i = 0; i < MP_SETTINGS_COUNT; i++) {
        lv_obj_set_style_border_width(s_set_cards[i], i == index ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_set_cards[i], lv_color_hex(BLUE), 0);
    }
    if (lv_screen_active() != s_settings) lv_screen_load(s_settings);
}

void memopass_ui_history(const app_hist_t *items, int count) {
    static char body[320];
    size_t used = 0;
    body[0] = '\0';
    if (count <= 0) {
        lv_label_set_text(s_hist_body, "没有记录");
    } else {
        for (int i = count - 1; i >= 0 && used + 40 < sizeof(body); i--) {
            int n = snprintf(body + used, sizeof(body) - used, "%s  %u分 %s\n",
                             items[i].id, (unsigned)((items[i].duration_sec + 59) / 60),
                             items[i].pushed ? "已推飞书" : "未推送");
            if (n > 0) used += (size_t)n;
        }
        lv_label_set_text(s_hist_body, body);
    }
    if (lv_screen_active() != s_hist) lv_screen_load(s_hist);
}
