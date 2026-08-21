#pragma once

#if defined(ARDUINO_ARCH_ESP32) && \
    !defined(KITSU_REFLASHABLE_GUARD_HOST_TEST)
#include <sdkconfig.h>
#endif

// Kitsu's owner image deliberately remains a normal, repurposable ESP32-S3
// image. These compile-time assertions turn any future attempt to combine the
// selected profile with one-way silicon security configuration into a build
// failure, before an image can be produced.
#if defined(ARDUINO_ARCH_ESP32) && \
    (!defined(KITSU_SECURITY_MODE_REFLASHABLE) || \
     KITSU_SECURITY_MODE_REFLASHABLE != 1)
#error "Kitsu owner firmware requires KITSU_SECURITY_MODE_REFLASHABLE=1"
#endif

#if defined(KITSU_PRODUCTION_PROFILE) || \
    (defined(CONFIG_SECURE_BOOT) && CONFIG_SECURE_BOOT) || \
    (defined(CONFIG_SECURE_BOOT_V2_ENABLED) && \
     CONFIG_SECURE_BOOT_V2_ENABLED) || \
    (defined(CONFIG_SECURE_FLASH_ENC_ENABLED) && \
     CONFIG_SECURE_FLASH_ENC_ENABLED) || \
    (defined(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE) && \
     CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE) || \
    (defined(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT) && \
     CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT) || \
    (defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION) || \
    (defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK) && \
     CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK) || \
    (defined(CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE) && \
     CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE) || \
    (defined(CONFIG_SECURE_DISABLE_ROM_DL_MODE) && \
     CONFIG_SECURE_DISABLE_ROM_DL_MODE)
#error "Reflashable Kitsu forbids secure boot, flash/NVS encryption, and ROM download locks"
#endif

namespace kitsu868 {
namespace connectivity {

enum class SecurityMode : unsigned char { Reflashable = 0 };

constexpr SecurityMode kSelectedSecurityMode = SecurityMode::Reflashable;
constexpr const char* kSelectedSecurityModeName = "reflashable";
constexpr bool kSecureBootEnabled = false;
constexpr bool kFlashEncryptionEnabled = false;
constexpr bool kNvsEncryptionEnabled = false;
constexpr bool kHardwareRootProtected = false;
constexpr bool kApplicationRecordsEncrypted = true;
constexpr bool kEfuseProgrammingAllowed = false;

}  // namespace connectivity
}  // namespace kitsu868
