#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* SoftAP and STA are mutually exclusive. A portal test may briefly add STA
 * beside the AP; work-mode STA still refuses to start while the portal owns Wi-Fi. */

esp_err_t app_net_start_portal(char *ssid, size_t ssid_cap);
esp_err_t app_net_start_sta(const char *ssid, const char *pass);
void app_net_stop(void);
bool app_net_sta_up(void);
bool app_net_portal_up(void);
uint32_t app_net_clock(void);

esp_err_t app_net_portal_join(const char *ssid, const char *pass);
void app_net_portal_leave(void);

int app_net_scan_cached(char ssids[][33], int *rssi, int max_n);
int app_net_scan_refresh(void);
