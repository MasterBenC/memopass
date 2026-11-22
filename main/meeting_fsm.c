#include "meeting_fsm.h"

#include <stdio.h>
#include <string.h>

void meeting_fsm_init(meeting_fsm_t *st, bool configured) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->configured = configured;
    st->screen = configured ? MP_SCR_HOME : MP_SCR_PROVISION;
}

static mp_action_t go(meeting_fsm_t *st, mp_screen_t screen) {
    st->screen = screen;
    st->factory_armed = false;
    st->detail_on = false;
    st->return_age_ms = 0;
    return MP_ACT_REDRAW;
}

static mp_action_t try_start(meeting_fsm_t *st) {
    if (!st->configured) return go(st, MP_SCR_PROVISION);
    st->recording = true;
    st->paused = false;
    st->muted = false;
    st->mark_on = false;
    st->mark_age_ms = 0;
    st->screen = MP_SCR_RECORD;
    return MP_ACT_START;
}

void meeting_fsm_push_done(meeting_fsm_t *st, bool ok) {
    if (!st) return;
    st->recording = false;
    st->paused = false;
    st->result_ok = ok;
    st->return_age_ms = 0;
    st->detail_on = false;
    st->screen = MP_SCR_RESULT;
}

mp_action_t meeting_fsm_input(meeting_fsm_t *st, mp_input_t input) {
    if (!st) return MP_ACT_NONE;

    if (input == MP_IN_TICK_100MS) {
        if (st->screen == MP_SCR_RECORD && st->mark_on) {
            if (st->mark_age_ms + 100u >= MP_MARK_SHOW_MS) st->mark_on = false;
            else st->mark_age_ms = (uint16_t)(st->mark_age_ms + 100u);
            return MP_ACT_REDRAW;
        }
        if (st->screen == MP_SCR_RESULT && st->result_ok) {
            st->return_age_ms = (uint16_t)(st->return_age_ms + 100u);
            if (st->return_age_ms >= MP_RESULT_RETURN_MS) return go(st, MP_SCR_HOME);
        }
        return MP_ACT_NONE;
    }

    switch (st->screen) {
    case MP_SCR_PROVISION:
        if (input == MP_IN_OK && st->configured) return go(st, MP_SCR_HOME) == MP_ACT_REDRAW
            ? MP_ACT_LEAVE_PROVISION : MP_ACT_LEAVE_PROVISION;
        return MP_ACT_NONE;
    case MP_SCR_HOME:
        if (input == MP_IN_UP) return go(st, MP_SCR_SETTINGS);
        if (input == MP_IN_DOWN) return go(st, MP_SCR_HISTORY);
        if (input == MP_IN_OK) return try_start(st);
        if (input == MP_IN_OK_HOLD) return go(st, MP_SCR_PROVISION) == MP_ACT_REDRAW
            ? MP_ACT_PROVISION : MP_ACT_PROVISION;
        return MP_ACT_NONE;
    case MP_SCR_RECORD:
        if (input == MP_IN_UP) {
            st->mark_on = true;
            st->mark_age_ms = 0;
            return MP_ACT_MARK;
        }
        if (input == MP_IN_DOWN) {
            st->muted = !st->muted;
            return st->muted ? MP_ACT_MUTE : MP_ACT_UNMUTE;
        }
        if (input == MP_IN_OK) {
            st->paused = !st->paused;
            return st->paused ? MP_ACT_PAUSE : MP_ACT_RESUME;
        }
        if (input == MP_IN_OK_HOLD) {
            st->recording = false;
            st->screen = MP_SCR_RESULT;
            st->result_ok = false;
            st->return_age_ms = 0;
            return MP_ACT_STOP;
        }
        return MP_ACT_NONE;
    case MP_SCR_RESULT:
        if (st->result_ok) {
            if (input == MP_IN_OK) return go(st, MP_SCR_HOME);
            return MP_ACT_NONE;
        }
        if (input == MP_IN_UP) return go(st, MP_SCR_HOME);
        if (input == MP_IN_OK) return MP_ACT_RETRY_PUSH;
        if (input == MP_IN_DOWN) {
            st->detail_on = true;
            return MP_ACT_REDRAW;
        }
        return MP_ACT_NONE;
    case MP_SCR_SETTINGS:
        if (input == MP_IN_UP) {
            st->settings_i = (uint8_t)((st->settings_i + MP_SETTINGS_COUNT - 1) % MP_SETTINGS_COUNT);
            st->factory_armed = false;
            return MP_ACT_REDRAW;
        }
        if (input == MP_IN_DOWN) {
            st->settings_i = (uint8_t)((st->settings_i + 1) % MP_SETTINGS_COUNT);
            st->factory_armed = false;
            return MP_ACT_REDRAW;
        }
        if (input == MP_IN_OK) {
            if (st->settings_i == 0) return go(st, MP_SCR_PROVISION) == MP_ACT_REDRAW
                ? MP_ACT_PROVISION : MP_ACT_PROVISION;
            if (st->settings_i == 1) return MP_ACT_TEST_PUSH;
            if (st->settings_i == 2) {
                st->beep = !st->beep;
                return MP_ACT_REDRAW;
            }
            if (!st->factory_armed) {
                st->factory_armed = true;
                return MP_ACT_REDRAW;
            }
            return MP_ACT_FACTORY;
        }
        if (input == MP_IN_OK_HOLD) return go(st, MP_SCR_HOME);
        return MP_ACT_NONE;
    case MP_SCR_HISTORY:
        if (input == MP_IN_OK || input == MP_IN_OK_HOLD) return go(st, MP_SCR_HOME);
        return MP_ACT_NONE;
    }
    return MP_ACT_NONE;
}

const char *meeting_fsm_error_text(mp_err_t err) {
    switch (err) {
    case MP_ERR_AUTH:
        return "火山引擎凭证无效，请核对 APP ID 与 Token";
    case MP_ERR_RESOURCE:
        return "请在火山引擎开通流式语音识别服务";
    case MP_ERR_FEISHU:
        return "飞书地址不可用，请检查机器人设置";
    case MP_ERR_NET:
    default:
        return "网络已断开，恢复后将自动继续";
    }
}

const char *meeting_fsm_key_guide(const meeting_fsm_t *st) {
    if (!st) return "";
    switch (st->screen) {
    case MP_SCR_PROVISION:
        return st->configured ? "确认:返回" : "";
    case MP_SCR_HOME:
        return "上:设置 | 确认:录音 | 下:记录";
    case MP_SCR_RECORD:
        if (st->paused && st->muted) return "上:标重点 | 确认:继续 | 下:取消静音";
        if (st->paused) return "上:标重点 | 确认:继续 | 下:静音";
        if (st->muted) return "上:标重点 | 确认:暂停 | 下:取消静音";
        return "上:标重点 | 确认:暂停 | 下:静音";
    case MP_SCR_RESULT:
        return st->result_ok ? "确认:回主页" : "上:放弃 | 确认:重试 | 下:详情";
    case MP_SCR_SETTINGS:
        return "上:上移 | 确认:执行 | 下:下移";
    case MP_SCR_HISTORY:
        return "确认:返回";
    }
    return "";
}

bool meeting_fsm_segment_due(size_t utf8_bytes, uint32_t segment_ms, int interval_min) {
    if (interval_min != 1 && interval_min != 5) interval_min = 3;
    if (utf8_bytes >= MP_SEGMENT_BYTES) return true;
    return segment_ms >= (uint32_t)interval_min * 60000u;
}

bool meeting_fsm_hold_fires(uint32_t held_ms) {
    return held_ms >= MP_HOLD_END_MS;
}

int meeting_fsm_format_last(char *out, size_t cap, int hour, int minute,
                            int duration_sec, bool pushed) {
    int minutes = duration_sec < 0 ? 0 : (duration_sec + 59) / 60;
    if (!out || cap == 0) return -1;
    if (hour < 0 || minute < 0 || hour > 23 || minute > 59) {
        return snprintf(out, cap, "上次 · %d分钟 · %s", minutes,
                        pushed ? "已推飞书" : "未推送");
    }
    return snprintf(out, cap, "上次 %02d:%02d · %d分钟 · %s",
                    hour, minute, minutes, pushed ? "已推飞书" : "未推送");
}

int meeting_fsm_format_timer(char *out, size_t cap, uint32_t elapsed_ms) {
    uint32_t total = elapsed_ms / 1000u;
    unsigned hh = (unsigned)(total / 3600u);
    unsigned mm = (unsigned)((total / 60u) % 60u);
    unsigned ss = (unsigned)(total % 60u);
    if (hh > 99) hh = 99;
    if (!out || cap == 0) return -1;
    return snprintf(out, cap, "%02u:%02u:%02u", hh, mm, ss);
}
