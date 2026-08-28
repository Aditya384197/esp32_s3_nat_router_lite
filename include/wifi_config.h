#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

extern char *ssid;
extern char *passwd;
extern char *ap_ssid;
extern char *ap_passwd;
extern bool wifi_scan_active;

esp_err_t wifi_config_load(void);
esp_err_t wifi_config_save_sta(const char *new_ssid, const char *new_passwd);
esp_err_t wifi_config_save_ap(const char *new_ssid, const char *new_passwd);
esp_err_t wifi_config_apply_ap(void);
esp_err_t wifi_config_apply_sta(void);
void wifi_config_free(void);

#ifdef __cplusplus
}
#endif
