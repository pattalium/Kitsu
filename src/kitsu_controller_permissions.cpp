#include "kitsu_controller_permissions.h"

#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint8_t kOwnerMask = 1U << 0U;
constexpr uint8_t kCaretakerMask = 1U << 1U;
constexpr uint8_t kBothRoles = kOwnerMask | kCaretakerMask;

struct OperationPolicy {
  const char* operation;
  uint8_t roles;
  bool requiresAction;
};

// This table is the closed authenticated BLE operation registry. Adding a
// handler elsewhere does not grant access: the exact operation must also be
// classified here. Caretaker entries are intentionally narrow and explicit.
const OperationPolicy kOperationPolicies[] = {
    {"state.get", kBothRoles, false},
    {"history.get", kBothRoles, false},
    {"peers.get", kOwnerMask, false},
    {"messages.get", kOwnerMask, false},
    {"messages.get.v2", kOwnerMask, false},
    {"messages.get.v3", kOwnerMask, false},
    {"messages.get.v4", kOwnerMask, false},
    {"messages.mark_read", kOwnerMask, false},
    {"encounter.codes.get.v1", kOwnerMask, false},
    {"encounter.neighbors.get.v1", kOwnerMask, false},
    {"encounter.neighbor.action.v1", kOwnerMask, false},
    {"encounter.catalog.get.v1", kOwnerMask, false},
    {"encounter.discovery.get.v1", kOwnerMask, false},
    {"companion.profile.get.v1", kBothRoles, false},
    {"companion.profile.nickname.set.v1", kOwnerMask, false},
    {"companion.request.answer.v1", kOwnerMask, false},
    {"companion.question.answer.v1", kOwnerMask, false},
    {"companion.presentation.open.v1", kBothRoles, false},
    {"companion.presentation.read.v1", kBothRoles, false},
    {"companion.presentation.close.v1", kBothRoles, false},
    {"focus.state.get.v1", kBothRoles, false},
    {"focus.start.v1", kBothRoles, false},
    {"focus.stop.v1", kBothRoles, false},
    {"focus.cancel.v1", kBothRoles, false},
    {"focus.ack.v1", kBothRoles, false},
    {"adventure.state.get.v1", kBothRoles, false},
    {"adventure.walk.start.v1", kBothRoles, false},
    {"adventure.walk.sync.v1", kBothRoles, false},
    {"adventure.walk.location.v1", kBothRoles, false},
    {"adventure.walk.decide.v1", kBothRoles, false},
    {"adventure.walk.finish.v1", kBothRoles, false},
    {"adventure.walk.ack.v1", kBothRoles, false},
    {"adventure.privacy.set.v1", kOwnerMask, false},
    {"adventure.home.set.v1", kOwnerMask, false},
    {"fun.state.get.v1", kOwnerMask, false},
    {"fun.expedition.start.v1", kOwnerMask, false},
    {"fun.expedition.claim.v1", kOwnerMask, false},
    {"fun.story.start.v1", kOwnerMask, false},
    {"fun.story.advance.v1", kOwnerMask, false},
    {"fun.story.choose.v1", kOwnerMask, false},
    {"fun.party.scan.v1", kOwnerMask, false},
    {"fun.party.host.v1", kOwnerMask, false},
    {"fun.party.join.v1", kOwnerMask, false},
    {"fun.party.begin.v1", kOwnerMask, false},
    {"fun.party.choose.v1", kOwnerMask, false},
    {"fun.party.leave.v1", kOwnerMask, false},
    {"channels.get", kOwnerMask, false},
    {"channels.get.v2", kOwnerMask, false},
    {"chat.storage.get", kOwnerMask, false},
    {"clock.sync", kOwnerMask, false},
    {"mesh.configure", kOwnerMask, false},
    {"action.apply", kBothRoles, true},
    {"controller.forget", kOwnerMask, false},
    {"firmware.update.status", kOwnerMask, false},
    {"firmware.update.begin", kOwnerMask, false},
    {"firmware.update.write", kOwnerMask, false},
    {"firmware.update.finish", kOwnerMask, false},
    {"firmware.update.reboot", kOwnerMask, false},
    {"firmware.update.abort", kOwnerMask, false},
};

struct ActionPolicy {
  BleActionKind action;
  uint8_t roles;
};

const ActionPolicy kActionPolicies[] = {
    {BleActionKind::Pet, kBothRoles},
    {BleActionKind::Feed, kBothRoles},
    {BleActionKind::Play, kBothRoles},
    {BleActionKind::ListenOnce, kOwnerMask},
    {BleActionKind::AdvertiseOnce, kOwnerMask},
    {BleActionKind::SendMessage, kOwnerMask},
};

bool roleMask(ControllerRole role, uint8_t& mask) {
  switch (role) {
    case ControllerRole::Owner:
      mask = kOwnerMask;
      return true;
    case ControllerRole::Caretaker:
      mask = kCaretakerMask;
      return true;
  }
  mask = 0U;
  return false;
}

}  // namespace

ControllerRoleCodecResult decodeControllerRolePadding(
    const uint8_t* padding, size_t paddingBytes, ControllerRole& output) {
  output = static_cast<ControllerRole>(0xFFU);
  if (!padding || paddingBytes != kControllerRolePaddingBytes) {
    return ControllerRoleCodecResult::InvalidArgument;
  }
  if (padding[1] != 0U || padding[2] != 0U) {
    return ControllerRoleCodecResult::UnsupportedEncoding;
  }
  if (padding[0] == static_cast<uint8_t>(ControllerRole::Owner)) {
    output = ControllerRole::Owner;
    return ControllerRoleCodecResult::Ok;
  }
  if (padding[0] == static_cast<uint8_t>(ControllerRole::Caretaker)) {
    output = ControllerRole::Caretaker;
    return ControllerRoleCodecResult::Ok;
  }
  return ControllerRoleCodecResult::UnsupportedEncoding;
}

ControllerRoleCodecResult encodeControllerRolePadding(
    ControllerRole role, uint8_t* padding, size_t paddingBytes) {
  if (!padding || paddingBytes != kControllerRolePaddingBytes) {
    return ControllerRoleCodecResult::InvalidArgument;
  }
  if (role != ControllerRole::Owner && role != ControllerRole::Caretaker) {
    memset(padding, 0xFF, paddingBytes);
    return ControllerRoleCodecResult::UnsupportedEncoding;
  }
  memset(padding, 0, paddingBytes);
  padding[0] = static_cast<uint8_t>(role);
  return ControllerRoleCodecResult::Ok;
}

const char* controllerRoleName(ControllerRole role) {
  switch (role) {
    case ControllerRole::Owner: return "owner";
    case ControllerRole::Caretaker: return "caretaker";
  }
  return "unknown";
}

const char* controllerRoleCodecResultName(
    ControllerRoleCodecResult result) {
  switch (result) {
    case ControllerRoleCodecResult::Ok: return "ok";
    case ControllerRoleCodecResult::InvalidArgument:
      return "invalid_argument";
    case ControllerRoleCodecResult::UnsupportedEncoding:
      return "unsupported_encoding";
  }
  return "unknown";
}

ControllerCapability controllerOperation(const char* operation) {
  ControllerCapability capability{};
  capability.kind = ControllerCapabilityKind::Operation;
  capability.operation = operation;
  return capability;
}

ControllerCapability controllerBleAction(BleActionKind action) {
  ControllerCapability capability{};
  capability.kind = ControllerCapabilityKind::BleAction;
  capability.action = action;
  return capability;
}

ControllerPermission controllerPermission(
    ControllerRole role, const ControllerCapability& capability) {
  uint8_t mask = 0U;
  if (!roleMask(role, mask)) return ControllerPermission::InvalidRole;

  if (capability.kind == ControllerCapabilityKind::Operation) {
    if (!capability.operation || capability.operation[0] == '\0') {
      return ControllerPermission::InvalidArgument;
    }
    for (size_t index = 0U;
         index < sizeof(kOperationPolicies) / sizeof(kOperationPolicies[0]);
         ++index) {
      const OperationPolicy& policy = kOperationPolicies[index];
      if (strcmp(capability.operation, policy.operation) != 0) continue;
      if ((policy.roles & mask) == 0U) return ControllerPermission::Denied;
      return policy.requiresAction ? ControllerPermission::ActionRequired
                                   : ControllerPermission::Allowed;
    }
    return ControllerPermission::UnknownCapability;
  }

  if (capability.kind == ControllerCapabilityKind::BleAction) {
    for (size_t index = 0U;
         index < sizeof(kActionPolicies) / sizeof(kActionPolicies[0]);
         ++index) {
      const ActionPolicy& policy = kActionPolicies[index];
      if (capability.action != policy.action) continue;
      return (policy.roles & mask) != 0U ? ControllerPermission::Allowed
                                        : ControllerPermission::Denied;
    }
    return ControllerPermission::UnknownCapability;
  }

  return ControllerPermission::InvalidArgument;
}

bool controllerAllowed(ControllerRole role,
                       const ControllerCapability& capability) {
  return controllerPermission(role, capability) ==
         ControllerPermission::Allowed;
}

size_t controllerOperationPolicyCount() {
  return sizeof(kOperationPolicies) / sizeof(kOperationPolicies[0]);
}

const char* controllerPermissionName(ControllerPermission permission) {
  switch (permission) {
    case ControllerPermission::Allowed: return "allowed";
    case ControllerPermission::ActionRequired: return "action_required";
    case ControllerPermission::Denied: return "denied";
    case ControllerPermission::UnknownCapability:
      return "unknown_capability";
    case ControllerPermission::InvalidRole: return "invalid_role";
    case ControllerPermission::InvalidArgument: return "invalid_argument";
  }
  return "unknown";
}

}  // namespace connectivity
}  // namespace kitsu868
