#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_partition.h"

using nvs_handle_t = uint32_t;
using nvs_open_mode_t = uint8_t;

constexpr nvs_open_mode_t NVS_READONLY = 0U;
constexpr nvs_open_mode_t NVS_READWRITE = 1U;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
constexpr esp_err_t ESP_ERR_NVS_INVALID_HANDLE = 0x1107;
constexpr esp_err_t ESP_ERR_NVS_READ_ONLY = 0x1104;
constexpr esp_err_t ESP_ERR_NVS_NOT_ENOUGH_SPACE = 0x1105;

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode,
                   nvs_handle_t* output);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* output,
                       size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key,
                       const void* input, size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
