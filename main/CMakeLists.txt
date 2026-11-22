idf_component_register(
    SRCS "main.c"
         "app_config.c"
         "app_net.c"
         "audio_pipeline.c"
         "feishu_formatter.c"
         "feishu_pusher.c"
         "meeting_fsm.c"
         "memopass_ui.c"
         "mp_utf8.c"
         "portal_http.c"
         "portal_server.c"
         "volc_asr_client.c"
         "volc_asr_json.c"
         "volc_asr_proto.c"
         "fonts/mp_digits_28.c"
         "fonts/mp_ui_18.c"
         "fonts/mp_hint_12.c"
         "fonts/mp_sub_14.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_http_server esp_netif esp_timer
             esp_wifi esp_websocket_client mbedtls nvs_flash
)
