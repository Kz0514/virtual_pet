/** @file mock/esp_spiffs.h — Mock SPIFFS: just an empty include.
 *  Actual fopen redirect is done via __wrap_fopen in sim_spiffs.c */
#pragma once
/* no-op — sim_spiffs.c provides __wrap_fopen for /spiffs path redirect */
