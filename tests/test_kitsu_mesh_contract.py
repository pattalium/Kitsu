"""Executable contract tests for Kitsu's app-facing MeshCore boundary."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
TRANSPORT_SOURCE = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)

from kitsu_mesh_commands import (  # noqa: E402
    MAX_COMMAND_BYTES,
    MESH_PREAMBLE_SYMBOLS,
    MESH_PRIVATE_SYNC_WORD,
    UK_EU_NARROW_PROFILE,
    ContractError,
    Coordinates,
    IntroduceScope,
    LocationMode,
    MapPublishPackage,
    MeshLocation,
    MeshRadioConfig,
    MeshResult,
    MeshStatus,
    mesh_config_command,
    mesh_enable_command,
    mesh_introduce_command,
    mesh_location_current_once_command,
    mesh_location_fixed_command,
    mesh_location_hidden_command,
    mesh_publish_map_command,
    mesh_status_command,
    mesh_time_command,
    mesh_tx_command,
    parse_device_line,
)


PROFILE = UK_EU_NARROW_PROFILE
POSITION = Coordinates(44_426_767, 26_102_538)
PUBLIC_KEY = "A1" * 32
ADVERT_HEX = "A2" * 118


def cpp_function(source: str, signature: str) -> str:
    """Return one C++ function body using a small brace-aware source scan."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for cursor in range(opening, len(source)):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start : cursor + 1]
    raise AssertionError(f"unterminated C++ function: {signature}")


def record(prefix: str, value: dict) -> str:
    return prefix + " " + json.dumps(value, ensure_ascii=False, separators=(",", ":"))


class CommandContractTest(unittest.TestCase):
    def test_all_commands_are_canonical_ascii_and_fit_existing_input(self) -> None:
        commands = (
            mesh_status_command(),
            mesh_enable_command(True),
            mesh_enable_command(False),
            mesh_config_command(PROFILE),
            mesh_location_hidden_command(),
            mesh_location_fixed_command(Coordinates(-90_000_000, -180_000_000)),
            mesh_location_current_once_command(Coordinates(90_000_000, 180_000_000)),
            mesh_introduce_command(IntroduceScope.NEARBY),
            mesh_introduce_command(IntroduceScope.MESH),
            mesh_time_command(1_775_638_400),
            mesh_tx_command(True),
            mesh_tx_command(False),
            mesh_publish_map_command(),
        )
        self.assertEqual(
            commands,
            (
                "mesh status",
                "mesh config on",
                "mesh config off",
                "mesh config 869618000 62500 8 5 22",
                "mesh location hidden",
                "mesh location fixed -90000000 -180000000",
                "mesh location current-once 90000000 180000000",
                "mesh introduce nearby",
                "mesh introduce mesh",
                "mesh time 1775638400",
                "mesh tx unlock",
                "mesh tx lock",
                "mesh publish-map",
            ),
        )
        for command in commands:
            encoded = command.encode("ascii")
            self.assertLessEqual(len(encoded), MAX_COMMAND_BYTES)
            self.assertNotRegex(command, r"[\r\n\0]")

    def test_coordinate_types_and_ranges_are_strict(self) -> None:
        with self.assertRaises(ContractError):
            Coordinates(True, 0)
        with self.assertRaises(ContractError):
            Coordinates(90_000_001, 0)
        with self.assertRaises(ContractError):
            Coordinates(0, -180_000_001)

    def test_radio_config_rejects_invalid_or_ambiguous_values(self) -> None:
        self.assertEqual(PROFILE, MeshRadioConfig(869_618_000, 62_500, 8, 5, 22))
        self.assertEqual(MESH_PRIVATE_SYNC_WORD, 0x12)
        self.assertEqual(MESH_PREAMBLE_SYMBOLS, 32)
        with self.assertRaises(ContractError):
            MeshRadioConfig(915_000_000, 62_500, 8, 5, 22)
        with self.assertRaises(ContractError):
            MeshRadioConfig(869_618_000, 60_000, 8, 5, 22)
        with self.assertRaises(ContractError):
            MeshRadioConfig(869_618_000, 62_500, True, 5, 22)
        with self.assertRaises(ContractError):
            MeshRadioConfig(869_618_000, 62_500, 13, 5, 22)

    def test_time_is_a_nonzero_unsigned_32_bit_epoch(self) -> None:
        self.assertEqual(mesh_time_command(0xFFFFFFFF), "mesh time 4294967295")
        for value in (0, -1, 0x1_0000_0000, True):
            with self.subTest(value=value):
                with self.assertRaises(ContractError):
                    mesh_time_command(value)


class RecordContractTest(unittest.TestCase):
    def status_value(self) -> dict:
        return {
            "protocol": 1,
            "available": True,
            "configured": True,
            "enabled": True,
            "role": "client",
            "kitsu": True,
            "uid": "KTDEAD",
            "marker": "fox",
            "advert_name": "🦊 Kitsu KTDEAD",
            "public_key": PUBLIC_KEY,
            "profile": {
                "frequency_hz": 869_618_000,
                "bandwidth_hz": 62_500,
                "spreading_factor": 8,
                "coding_rate": 5,
                "tx_power_dbm": 22,
            },
            "location": {"mode": "hidden", "lat_e6": None, "lon_e6": None},
            "time_valid": True,
            "tx_policy": "explicit_session",
            "tx_unlocked": True,
            "tx_ready": True,
            "rx_ready": True,
            "received_adverts": 12,
            "dropped_adverts": 2,
            "queued_adverts": 1,
            "map_upload": "phone_only",
        }

    def test_status_accepts_utf8_name_and_ignores_unknown_fields(self) -> None:
        value = self.status_value()
        value["future_capability"] = {"anything": 2}
        parsed = parse_device_line(record("KITSU_MESH", value).encode("utf-8"))
        self.assertIsInstance(parsed, MeshStatus)
        assert isinstance(parsed, MeshStatus)
        self.assertEqual(parsed.role, "client")
        self.assertEqual(parsed.marker, "fox")
        self.assertEqual(parsed.advert_name, "🦊 Kitsu KTDEAD")
        self.assertEqual(parsed.profile, PROFILE)
        self.assertEqual(parsed.location.mode, LocationMode.HIDDEN)
        self.assertTrue(parsed.rx_ready)
        self.assertEqual(parsed.received_adverts, 12)
        self.assertEqual(parsed.dropped_adverts, 2)
        self.assertEqual(parsed.queued_adverts, 1)

    def test_status_advert_counters_must_be_nonnegative(self) -> None:
        for field in ("received_adverts", "dropped_adverts", "queued_adverts"):
            with self.subTest(field=field):
                value = self.status_value()
                value[field] = -1
                with self.assertRaisesRegex(ContractError, "non-negative"):
                    parse_device_line(record("KITSU_MESH", value))

        value = self.status_value()
        value.update({"received_adverts": 1, "dropped_adverts": 2})
        with self.assertRaisesRegex(ContractError, "cannot exceed"):
            parse_device_line(record("KITSU_MESH", value))

    def test_unconfigured_status_uses_a_null_profile(self) -> None:
        value = self.status_value()
        value.update(
            {
                "configured": False,
                "enabled": False,
                "profile": None,
                "tx_policy": "locked",
                "tx_unlocked": False,
                "tx_ready": False,
                "rx_ready": False,
                "queued_adverts": 0,
            }
        )
        parsed = parse_device_line(record("KITSU_MESH", value))
        self.assertIsInstance(parsed, MeshStatus)
        assert isinstance(parsed, MeshStatus)
        self.assertIsNone(parsed.profile)

    def test_existing_v1_and_unknown_lines_are_ignored(self) -> None:
        self.assertIsNone(parse_device_line('KITSU_SYNC {"protocol":1}'))
        self.assertIsNone(parse_device_line("KITSU_BOOT firmware=Kitsu868"))
        self.assertIsNone(parse_device_line("debug mentions KITSU_MESH but is not a record"))

    def test_recognized_invalid_json_duplicate_keys_and_nan_are_rejected(self) -> None:
        with self.assertRaises(ContractError):
            parse_device_line("KITSU_MESH {")
        with self.assertRaises(ContractError):
            parse_device_line('KITSU_MESH {"protocol":1,"protocol":1}')
        with self.assertRaises(ContractError):
            parse_device_line('KITSU_MESH {"protocol":1,"x":NaN}')

    def test_role_cannot_be_relabelled_as_a_private_kitsu_node_type(self) -> None:
        value = self.status_value()
        value["role"] = "kitsu"
        with self.assertRaisesRegex(ContractError, "standard MeshCore client role"):
            parse_device_line(record("KITSU_MESH", value))

    def test_kitsu_brand_marker_is_fixed_and_tx_policy_is_coherent(self) -> None:
        value = self.status_value()
        value["marker"] = "cat"
        with self.assertRaisesRegex(ContractError, "brand marker"):
            parse_device_line(record("KITSU_MESH", value))

        value = self.status_value()
        value.update({"tx_policy": "locked", "tx_unlocked": True})
        with self.assertRaisesRegex(ContractError, "locked TX policy"):
            parse_device_line(record("KITSU_MESH", value))

        value = self.status_value()
        value.update({"tx_unlocked": False, "tx_ready": False})
        with self.assertRaisesRegex(ContractError, "queued adverts"):
            parse_device_line(record("KITSU_MESH", value))

    def test_result_distinguishes_nearby_and_mesh_introductions(self) -> None:
        for action in ("introduce_nearby", "introduce_mesh"):
            with self.subTest(action=action):
                parsed = parse_device_line(
                    record(
                        "KITSU_MESH_RESULT",
                        {
                            "protocol": 1,
                            "action": action,
                            "status": "queued",
                            "error": None,
                            "future_field": 7,
                        },
                    )
                )
                self.assertEqual(parsed, MeshResult(action, "queued", None))

    def test_rejected_result_requires_a_safe_machine_error(self) -> None:
        parsed = parse_device_line(
            record(
                "KITSU_MESH_RESULT",
                {
                    "protocol": 1,
                    "action": "config",
                    "status": "rejected",
                    "error": "unsupported_profile",
                },
            )
        )
        self.assertEqual(parsed, MeshResult("config", "rejected", "unsupported_profile"))
        with self.assertRaises(ContractError):
            parse_device_line(
                record(
                    "KITSU_MESH_RESULT",
                    {"protocol": 1, "action": "config", "status": "rejected", "error": "bad\nline"},
                )
            )


class MapBoundaryTest(unittest.TestCase):
    def test_ready_package_is_signed_material_for_the_phone_uploader(self) -> None:
        parsed = parse_device_line(
            record(
                "KITSU_MAP_PUBLISH",
                {
                    "protocol": 1,
                    "status": "ready",
                    "error": None,
                    "uploader": "phone",
                    "firmware_upload": False,
                    "advert_hex": ADVERT_HEX,
                    "location": {
                        "mode": "current_once",
                        "lat_e6": POSITION.latitude_e6,
                        "lon_e6": POSITION.longitude_e6,
                    },
                },
            )
        )
        self.assertIsInstance(parsed, MapPublishPackage)
        assert isinstance(parsed, MapPublishPackage)
        self.assertEqual(parsed.advert_hex, ADVERT_HEX)
        self.assertEqual(parsed.location, MeshLocation(LocationMode.CURRENT_ONCE, POSITION))
        self.assertFalse(parsed.firmware_upload)

    def test_hidden_location_rejection_leaks_neither_position_nor_advert(self) -> None:
        parsed = parse_device_line(
            record(
                "KITSU_MAP_PUBLISH",
                {
                    "protocol": 1,
                    "status": "rejected",
                    "error": "location_hidden",
                    "uploader": "phone",
                    "firmware_upload": False,
                    "advert_hex": None,
                    "location": None,
                },
            )
        )
        self.assertEqual(
            parsed,
            MapPublishPackage("rejected", "location_hidden", "phone", False, None, None),
        )

    def test_firmware_can_never_claim_it_uploaded_or_select_an_endpoint(self) -> None:
        value = {
            "protocol": 1,
            "status": "ready",
            "error": None,
            "uploader": "firmware",
            "firmware_upload": True,
            "advert_hex": ADVERT_HEX,
            "location": {"mode": "fixed", "lat_e6": 1, "lon_e6": 2},
            "url": "https://example.invalid/",
            "token": "must-not-be-used",
        }
        with self.assertRaisesRegex(ContractError, "phone-side"):
            parse_device_line(record("KITSU_MAP_PUBLISH", value))

    def test_map_package_rejects_noncanonical_or_unbounded_advert_material(self) -> None:
        base = {
            "protocol": 1,
            "status": "ready",
            "error": None,
            "uploader": "phone",
            "firmware_upload": False,
            "location": {"mode": "fixed", "lat_e6": 1, "lon_e6": 2},
        }
        for advert in ("abc", "AAZ0", "AA" * 256):
            with self.subTest(advert_length=len(advert)):
                with self.assertRaises(ContractError):
                    parse_device_line(record("KITSU_MAP_PUBLISH", {**base, "advert_hex": advert}))


class OneShotTransmitAuthorizationTest(unittest.TestCase):
    def test_exact_action_approval_cannot_be_inherited_from_session_gate(self) -> None:
        for method in ("sendDirectTextOnce(", "sendChannelTextOnce("):
            with self.subTest(method=method):
                body = cpp_function(TRANSPORT_SOURCE, method)
                approval = "if (!explicitUserApproval) return TransportStatus::TxLocked;"
                self.assertIn(approval, body)
                self.assertIn("const bool sessionAllowed", body)
                self.assertLess(body.index(approval), body.index("const bool sessionAllowed"))

        arm = cpp_function(TRANSPORT_SOURCE, "bool armOneShot(")
        self.assertIn("requested.txPolicy != TxPolicy::ExplicitSession", arm)
        self.assertIn("settings_->txPolicy != TxPolicy::ExplicitSession", arm)
        self.assertIn("!sameRadioProfile(requested.radio, settings_->radio)", arm)

    def test_locked_mode_permit_is_consumed_before_radio_start(self) -> None:
        body = cpp_function(TRANSPORT_SOURCE, "bool startSendRaw(")
        self.assertIn("if (!sessionAllowed) revokeOneShot();", body)
        self.assertIn("CustomSX1262Wrapper::startSendRaw(bytes, length)", body)
        self.assertLess(
            body.index("if (!sessionAllowed) revokeOneShot();"),
            body.index("CustomSX1262Wrapper::startSendRaw(bytes, length)"),
        )

    def test_authenticated_text_reply_is_scoped_and_rate_limited(self) -> None:
        admission = cpp_function(TRANSPORT_SOURCE, "bool armAuthenticatedReply(")
        self.assertIn("requested.txPolicy != TxPolicy::ExplicitSession", admission)
        self.assertIn("settings_->txPolicy != TxPolicy::ExplicitSession", admission)
        self.assertIn("!sameRadioProfile(requested.radio, settings_->radio)", admission)
        self.assertIn("!takeProtocolReplyToken()", admission)

        callback = cpp_function(TRANSPORT_SOURCE, "void onPeerDataRecv(")
        authenticated = callback.index("decodeDirectTextPayload(")
        reply = callback.index("authorizeAuthenticatedReply(path)")
        self.assertLess(authenticated, reply)

        binding = cpp_function(
            TRANSPORT_SOURCE, "bool authorizeAuthenticatedReply(::mesh::Packet* packet)"
        )
        self.assertIn("currentOutboundPacket()", binding)
        self.assertIn("_mgr->getOutboundTotal() != 0", binding)
        self.assertIn("driver_->armAuthenticatedReply(*settings_)", binding)

    def test_protocol_reply_limiter_has_burst_and_refill_bounds(self) -> None:
        self.assertIn("kProtocolReplyBurst = 8U", TRANSPORT_SOURCE)
        self.assertIn("kProtocolReplyRefillMs = 10000UL", TRANSPORT_SOURCE)
        limiter = cpp_function(TRANSPORT_SOURCE, "bool takeProtocolReplyToken(")
        self.assertIn("if (protocolReplyTokens_ == 0U) return false;", limiter)
        self.assertIn("--protocolReplyTokens_;", limiter)


if __name__ == "__main__":
    unittest.main(verbosity=2)
