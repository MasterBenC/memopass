#pragma once

#include "app_config.h"
#include "meeting_fsm.h"

void memopass_ui_init(void);
void memopass_ui_provision(const char *qr, const char *ssid, bool can_back);
void memopass_ui_home(const char *last_line, bool wifi, bool feishu, int battery);
void memopass_ui_record(const meeting_fsm_t *st, const char *timer, const char *subtitle,
                        uint8_t level, int pushed, bool weak, uint16_t hold_ms);
void memopass_ui_result(bool ok, const char *detail, int duration_sec, int segments, bool detail_on);
void memopass_ui_settings(int index, bool beep, bool factory_armed);
void memopass_ui_history(const app_hist_t *items, int count);
