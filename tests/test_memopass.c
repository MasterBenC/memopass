#include "app_config.h"
#include "feishu_formatter.h"
#include "meeting_fsm.h"
#include "mp_utf8.h"
#include "portal_server.h"
#include "volc_asr_json.h"
#include "volc_asr_proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_fsm(void) {
    meeting_fsm_t st;
    meeting_fsm_init(&st, false);
    assert(st.screen == MP_SCR_PROVISION);
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_NONE);
    meeting_fsm_init(&st, true);
    assert(st.screen == MP_SCR_HOME);
    assert(meeting_fsm_input(&st, MP_IN_OK_HOLD) == MP_ACT_PROVISION);
    assert(st.screen == MP_SCR_PROVISION);
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_LEAVE_PROVISION);
    assert(st.screen == MP_SCR_HOME);
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_START);
    assert(st.screen == MP_SCR_RECORD && st.recording);
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_PAUSE);
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_RESUME);
    assert(meeting_fsm_input(&st, MP_IN_DOWN) == MP_ACT_MUTE);
    assert(strcmp(meeting_fsm_key_guide(&st), "上:标重点 | 确认:暂停 | 下:取消静音") == 0);
    assert(meeting_fsm_input(&st, MP_IN_UP) == MP_ACT_MARK);
    assert(st.mark_on);
    assert(meeting_fsm_input(&st, MP_IN_TICK_100MS) == MP_ACT_REDRAW);
    for (int i = 0; i < 20; i++) meeting_fsm_input(&st, MP_IN_TICK_100MS);
    assert(!st.mark_on);
    assert(meeting_fsm_input(&st, MP_IN_OK_HOLD) == MP_ACT_STOP);
    assert(st.screen == MP_SCR_RESULT);
    meeting_fsm_push_done(&st, true);
    assert(st.result_ok);
    assert(strcmp(meeting_fsm_key_guide(&st), "确认:回主页") == 0);
    for (int i = 0; i < 29; i++) assert(meeting_fsm_input(&st, MP_IN_TICK_100MS) == MP_ACT_NONE);
    assert(meeting_fsm_input(&st, MP_IN_TICK_100MS) == MP_ACT_REDRAW);
    assert(st.screen == MP_SCR_HOME);
    meeting_fsm_push_done(&st, false);
    assert(meeting_fsm_input(&st, MP_IN_DOWN) == MP_ACT_REDRAW);
    assert(st.detail_on);
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_RETRY_PUSH);
    assert(meeting_fsm_input(&st, MP_IN_UP) == MP_ACT_REDRAW);
    assert(st.screen == MP_SCR_HOME);
    assert(meeting_fsm_input(&st, MP_IN_UP) == MP_ACT_REDRAW);
    assert(st.screen == MP_SCR_SETTINGS);
    assert(meeting_fsm_input(&st, MP_IN_DOWN) == MP_ACT_REDRAW);
    assert(st.settings_i == 1);
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_TEST_PUSH);
    st.settings_i = 3;
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_REDRAW);
    assert(st.factory_armed);
    assert(meeting_fsm_input(&st, MP_IN_OK) == MP_ACT_FACTORY);
    assert(!meeting_fsm_hold_fires(1199));
    assert(meeting_fsm_hold_fires(1200));
    assert(!meeting_fsm_segment_due(100, 1000, 3));
    assert(meeting_fsm_segment_due(2500, 0, 3));
    assert(meeting_fsm_segment_due(10, 60000, 1));
    assert(!meeting_fsm_segment_due(10, 60000, 3));
    assert(meeting_fsm_segment_due(10, 180000, 3));
}

static void test_text(void) {
    char buf[64];
    assert(meeting_fsm_format_timer(buf, sizeof(buf), 754000) > 0);
    assert(strcmp(buf, "00:12:34") == 0);
    assert(meeting_fsm_format_last(buf, sizeof(buf), 14, 20, 28 * 60, true) > 0);
    assert(strcmp(buf, "上次 14:20 · 28分钟 · 已推飞书") == 0);
    assert(strcmp(meeting_fsm_error_text(MP_ERR_AUTH),
                  "火山引擎凭证无效，请核对 APP ID 与 Token") == 0);
    assert(strstr(meeting_fsm_error_text(MP_ERR_FEISHU), "飞书") != NULL);
    assert(strstr(meeting_fsm_error_text(MP_ERR_NET), "HTTP") == NULL);
    assert(mp_utf8_fit("中文A", 7, 4) == 3);
    assert(mp_utf8_valid("中文", 6));
    assert(!mp_utf8_valid("中文", 5));
}

static void test_feishu(void) {
    char body[512];
    char sign[64];
    feishu_job_t job;
    const char *bot = "https://open.feishu.cn/open-apis/bot/v2/hook/abc";
    const char *bit = "https://www.feishu.cn/base/automation/webhook/xyz";
    assert(feishu_kind_from_url(bot) == FEISHU_KIND_BOT);
    assert(feishu_kind_from_url(bit) == FEISHU_KIND_BITABLE);
    assert(!feishu_url_ok("http://open.feishu.cn/open-apis/bot/v2/hook/abc"));
    assert(!feishu_url_ok("https://example.com/hook"));
    assert(feishu_sign(1599360473, "demo", sign, sizeof(sign)) > 0);
    assert(strcmp(sign, "l1N0gAcBjdwBvGm1xMjOF0XSyaLRpR7tuO5dHfhAYc8=") == 0);
    memset(&job, 0, sizeof(job));
    job.meeting_id = "ab12cd34";
    job.segment = 2;
    job.duration_sec = 180;
    job.text = "你好\"世界\"";
    job.text_len = strlen(job.text);
    assert(feishu_format(FEISHU_KIND_BOT, &job, body, sizeof(body)) > 0);
    assert(strstr(body, "会议纪要") != NULL);
    assert(strstr(body, "msg_type") != NULL);
    assert(strstr(body, "你好\\\"世界\\\"") != NULL);
    job.is_final = true;
    job.text = "结论";
    job.text_len = strlen(job.text);
    assert(feishu_format(FEISHU_KIND_BITABLE, &job, body, sizeof(body)) > 0);
    assert(strstr(body, "\"meeting_id\":\"ab12cd34\"") != NULL);
    assert(strstr(body, "\"segment\":2") != NULL);
    assert(strstr(body, "\"is_final\":true") != NULL);
    assert(strstr(body, "\"text\":\"结论\"") != NULL);
    assert(feishu_utf8_cut("甲乙丙", 9, 4) == 3);
}

static void test_portal_and_config(void) {
    portal_form_t form;
    app_config_t cfg;
    const char *body =
        "ssid=Office%202.4&pass=secret+word&app_id=123456&access=token-value"
        "&webhook=https%3A%2F%2Fopen.feishu.cn%2Fopen-apis%2Fbot%2Fv2%2Fhook%2Fabc"
        "&segment=1&auth=app";
    assert(portal_form_parse(body, strlen(body), &form) == 0);
    assert(strcmp(form.ssid, "Office 2.4") == 0);
    assert(strcmp(form.pass, "secret word") == 0);
    assert(strcmp(form.app_id, "123456") == 0);
    assert(form.segment_min == 1);
    assert(!form.api_key_mode);
    assert(feishu_kind_from_url(form.webhook) == FEISHU_KIND_BOT);
    snprintf(cfg.webhook, sizeof(cfg.webhook), "%s", form.webhook);
    assert(portal_form_parse("ssid=%ZZ", 8, &form) == -1);
    memset(&cfg, 0, sizeof(cfg));
    assert(!app_config_ready(&cfg));
    snprintf(cfg.ssid, sizeof(cfg.ssid), "Office");
    snprintf(cfg.app_id, sizeof(cfg.app_id), "123456");
    snprintf(cfg.access, sizeof(cfg.access), "token-value");
    snprintf(cfg.webhook, sizeof(cfg.webhook),
             "https://open.feishu.cn/open-apis/bot/v2/hook/abc");
    assert(app_config_ready(&cfg));
    assert(app_config_segment_min(9) == 3);
    assert(strcmp(app_config_resource(&cfg), APP_RESOURCE_DEFAULT) == 0);
}

static void test_asr(void) {
    char headers[320];
    const uint8_t payload[] = {'A', 'B'};
    uint8_t frame[32];
    volc_frame_t decoded;
    char text[64];
    size_t n = volc_frame_encode(frame, sizeof(frame), VOLC_MSG_FULL_CLIENT,
                                 VOLC_FLAG_SEQUENCE, VOLC_SER_JSON, VOLC_COMP_NONE,
                                 true, 1, false, 0, payload, sizeof(payload));
    assert(n == 14);
    assert(volc_frame_decode(frame, n, &decoded) == 0);
    assert(decoded.sequence == 1 && decoded.payload_len == 2);
    assert(volc_asr_headers(headers, sizeof(headers), "123456", "access-token",
                            "", "volc.bigasr.sauc.duration", "req-1") > 0);
    assert(strstr(headers, "X-Api-App-Key: 123456\r\n") != NULL);
    assert(strstr(headers, "X-Api-Access-Key: access-token\r\n") != NULL);
    assert(strstr(headers, "X-Api-Request-Id: req-1\r\n") != NULL);
    assert(strstr(headers, "X-Api-Resource-Id: volc.bigasr.sauc.duration\r\n") != NULL);
    assert(volc_asr_headers(headers, sizeof(headers), "", "", "api-key-value",
                            NULL, "req-2") > 0);
    assert(strstr(headers, "X-Api-Key: api-key-value\r\n") != NULL);
    assert(strstr(headers, "X-Api-Request-Id: req-2\r\n") != NULL);
    assert(strstr(volc_asr_request_json(), "\"rate\":16000") != NULL);
    assert(volc_asr_classify(401, "") == VOLC_FAIL_AUTH);
    assert(volc_asr_classify(0, "resource not opened") == VOLC_FAIL_RESOURCE);
    assert(volc_asr_classify(8, "") == VOLC_FAIL_NET);
    assert(volc_json_longest_text("{\"text\":\"\\u4f60\\u597d\"}", 22, text, sizeof(text)) == 6);
    assert(strcmp(text, "你好") == 0);
}

int main(void) {
    test_fsm();
    test_text();
    test_feishu();
    test_portal_and_config();
    test_asr();
    puts("memopass host tests: PASS");
    return 0;
}
