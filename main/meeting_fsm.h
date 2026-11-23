#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MemoPass screen and button reduction. No ESP-IDF or LVGL types. */

#define MP_HOLD_END_MS 1200u
#define MP_MARK_SHOW_MS 1500u
#define MP_RESULT_RETURN_MS 3000u
#define MP_SEGMENT_BYTES 2500u
#define MP_SETTINGS_COUNT 4

typedef enum {
    MP_SCR_PROVISION = 0,
    MP_SCR_HOME,
    MP_SCR_RECORD,
    MP_SCR_RESULT,
    MP_SCR_SETTINGS,
    MP_SCR_HISTORY,
} mp_screen_t;

typedef enum {
    MP_IN_UP = 1,
    MP_IN_DOWN,
    MP_IN_OK,
    MP_IN_OK_HOLD,
    MP_IN_TICK_100MS,
} mp_input_t;

typedef enum {
    MP_ACT_NONE = 0,
    MP_ACT_REDRAW,
    MP_ACT_START,
    MP_ACT_PAUSE,
    MP_ACT_RESUME,
    MP_ACT_MARK,
    MP_ACT_MUTE,
    MP_ACT_UNMUTE,
    MP_ACT_STOP,
    MP_ACT_PROVISION,
    MP_ACT_LEAVE_PROVISION,
    MP_ACT_TEST_PUSH,
    MP_ACT_FACTORY,
    MP_ACT_RETRY_PUSH,
} mp_action_t;

typedef enum {
    MP_ERR_AUTH = 1,
    MP_ERR_RESOURCE,
    MP_ERR_FEISHU,
    MP_ERR_NET,
    MP_ERR_EMPTY,
} mp_err_t;

typedef struct {
    mp_screen_t screen;
    bool configured;
    bool recording;
    bool paused;
    bool muted;
    bool mark_on;
    uint16_t mark_age_ms;
    uint8_t settings_i;
    bool factory_armed;
    bool result_ok;
    bool result_pending;
    bool detail_on;
    uint16_t return_age_ms;
    bool beep;
} meeting_fsm_t;

void meeting_fsm_init(meeting_fsm_t *st, bool configured);
mp_action_t meeting_fsm_input(meeting_fsm_t *st, mp_input_t input);
void meeting_fsm_push_done(meeting_fsm_t *st, bool ok);
void meeting_fsm_return_home(meeting_fsm_t *st);

const char *meeting_fsm_error_text(mp_err_t err);
const char *meeting_fsm_key_guide(const meeting_fsm_t *st);
bool meeting_fsm_segment_due(size_t utf8_bytes, uint32_t segment_ms, int interval_min);
bool meeting_fsm_hold_fires(uint32_t held_ms);

/* hour < 0 hides the clock. duration_sec is the meeting length. */
int meeting_fsm_format_last(char *out, size_t cap, int hour, int minute,
                            int duration_sec, bool pushed);
int meeting_fsm_format_timer(char *out, size_t cap, uint32_t elapsed_ms);
