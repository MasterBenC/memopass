#pragma once

#include "app_config.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_PUSH_TEXT 2500

typedef struct {
    char meeting_id[12];
    int segment;
    bool is_final;
    int duration_sec;
    char text[FEISHU_PUSH_TEXT + 1];
    size_t text_len;
} feishu_seg_t;

esp_err_t feishu_pusher_start(const app_config_t *cfg);
void feishu_pusher_stop(void);
bool feishu_pusher_submit(const feishu_seg_t *seg);
int feishu_pusher_sent(void);
int feishu_pusher_failed(void);
bool feishu_pusher_idle(void);
esp_err_t feishu_pusher_once(const app_config_t *cfg, const char *text);
