#include "kitsu_controller_permissions.h"

#include <cstdio>
#include <cstring>

namespace {

using kitsu868::connectivity::BleActionKind;
using kitsu868::connectivity::ControllerCapability;
using kitsu868::connectivity::ControllerCapabilityKind;
using kitsu868::connectivity::ControllerPermission;
using kitsu868::connectivity::ControllerRole;
using kitsu868::connectivity::ControllerRoleCodecResult;

int failures = 0;

void expect(bool condition, const char* message) {
  if (condition) return;
  ++failures;
  std::printf("TEST_FAIL kitsu_controller_permissions %s\n", message);
}

struct ExpectedOperation {
  const char* name;
  bool caretakerAllowed;
  bool actionRequired;
};

const ExpectedOperation kExpectedOperations[] = {
    {"state.get", true, false},
    {"history.get", true, false},
    {"peers.get", false, false},
    {"messages.get", false, false},
    {"messages.get.v2", false, false},
    {"messages.get.v3", false, false},
    {"messages.get.v4", false, false},
    {"messages.mark_read", false, false},
    {"encounter.codes.get.v1", false, false},
    {"encounter.neighbors.get.v1", false, false},
    {"encounter.neighbor.action.v1", false, false},
    {"encounter.catalog.get.v1", false, false},
    {"encounter.discovery.get.v1", false, false},
    {"companion.profile.get.v1", true, false},
    {"companion.profile.nickname.set.v1", false, false},
    {"companion.request.answer.v1", false, false},
    {"companion.question.answer.v1", false, false},
    {"companion.presentation.open.v1", true, false},
    {"companion.presentation.read.v1", true, false},
    {"companion.presentation.close.v1", true, false},
    {"focus.state.get.v1", true, false},
    {"focus.start.v1", true, false},
    {"focus.stop.v1", true, false},
    {"focus.cancel.v1", true, false},
    {"focus.ack.v1", true, false},
    {"adventure.state.get.v1", true, false},
    {"adventure.walk.start.v1", true, false},
    {"adventure.walk.sync.v1", true, false},
    {"adventure.walk.location.v1", true, false},
    {"adventure.walk.decide.v1", true, false},
    {"adventure.walk.finish.v1", true, false},
    {"adventure.walk.ack.v1", true, false},
    {"adventure.privacy.set.v1", false, false},
    {"adventure.home.set.v1", false, false},
    {"fun.state.get.v1", false, false},
    {"fun.expedition.start.v1", false, false},
    {"fun.expedition.claim.v1", false, false},
    {"fun.story.start.v1", false, false},
    {"fun.story.advance.v1", false, false},
    {"fun.story.choose.v1", false, false},
    {"fun.party.scan.v1", false, false},
    {"fun.party.host.v1", false, false},
    {"fun.party.join.v1", false, false},
    {"fun.party.begin.v1", false, false},
    {"fun.party.choose.v1", false, false},
    {"fun.party.leave.v1", false, false},
    {"channels.get", false, false},
    {"channels.get.v2", false, false},
    {"chat.storage.get", false, false},
    {"clock.sync", false, false},
    {"mesh.configure", false, false},
    {"action.apply", true, true},
    {"controller.forget", false, false},
    {"firmware.update.status", false, false},
    {"firmware.update.begin", false, false},
    {"firmware.update.write", false, false},
    {"firmware.update.finish", false, false},
    {"firmware.update.reboot", false, false},
    {"firmware.update.abort", false, false},
};

void testRolePadding() {
  using namespace kitsu868::connectivity;
  static_assert(static_cast<uint8_t>(ControllerRole::Owner) == 0U,
                "legacy zero role must remain Owner");
  static_assert(kControllerRolePaddingBytes == 3U,
                "security record exposes exactly three padding bytes");

  const uint8_t legacy[kControllerRolePaddingBytes] = {0U, 0U, 0U};
  ControllerRole role = ControllerRole::Caretaker;
  expect(decodeControllerRolePadding(legacy, sizeof(legacy), role) ==
             ControllerRoleCodecResult::Ok &&
             role == ControllerRole::Owner,
         "legacy zero padding must decode as owner");

  uint8_t encoded[kControllerRolePaddingBytes] = {0xA5U, 0xA5U, 0xA5U};
  expect(encodeControllerRolePadding(ControllerRole::Owner, encoded,
                                     sizeof(encoded)) ==
             ControllerRoleCodecResult::Ok &&
             std::memcmp(encoded, legacy, sizeof(encoded)) == 0,
         "owner encoding must stay all zero");
  expect(encodeControllerRolePadding(ControllerRole::Caretaker, encoded,
                                     sizeof(encoded)) ==
             ControllerRoleCodecResult::Ok &&
             encoded[0] == 1U && encoded[1] == 0U && encoded[2] == 0U,
         "caretaker encoding must be exact");
  role = ControllerRole::Owner;
  expect(decodeControllerRolePadding(encoded, sizeof(encoded), role) ==
             ControllerRoleCodecResult::Ok &&
             role == ControllerRole::Caretaker,
         "caretaker role must round trip");

  const uint8_t unknownRole[kControllerRolePaddingBytes] = {2U, 0U, 0U};
  role = ControllerRole::Caretaker;
  expect(decodeControllerRolePadding(unknownRole, sizeof(unknownRole), role) ==
             ControllerRoleCodecResult::UnsupportedEncoding &&
             static_cast<uint8_t>(role) == 0xFFU,
         "unknown stored role must fail closed");
  const uint8_t nonzeroReserved[kControllerRolePaddingBytes] = {0U, 1U, 0U};
  expect(decodeControllerRolePadding(nonzeroReserved,
                                     sizeof(nonzeroReserved), role) ==
             ControllerRoleCodecResult::UnsupportedEncoding,
         "nonzero reserved role byte must be rejected");
  expect(decodeControllerRolePadding(nullptr, sizeof(legacy), role) ==
             ControllerRoleCodecResult::InvalidArgument,
         "null role padding must be rejected");
  expect(decodeControllerRolePadding(legacy, sizeof(legacy) - 1U, role) ==
             ControllerRoleCodecResult::InvalidArgument,
         "wrong role padding size must be rejected");
  expect(encodeControllerRolePadding(static_cast<ControllerRole>(2U), encoded,
                                     sizeof(encoded)) ==
             ControllerRoleCodecResult::UnsupportedEncoding &&
             encoded[0] == 0xFFU && encoded[1] == 0xFFU &&
             encoded[2] == 0xFFU,
         "invalid runtime role must not encode owner authority implicitly");
}

void testExactOperationPolicy() {
  using namespace kitsu868::connectivity;
  const size_t count = sizeof(kExpectedOperations) /
                       sizeof(kExpectedOperations[0]);
  expect(controllerOperationPolicyCount() == count,
         "operation policy count must match exhaustive test table");

  for (size_t index = 0U; index < count; ++index) {
    const ExpectedOperation& expected = kExpectedOperations[index];
    const ControllerCapability capability =
        controllerOperation(expected.name);
    const ControllerPermission owner =
        controllerPermission(ControllerRole::Owner, capability);
    const ControllerPermission caretaker =
        controllerPermission(ControllerRole::Caretaker, capability);
    const ControllerPermission ownerExpected = expected.actionRequired
        ? ControllerPermission::ActionRequired
        : ControllerPermission::Allowed;
    const ControllerPermission caretakerExpected =
        !expected.caretakerAllowed
            ? ControllerPermission::Denied
            : expected.actionRequired
                  ? ControllerPermission::ActionRequired
                  : ControllerPermission::Allowed;
    if (owner != ownerExpected) {
      ++failures;
      std::printf("TEST_FAIL kitsu_controller_permissions owner op=%s got=%s\n",
                  expected.name, controllerPermissionName(owner));
    }
    if (caretaker != caretakerExpected) {
      ++failures;
      std::printf(
          "TEST_FAIL kitsu_controller_permissions caretaker op=%s got=%s\n",
          expected.name, controllerPermissionName(caretaker));
    }
    for (size_t other = index + 1U; other < count; ++other) {
      expect(std::strcmp(expected.name, kExpectedOperations[other].name) != 0,
             "operation policy test table must not contain duplicates");
    }
  }
}

void testActionPolicy() {
  using namespace kitsu868::connectivity;
  const BleActionKind actions[] = {
      BleActionKind::Pet,          BleActionKind::Feed,
      BleActionKind::Play,         BleActionKind::ListenOnce,
      BleActionKind::AdvertiseOnce, BleActionKind::SendMessage,
  };
  for (size_t index = 0U; index < sizeof(actions) / sizeof(actions[0]);
       ++index) {
    const ControllerCapability capability = controllerBleAction(actions[index]);
    expect(controllerPermission(ControllerRole::Owner, capability) ==
               ControllerPermission::Allowed,
           "owner must retain every existing action");
    const bool careAction = actions[index] == BleActionKind::Pet ||
        actions[index] == BleActionKind::Feed ||
        actions[index] == BleActionKind::Play;
    expect(controllerPermission(ControllerRole::Caretaker, capability) ==
               (careAction ? ControllerPermission::Allowed
                           : ControllerPermission::Denied),
           "caretaker action classification must be exact");
  }

  expect(controllerPermission(ControllerRole::Caretaker,
                              controllerOperation("action.apply")) ==
             ControllerPermission::ActionRequired,
         "generic action envelope must require decoded action authorization");
  expect(!controllerAllowed(ControllerRole::Caretaker,
                            controllerOperation("action.apply")),
         "action envelope alone must never complete authorization");
  expect(controllerAllowed(ControllerRole::Caretaker,
                           controllerBleAction(BleActionKind::Pet)),
         "caretaker pet action must be allowed after decoding");
  expect(!controllerAllowed(ControllerRole::Caretaker,
                            controllerBleAction(BleActionKind::SendMessage)),
         "caretaker messaging action must be denied after decoding");
  expect(controllerPermission(
             ControllerRole::Owner,
             controllerBleAction(static_cast<BleActionKind>(0xFFU))) ==
             ControllerPermission::UnknownCapability,
         "unknown action enum must fail closed");
}

void testUnknownsAndInventoryFailClosed() {
  using namespace kitsu868::connectivity;
  const char* const unknownOperations[] = {
      "inventory.get.v1",
      "inventory.grant.v1",
      "inventory.consume.v1",
      "companion.pack.install.v1",
      "companion.pack.replace.v1",
      "controller.add.v1",
      "settings.set.v1",
      "unlock.apply.v1",
      "Inventory.get.v1",
      "state.get.extra",
      " state.get",
      "state.get ",
  };
  for (size_t index = 0U;
       index < sizeof(unknownOperations) / sizeof(unknownOperations[0]);
       ++index) {
    const ControllerCapability capability =
        controllerOperation(unknownOperations[index]);
    expect(controllerPermission(ControllerRole::Owner, capability) ==
               ControllerPermission::UnknownCapability,
           "unknown owner operation must fail closed");
    expect(controllerPermission(ControllerRole::Caretaker, capability) ==
               ControllerPermission::UnknownCapability,
           "unknown caretaker or inventory operation must fail closed");
  }

  expect(controllerPermission(ControllerRole::Owner,
                              controllerOperation(nullptr)) ==
             ControllerPermission::InvalidArgument,
         "null operation must be invalid");
  expect(controllerPermission(ControllerRole::Owner,
                              controllerOperation("")) ==
             ControllerPermission::InvalidArgument,
         "empty operation must be invalid");
  expect(controllerPermission(static_cast<ControllerRole>(0xFFU),
                              controllerOperation("state.get")) ==
             ControllerPermission::InvalidRole,
         "unknown controller role must fail closed");

  ControllerCapability invalid{};
  invalid.kind = static_cast<ControllerCapabilityKind>(0xFFU);
  expect(controllerPermission(ControllerRole::Owner, invalid) ==
             ControllerPermission::InvalidArgument,
         "unknown capability kind must fail closed");
}

void testNames() {
  using namespace kitsu868::connectivity;
  expect(std::strcmp(controllerRoleName(ControllerRole::Owner), "owner") == 0,
         "owner role name must be stable");
  expect(std::strcmp(controllerRoleName(ControllerRole::Caretaker),
                     "caretaker") == 0,
         "caretaker role name must be stable");
  expect(std::strcmp(controllerPermissionName(
                         ControllerPermission::ActionRequired),
                     "action_required") == 0,
         "action-required result name must be stable");
  expect(std::strcmp(controllerRoleCodecResultName(
                         ControllerRoleCodecResult::UnsupportedEncoding),
                     "unsupported_encoding") == 0,
         "role codec result name must be stable");
}

}  // namespace

int main() {
  testRolePadding();
  testExactOperationPolicy();
  testActionPolicy();
  testUnknownsAndInventoryFailClosed();
  testNames();
  if (failures != 0) return 1;
  std::printf(
      "TEST_PASS kitsu_controller_permissions operations=%zu "
      "roles=owner,caretaker caretaker_actions=pet,feed,play\n",
      sizeof(kExpectedOperations) / sizeof(kExpectedOperations[0]));
  return 0;
}
