#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_ble_action.h"

namespace kitsu868 {
namespace connectivity {

// Owner is deliberately zero. The v2 security record currently writes three
// zero padding bytes beside every controller-valid byte, so records created by
// existing firmware decode as Owner when role storage is integrated later.
enum class ControllerRole : uint8_t {
  Owner = 0U,
  Caretaker = 1U,
};

constexpr size_t kControllerRolePaddingBytes = 3U;

enum class ControllerRoleCodecResult : uint8_t {
  Ok = 0U,
  InvalidArgument,
  UnsupportedEncoding,
};

// Exact future encoding for the three currently-zero bytes in each controller
// record: byte 0 is ControllerRole and bytes 1..2 remain zero/reserved.
// All-zero legacy padding therefore maps to Owner. Unknown roles or nonzero
// reserved bytes fail closed instead of gaining Owner permissions.
ControllerRoleCodecResult decodeControllerRolePadding(
    const uint8_t* padding, size_t paddingBytes, ControllerRole& output);
ControllerRoleCodecResult encodeControllerRolePadding(
    ControllerRole role, uint8_t* padding, size_t paddingBytes);

const char* controllerRoleName(ControllerRole role);
const char* controllerRoleCodecResultName(ControllerRoleCodecResult result);

enum class ControllerCapabilityKind : uint8_t {
  Operation = 0U,
  BleAction,
};

struct ControllerCapability {
  ControllerCapabilityKind kind = ControllerCapabilityKind::Operation;
  const char* operation = nullptr;
  BleActionKind action = BleActionKind::Pet;
};

ControllerCapability controllerOperation(const char* operation);
ControllerCapability controllerBleAction(BleActionKind action);

enum class ControllerPermission : uint8_t {
  Allowed = 0U,
  // action.apply is a known role-eligible envelope, but authorization is not
  // complete until its decoded BleActionKind is checked through this policy.
  ActionRequired,
  Denied,
  UnknownCapability,
  InvalidRole,
  InvalidArgument,
};

// The single authorization decision point for both authenticated operation
// names and decoded action.apply kinds. Names are matched exactly; unknown
// names and enum values fail closed for both roles.
ControllerPermission controllerPermission(
    ControllerRole role, const ControllerCapability& capability);
bool controllerAllowed(ControllerRole role,
                       const ControllerCapability& capability);

size_t controllerOperationPolicyCount();
const char* controllerPermissionName(ControllerPermission permission);

}  // namespace connectivity
}  // namespace kitsu868
