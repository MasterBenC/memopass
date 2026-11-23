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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
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
static TaskHandle_t s_session_task;
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
/* Guards state shared by input_task, session_task and refresh_ui: s_fsm,
 * s_cfg, s_paused, s_muted, s_detail, s_segments, s_elapsed_ms, s_meeting_id
 * and the hold timer. Recursive so apply_action helpers may nest. Never hold
 * it across blocking work (network, LVGL lock, task waits). */
static SemaphoreHandle_t s_state_mu;
/* Set when a redraw could not take the LVGL lock; input_task redraws later. */
static volatile bool s_ui_dirty;

static void state_lock(void) {
    if (s_state_mu) xSemaphoreTakeRecursive(s_state_mu, portMAX_DELAY);
}

static void state_unlock(void) {
    if (s_state_mu) xSemaphoreGiveRecursive(s_state_mu);
}

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
    state_lock();
    snprintf(s_detail, sizeof(s_detail), "%s", meeting_fsm_error_text(err));
    state_unlock();
}

static void set_detail_text(const char *text) {
    state_lock();
    snprintf(s_detail, sizeof(s_detail), "%s", text);
    state_unlock();
}

static void refresh_ui(void) {
    if (!bsp_lvgl_lock(30)) {
        /* Do not drop a state change silently: input_task redraws next loop. */
        s_ui_dirty = true;
        return;
    }
    s_ui_dirty = false;
    meeting_fsm_t fsm;
    char detail[sizeof(s_detail)];
    char webhook[sizeof(s_cfg.webhook)];
    uint32_t elapsed_ms;
    int segments;
    bool holding;
    int64_t hold_start;
    state_lock();
    fsm = s_fsm;
    memcpy(detail, s_detail, sizeof(detail));
    memcpy(webhook, s_cfg.webhook, sizeof(webhook));
    elapsed_ms = s_elapsed_ms;
    segments = s_segments;
    holding = s_holding;
    hold_start = s_hold_start;
    state_unlock();
    char timer[16];
    char last[64];
    app_hist_t hist[APP_HIST_MAX];
    int count = 0;
    int battery = bsp_battery_soc();
    meeting_fsm_format_timer(timer, sizeof(timer), elapsed_ms);
    switch (fsm.screen) {
    case MP_SCR_PROVISION: {
        char qr[64];
        snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", portal_ap_ssid());
        memopass_ui_provision(qr, portal_ap_ssid(), fsm.configured);
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
        memopass_ui_home(last, app_net_sta_up(), feishu_url_ok(webhook), battery);
        break;
    case MP_SCR_RECORD: {
        char sub[192];
        size_t n = 0;
        uint16_t hold = 0;
        volc_asr_copy(sub, sizeof(sub), &n);
        if (holding) {
            int64_t ms = (esp_timer_get_time() - hold_start) / 1000;
            if (ms < 0) ms = 0;
            if (ms > MP_HOLD_END_MS) ms = MP_HOLD_END_MS;
            hold = (uint16_t)ms;
        }
        memopass_ui_record(&fsm, timer, sub, audio_pipeline_level(), segments,
                           !app_net_sta_up() || !volc_asr_up(), hold);
        break;
    }
    case MP_SCR_RESULT:
        memopass_ui_result(fsm.result_ok, detail, (int)(elapsed_ms / 1000),
                           segments, fsm.detail_on, fsm.result_pending);
        break;
    case MP_SCR_SETTINGS:
        memopass_ui_settings(fsm.settings_i, fsm.beep, fsm.factory_armed);
        break;
    case MP_SCR_HISTORY:
        app_config_hist_load(hist, &count);
        memopass_ui_history(hist, count);
        break;
    }
    bsp_lvgl_unlock();
}

static void submit_text(const char *text, size_t len, bool final) {
    feishu_seg_t *seg;
    size_t cut = mp_utf8_fit(text, len, FEISHU_PUSH_TEXT);
    if (!cut && !final) return;
    seg = (feishu_seg_t *)calloc(1, sizeof(*seg));
    if (!seg) {
        ESP_LOGE(TAG, "submit_text alloc failed");
        if (final) set_detail(MP_ERR_FEISHU);
        return;
    }
    state_lock();
    snprintf(seg->meeting_id, sizeof(seg->meeting_id), "%s", s_meeting_id);
    seg->segment = ++s_segments;
    seg->duration_sec = (int)(s_elapsed_ms / 1000);
    state_unlock();
    seg->is_final = final;
    seg->text_len = cut;
    if (cut) memcpy(seg->text, text, cut);
    if (!feishu_pusher_submit(seg) && final) set_detail(MP_ERR_FEISHU);
    free(seg);
}

static void session_task(void *arg) {
    (void)arg;
    const size_t snap_cap = 2200;
    char *snap = (char *)malloc(snap_cap);
    uint32_t seg_ms = 0;
    bool mark = false;
    int segment_min;
    if (!snap) {
        ESP_LOGE(TAG, "session snap alloc failed");
        set_detail_text("内存不足，请再试一次");
        s_session_run = false;
    }
    state_lock();
    segment_min = s_cfg.segment_min;
    state_unlock();
    while (s_session_run) {
        size_t n = 0;
        int fail = volc_asr_fail();
        if (fail == 401) set_detail(MP_ERR_AUTH);
        else if (fail == 45000030) set_detail(MP_ERR_RESOURCE);
        else if (fail == 8) set_detail(MP_ERR_NET);
        if (snap && volc_asr_copy(snap, snap_cap, &n) && n) {
            if (mark) {
                submit_text("[重点]", strlen("[重点]"), false);
                mark = false;
                seg_ms = 0;
            } else if (meeting_fsm_segment_due(n, seg_ms, segment_min)) {
                size_t cut = mp_utf8_fit(snap, n, FEISHU_PUSH_TEXT);
                submit_text(snap, cut, false);
                volc_asr_drop(cut);
                seg_ms = 0;
            }
        }
        state_lock();
        if (s_fsm.mark_on) mark = true;
        state_unlock();
        vTaskDelay(pdMS_TO_TICKS(200));
        state_lock();
        if (!s_paused) {
            s_elapsed_ms += 200;
            seg_ms += 200;
        }
        state_unlock();
    }
    if (snap) {
        size_t n = 0;
        if (volc_asr_copy(snap, snap_cap, &n)) submit_text(snap, n, true);
        free(snap);
        snap = NULL;
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
        state_lock();
        item.duration_sec = (uint16_t)(s_elapsed_ms / 1000);
        item.segments = (uint8_t)(s_segments > 255 ? 255 : s_segments);
        item.pushed = feishu_pusher_failed() == 0 && s_segments > 0;
        snprintf(item.id, sizeof(item.id), "%s", s_meeting_id);
        state_unlock();
        app_config_hist_push(&item);
        state_lock();
        meeting_fsm_push_done(&s_fsm, item.pushed);
        if (!item.pushed && !s_detail[0]) {
            set_detail(s_segments == 0 ? MP_ERR_EMPTY : MP_ERR_FEISHU);
        }
        state_unlock();
        /* The result screen must show the final push state. */
        s_ui_dirty = true;
    }
    s_session_task = NULL;
    vTaskDelete(NULL);
}

static void start_session(void) {
    uint32_t rnd;
    for (int i = 0; i < 2400 && s_session_task; i++) vTaskDelay(pdMS_TO_TICKS(25));
    if (s_session_task) {
        ESP_LOGW(TAG, "previous session still running, not starting another");
        state_lock();
        meeting_fsm_return_home(&s_fsm);
        state_unlock();
        s_ui_dirty = true;
        refresh_ui();
        return;
    }
    rnd = esp_random();
    state_lock();
    snprintf(s_meeting_id, sizeof(s_meeting_id), "%08lx", (unsigned long)rnd);
    s_segments = 0;
    s_elapsed_ms = 0;
    s_paused = false;
    s_muted = false;
    s_detail[0] = '\0';
    state_unlock();
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
    if (xTaskCreate(session_task, "mp_session", 8192, NULL, 5, &s_session_task) != pdPASS) {
        s_session_task = NULL;
        s_session_run = false;
        ESP_LOGE(TAG, "session task create failed");
        set_detail_text("内存不足，请再试一次");
        audio_pipeline_stop();
        volc_asr_close();
        feishu_pusher_stop();
        state_lock();
        meeting_fsm_push_done(&s_fsm, false);
        state_unlock();
        s_ui_dirty = true;
        refresh_ui();
    }
}

static void set_flag(volatile bool *flag, bool value) {
    state_lock();
    *flag = value;
    state_unlock();
}

static void apply_action(mp_action_t action) {
    if (action == MP_ACT_START) start_session();
    else if (action == MP_ACT_PAUSE) set_flag(&s_paused, true);
    else if (action == MP_ACT_RESUME) set_flag(&s_paused, false);
    else if (action == MP_ACT_MUTE) set_flag(&s_muted, true);
    else if (action == MP_ACT_UNMUTE) set_flag(&s_muted, false);
    else if (action == MP_ACT_STOP) s_session_run = false;
    else if (action == MP_ACT_PROVISION) {
        app_net_stop();
        portal_start(NULL);
    } else if (action == MP_ACT_LEAVE_PROVISION) {
        portal_stop();
        app_net_start_sta(s_cfg.ssid, s_cfg.pass);
    } else if (action == MP_ACT_TEST_PUSH) {
        if (feishu_pusher_once(&s_cfg, "会议纪要 MemoPass 测试") == ESP_OK) {
            set_detail_text("测试已发送");
        } else {
            set_detail(MP_ERR_FEISHU);
        }
        refresh_ui();
    } else if (action == MP_ACT_FACTORY) {
        app_config_clear();
        esp_restart();
    } else if (action == MP_ACT_RETRY_PUSH) {
        char detail[sizeof(s_detail)];
        bool ok;
        state_lock();
        snprintf(detail, sizeof(detail), "%s", s_detail[0] ? s_detail : "会议纪要");
        state_unlock();
        ok = feishu_pusher_once(&s_cfg, detail) == ESP_OK;
        state_lock();
        s_fsm.result_ok = ok;
        s_fsm.result_pending = false;
        state_unlock();
        s_ui_dirty = true;
    }
    if (action == MP_ACT_REDRAW || action == MP_ACT_START || action == MP_ACT_STOP ||
        action == MP_ACT_PROVISION || action == MP_ACT_LEAVE_PROVISION ||
        action == MP_ACT_RETRY_PUSH) {
        refresh_ui();
    }
}

static mp_action_t fsm_input(mp_input_t which) {
    state_lock();
    mp_action_t action = meeting_fsm_input(&s_fsm, which);
    state_unlock();
    return action;
}

static mp_screen_t fsm_screen(void) {
    state_lock();
    mp_screen_t screen = s_fsm.screen;
    state_unlock();
    return screen;
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(100)) == pdTRUE) {
            mp_input_t which = 0;
            if (input.btn == BSP_BTN_OK && input.event == BSP_BTN_PRESS) {
                state_lock();
                s_holding = true;
                s_hold_fired = false;
                s_hold_start = esp_timer_get_time();
                state_unlock();
            } else if (input.btn == BSP_BTN_OK && input.event == BSP_BTN_RELEASE) {
                state_lock();
                uint32_t held = (uint32_t)((esp_timer_get_time() - s_hold_start) / 1000);
                s_holding = false;
                if (!s_hold_fired) which = meeting_fsm_hold_fires(held) ? MP_IN_OK_HOLD : MP_IN_OK;
                state_unlock();
            } else if (input.event == BSP_BTN_CLICK && input.btn == BSP_BTN_UP) {
                which = MP_IN_UP;
            } else if (input.event == BSP_BTN_CLICK && input.btn == BSP_BTN_DOWN) {
                which = MP_IN_DOWN;
            }
            if (which) apply_action(fsm_input(which));
        }
        mp_action_t hold_action = MP_ACT_NONE;
        bool hold_hit = false;
        state_lock();
        if (s_holding && !s_hold_fired) {
            uint32_t held = (uint32_t)((esp_timer_get_time() - s_hold_start) / 1000);
            if (meeting_fsm_hold_fires(held) && s_fsm.screen == MP_SCR_RECORD) {
                s_hold_fired = true;
                s_holding = false;
                hold_action = meeting_fsm_input(&s_fsm, MP_IN_OK_HOLD);
                hold_hit = true;
            }
        }
        state_unlock();
        if (hold_hit) apply_action(hold_action);
        if (fsm_screen() == MP_SCR_RECORD || fsm_screen() == MP_SCR_RESULT) {
            apply_action(fsm_input(MP_IN_TICK_100MS));
        }
        if (fsm_screen() == MP_SCR_RECORD || s_ui_dirty) refresh_ui();
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "MemoPass 启动");
    s_state_mu = xSemaphoreCreateRecursiveMutex();
    if (!s_state_mu) ESP_LOGE(TAG, "state mutex alloc failed");
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
