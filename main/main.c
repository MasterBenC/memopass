#include "app_config.h"
#include "app_net.h"
#include "audio_pipeline.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "feishu_formatter.h"
#include "feishu_pusher.h"
#include "key_guard.h"
#include "meeting_fsm.h"
#include "memopass_ui.h"
#include "mp_utf8.h"
#include "portal_server.h"
#include "volc_asr_client.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "memopass";
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static key_guard_t s_key_guard;
static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static meeting_fsm_t s_fsm;
static app_config_t s_cfg;
static volatile bool s_paused;
static volatile bool s_muted;
static volatile bool s_session_run;
static volatile bool s_holding;
static volatile int64_t s_hold_start;
static volatile bool s_hold_fired;
static char s_detail[96];
static int s_segments;
static uint32_t s_elapsed_ms;
static char s_meeting_id[12];
static uint8_t s_pcm[3][AUDIO_PCM_BYTES];
static QueueHandle_t s_pcm_used;
static QueueHandle_t s_pcm_free;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = {.btn = btn, .event = ev};
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void set_detail(mp_err_t err) {
    snprintf(s_detail, sizeof(s_detail), "%s", meeting_fsm_error_text(err));
}

static void refresh_ui(void) {
    if (!bsp_lvgl_lock(30)) return;
    char timer[16];
    char last[64];
    app_hist_t hist[APP_HIST_MAX];
    int count = 0;
    int battery = bsp_battery_soc();
    meeting_fsm_format_timer(timer, sizeof(timer), s_elapsed_ms);
    switch (s_fsm.screen) {
    case MP_SCR_PROVISION: {
        char qr[64];
        snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", portal_ap_ssid());
        memopass_ui_provision(qr, portal_ap_ssid(), s_fsm.configured);
        break;
    }
    case MP_SCR_HOME:
        app_config_hist_load(hist, &count);
        if (count > 0) {
            struct tm tm;
            time_t unix_time = hist[count - 1].unix_time;
            bool pushed = hist[count - 1].pushed != 0;
            if (unix_time > 1700000000 && localtime_r(&unix_time, &tm)) {
                meeting_fsm_format_last(last, sizeof(last), tm.tm_hour, tm.tm_min,
                                        hist[count - 1].duration_sec, pushed);
            } else {
                meeting_fsm_format_last(last, sizeof(last), -1, -1,
                                        hist[count - 1].duration_sec, pushed);
            }
        } else {
            meeting_fsm_format_last(last, sizeof(last), -1, -1, 0, false);
        }
        memopass_ui_home(last, app_net_sta_up(), feishu_url_ok(s_cfg.webhook), battery);
        break;
    case MP_SCR_RECORD: {
        char sub[192];
        size_t n = 0;
        uint16_t hold = 0;
        volc_asr_copy(sub, sizeof(sub), &n);
        if (s_holding) {
            int64_t ms = (esp_timer_get_time() - s_hold_start) / 1000;
            if (ms < 0) ms = 0;
            if (ms > MP_HOLD_END_MS) ms = MP_HOLD_END_MS;
            hold = (uint16_t)ms;
        }
        memopass_ui_record(&s_fsm, timer, sub, audio_pipeline_level(), s_segments,
                           !app_net_sta_up() || !volc_asr_up(), hold);
        break;
    }
    case MP_SCR_RESULT:
        memopass_ui_result(s_fsm.result_ok, s_detail, (int)(s_elapsed_ms / 1000),
                           s_segments, s_fsm.detail_on);
        break;
    case MP_SCR_SETTINGS:
        memopass_ui_settings(s_fsm.settings_i, s_fsm.beep, s_fsm.factory_armed);
        break;
    case MP_SCR_HISTORY:
        app_config_hist_load(hist, &count);
        memopass_ui_history(hist, count);
        break;
    }
    bsp_lvgl_unlock();
}

static void submit_text(const char *text, size_t len, bool final) {
    feishu_seg_t seg;
    size_t cut = mp_utf8_fit(text, len, FEISHU_PUSH_TEXT);
    if (!cut && !final) return;
    memset(&seg, 0, sizeof(seg));
    snprintf(seg.meeting_id, sizeof(seg.meeting_id), "%s", s_meeting_id);
    seg.segment = ++s_segments;
    seg.is_final = final;
    seg.duration_sec = (int)(s_elapsed_ms / 1000);
    seg.text_len = cut;
    if (cut) memcpy(seg.text, text, cut);
    if (!feishu_pusher_submit(&seg) && final) set_detail(MP_ERR_FEISHU);
}

static void session_task(void *arg) {
    (void)arg;
    char snap[2200];
    uint32_t seg_ms = 0;
    bool mark = false;
    while (s_session_run) {
        size_t n = 0;
        int fail = volc_asr_fail();
        if (fail == 401) set_detail(MP_ERR_AUTH);
        else if (fail == 45000030) set_detail(MP_ERR_RESOURCE);
        else if (fail == 8) set_detail(MP_ERR_NET);
        if (volc_asr_copy(snap, sizeof(snap), &n) && n) {
            if (mark) {
                submit_text("[重点]", strlen("[重点]"), false);
                mark = false;
                seg_ms = 0;
            } else if (meeting_fsm_segment_due(n, seg_ms, s_cfg.segment_min)) {
                size_t cut = mp_utf8_fit(snap, n, FEISHU_PUSH_TEXT);
                submit_text(snap, cut, false);
                volc_asr_drop(cut);
                seg_ms = 0;
            }
        }
        if (s_fsm.mark_on) mark = true;
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_paused) {
            s_elapsed_ms += 200;
            seg_ms += 200;
        }
    }
    {
        size_t n = 0;
        if (volc_asr_copy(snap, sizeof(snap), &n)) submit_text(snap, n, true);
    }
    audio_pipeline_stop();
    volc_asr_close();
    for (int i = 0; i < 40 && !feishu_pusher_idle() && feishu_pusher_failed() == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    feishu_pusher_stop();
    {
        app_hist_t item = {0};
        item.unix_time = app_net_clock();
        item.duration_sec = (uint16_t)(s_elapsed_ms / 1000);
        item.segments = (uint8_t)(s_segments > 255 ? 255 : s_segments);
        item.pushed = feishu_pusher_failed() == 0 && s_segments > 0;
        snprintf(item.id, sizeof(item.id), "%s", s_meeting_id);
        app_config_hist_push(&item);
        meeting_fsm_push_done(&s_fsm, item.pushed);
        if (!item.pushed && !s_detail[0]) set_detail(MP_ERR_FEISHU);
    }
    vTaskDelete(NULL);
}

static void start_session(void) {
    uint32_t rnd = esp_random();
    snprintf(s_meeting_id, sizeof(s_meeting_id), "%08lx", (unsigned long)rnd);
    s_segments = 0;
    s_elapsed_ms = 0;
    s_paused = false;
    s_muted = false;
    s_detail[0] = '\0';
    if (!s_pcm_used) s_pcm_used = xQueueCreate(3, sizeof(uint8_t *));
    if (!s_pcm_free) s_pcm_free = xQueueCreate(3, sizeof(uint8_t *));
    xQueueReset(s_pcm_used);
    xQueueReset(s_pcm_free);
    for (int i = 0; i < 3; i++) {
        uint8_t *slot = s_pcm[i];
        xQueueSend(s_pcm_free, &slot, 0);
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    feishu_pusher_start(&s_cfg);
    volc_asr_open(&s_cfg, s_pcm_used, s_pcm_free);
    audio_pipeline_start(s_pcm_used, s_pcm_free, &s_paused, &s_muted);
    s_session_run = true;
    xTaskCreate(session_task, "mp_session", 6144, NULL, 5, NULL);
}

static void apply_action(mp_action_t action) {
    if (action == MP_ACT_START) start_session();
    else if (action == MP_ACT_PAUSE) s_paused = true;
    else if (action == MP_ACT_RESUME) s_paused = false;
    else if (action == MP_ACT_MUTE) s_muted = true;
    else if (action == MP_ACT_UNMUTE) s_muted = false;
    else if (action == MP_ACT_STOP) s_session_run = false;
    else if (action == MP_ACT_PROVISION) {
        app_net_stop();
        portal_start(NULL);
    } else if (action == MP_ACT_LEAVE_PROVISION) {
        portal_stop();
        app_net_start_sta(s_cfg.ssid, s_cfg.pass);
    } else if (action == MP_ACT_TEST_PUSH) {
        if (feishu_pusher_once(&s_cfg, "会议纪要 MemoPass 测试") == ESP_OK) {
            snprintf(s_detail, sizeof(s_detail), "测试已发送");
        } else {
            set_detail(MP_ERR_FEISHU);
        }
        refresh_ui();
    } else if (action == MP_ACT_FACTORY) {
        app_config_clear();
        esp_restart();
    } else if (action == MP_ACT_RETRY_PUSH) {
        s_fsm.result_ok = feishu_pusher_once(&s_cfg, s_detail[0] ? s_detail : "会议纪要") == ESP_OK;
    }
    if (action == MP_ACT_REDRAW || action == MP_ACT_START || action == MP_ACT_STOP ||
        action == MP_ACT_PROVISION || action == MP_ACT_LEAVE_PROVISION) {
        refresh_ui();
    }
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(100)) == pdTRUE) {
            mp_input_t which = 0;
            if (input.btn == BSP_BTN_OK && input.event == BSP_BTN_PRESS) {
                s_holding = true;
                s_hold_fired = false;
                s_hold_start = esp_timer_get_time();
            } else if (input.btn == BSP_BTN_OK && input.event == BSP_BTN_RELEASE) {
                uint32_t held = (uint32_t)((esp_timer_get_time() - s_hold_start) / 1000);
                s_holding = false;
                if (!s_hold_fired) which = meeting_fsm_hold_fires(held) ? MP_IN_OK_HOLD : MP_IN_OK;
            } else if (input.event == BSP_BTN_CLICK && input.btn == BSP_BTN_UP) {
                which = MP_IN_UP;
            } else if (input.event == BSP_BTN_CLICK && input.btn == BSP_BTN_DOWN) {
                which = MP_IN_DOWN;
            }
            if (which) apply_action(meeting_fsm_input(&s_fsm, which));
        }
        if (s_holding && !s_hold_fired) {
            uint32_t held = (uint32_t)((esp_timer_get_time() - s_hold_start) / 1000);
            if (meeting_fsm_hold_fires(held) && s_fsm.screen == MP_SCR_RECORD) {
                s_hold_fired = true;
                s_holding = false;
                apply_action(meeting_fsm_input(&s_fsm, MP_IN_OK_HOLD));
            }
        }
        if (s_fsm.screen == MP_SCR_RECORD || s_fsm.screen == MP_SCR_RESULT) {
            apply_action(meeting_fsm_input(&s_fsm, MP_IN_TICK_100MS));
        }
        if (s_fsm.screen == MP_SCR_RECORD) refresh_ui();
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "MemoPass 启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }
    app_config_load(&s_cfg);
    s_fsm.beep = s_cfg.beep;
    meeting_fsm_init(&s_fsm, app_config_ready(&s_cfg));
    s_fsm.beep = s_cfg.beep;
    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败 MOSI=%d SCLK=%d CS=%d DC=%d BL=%d",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(70);
    bsp_battery_init();
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    xTaskCreate(input_task, "mp_input", 6144, NULL, 5, NULL);
    if (bsp_button_init(on_key, NULL) == ESP_OK && s_key_guard.armed) {
        key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
    }
    if (bsp_lvgl_lock(1000)) {
        memopass_ui_init();
        bsp_lvgl_unlock();
    }
    if (s_fsm.screen == MP_SCR_PROVISION) portal_start(NULL);
    else app_net_start_sta(s_cfg.ssid, s_cfg.pass);
    refresh_ui();
    s_input_ready = true;
}
