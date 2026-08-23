"""Executable tests for Kitsu's v0.9 custom-phone Mesh chat contract."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from urllib.parse import quote_plus


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from kitsu_chat_commands import (  # noqa: E402
    CHANNEL_CAPACITY,
    CHANNEL_TEXT_MAX_BYTES,
    CONTACT_CAPACITY,
    DISCOVERED_NAME_MAX_BYTES,
    DIRECT_TEXT_MAX_BYTES,
    INBOX_CAPACITY,
    MAX_INPUT_BYTES,
    PUBLIC_CHANNEL_SECRET_HEX,
    ChatEvent,
    ChatResult,
    ChatStatus,
    ChannelEnd,
    ChannelRecord,
    ContactEnd,
    ContactRecord,
    ContactRole,
    ContractError,
    MessageEnd,
    MessageRecord,
    chat_channel_clear_command,
    chat_channel_command_from_uri,
    chat_channel_set_command,
    chat_channels_command,
    chat_contact_command_from_uri,
    chat_contact_drop_command,
    chat_contact_set_command,
    chat_contacts_command,
    chat_hashtag_channel_command,
    chat_inbox_command,
    chat_reset_command,
    chat_send_channel_command,
    chat_send_direct_command,
    chat_status_command,
    parse_device_line,
)


PUBLIC_KEY = "A1B2C3D4E5F6" + "07" * 26
CONTACT_ID = PUBLIC_KEY[:12]
PRIVATE_SECRET = "9CD8FCF22A47333B591D96A2B848B73F"
SESSION = "29C3F001"


def record(prefix: str, value: dict) -> str:
    return prefix + " " + json.dumps(value, ensure_ascii=False, separators=(",", ":"))


class CommandContractTest(unittest.TestCase):
    def test_queries_are_canonical_and_bounded(self) -> None:
        commands = (
            chat_status_command(),
            chat_reset_command(),
            chat_contacts_command(),
            chat_channels_command(),
            chat_inbox_command(),
            chat_inbox_command(0xFFFFFFFF),
        )
        self.assertEqual(
            commands,
            (
                "chat status",
                "chat reset",
                "chat contacts",
                "chat channels",
                "chat inbox",
                "chat inbox 4294967295",
            ),
        )
        for command in commands:
            self.assertLessEqual(len(command.encode("utf-8")), MAX_INPUT_BYTES)

    def test_reset_result_is_owner_recovery_without_identifiers(self) -> None:
        result = {
            "protocol": 1,
            "action": "reset",
            "status": "ok",
            "error": None,
            "message_id": None,
            "route": None,
        }
        self.assertEqual(
            parse_device_line(record("KITSU_CHAT_RESULT", result)),
            ChatResult("reset", "ok", None, None, None),
        )

    def test_contact_provisioning_uses_full_key_and_preserves_name_case(self) -> None:
        command = chat_contact_set_command(PUBLIC_KEY.lower(), "client", "Alice McFox")
        self.assertEqual(
            command,
            f"chat contact set {PUBLIC_KEY} client Alice McFox",
        )
        self.assertEqual(
            chat_contact_drop_command(PUBLIC_KEY.lower()),
            f"chat contact drop {PUBLIC_KEY}",
        )
        with self.assertRaisesRegex(ContractError, "all zero"):
            chat_contact_set_command("00" * 32, ContactRole.CLIENT, "Alice")
        with self.assertRaisesRegex(ContractError, "role"):
            chat_contact_set_command(PUBLIC_KEY, "kitsu", "Alice")

    def test_explicit_names_are_capped_at_31_utf8_bytes(self) -> None:
        contact_name = "C" * 31
        channel_name = "#" + ("C" * 30)
        self.assertTrue(
            chat_contact_set_command(PUBLIC_KEY, ContactRole.CLIENT, contact_name).endswith(
                contact_name
            )
        )
        self.assertTrue(
            chat_channel_set_command(1, PRIVATE_SECRET, channel_name).endswith(
                channel_name
            )
        )

        with self.assertRaisesRegex(ContractError, "31"):
            chat_contact_set_command(PUBLIC_KEY, ContactRole.CLIENT, "C" * 32)
        with self.assertRaisesRegex(ContractError, "31"):
            chat_channel_set_command(1, PRIVATE_SECRET, "C" * 32)

    def test_current_official_contact_qr_maps_to_canonical_command(self) -> None:
        uri = (
            "meshcore://contact/add?name="
            + quote_plus("Alice McFox")
            + f"&public_key={PUBLIC_KEY.lower()}&type=1"
        )
        self.assertEqual(
            chat_contact_command_from_uri(uri),
            f"chat contact set {PUBLIC_KEY} client Alice McFox",
        )
        with self.assertRaises(ContractError):
            chat_contact_command_from_uri("meshcore://DEADBEEF")

    def test_channel_provisioning_never_needs_more_than_one_line(self) -> None:
        command = chat_channel_set_command(3, PRIVATE_SECRET.lower(), "Team Alpha")
        self.assertEqual(
            command,
            f"chat channel set 3 {PRIVATE_SECRET} Team Alpha",
        )
        self.assertEqual(
            chat_channel_set_command(
                3, PRIVATE_SECRET.lower(), "Team Alpha", "EU"
            ),
            f"chat channel set 3 region_scope=EU {PRIVATE_SECRET} Team Alpha",
        )
        for invalid_scope in ("eu", "US", "", "#EU"):
            with self.subTest(region_scope=invalid_scope):
                with self.assertRaisesRegex(ContractError, "EU or absent"):
                    chat_channel_set_command(
                        3, PRIVATE_SECRET, "Team Alpha", invalid_scope
                    )
        self.assertLessEqual(len(command.encode("utf-8")), MAX_INPUT_BYTES)
        self.assertEqual(chat_channel_clear_command(1), "chat channel clear 1")
        for invalid in (0, 4, -1):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ContractError):
                    chat_channel_set_command(invalid, PRIVATE_SECRET, "Team")

    def test_official_channel_qr_and_hashtag_derivation(self) -> None:
        uri = (
            "meshcore://channel/add?name=Team+Alpha"
            f"&secret={PRIVATE_SECRET.lower()}&region_scope=EU"
        )
        self.assertEqual(
            chat_channel_command_from_uri(uri, 1),
            f"chat channel set 1 region_scope=EU {PRIVATE_SECRET} Team Alpha",
        )
        self.assertEqual(
            chat_channel_command_from_uri(
                "meshcore://channel/add?name=Team+Alpha"
                f"&secret={PRIVATE_SECRET.lower()}",
                1,
            ),
            f"chat channel set 1 {PRIVATE_SECRET} Team Alpha",
        )
        # Decoded scope comparison remains exact/case-sensitive. Absent scope
        # is legacy; only explicit EU opts this channel into #EU routing.
        for non_eu_scope in ("US", "eu", "", "EU%00", "%EF%BC%A5%EF%BC%B5"):
            with self.subTest(region_scope=non_eu_scope):
                with self.assertRaises(ContractError):
                    chat_channel_command_from_uri(
                        "meshcore://channel/add?name=Team+Alpha"
                        f"&secret={PRIVATE_SECRET.lower()}"
                        f"&region_scope={non_eu_scope}",
                        1,
                    )
        hashtag = chat_hashtag_channel_command("#test", 2)
        self.assertEqual(
            hashtag,
            "chat channel set 2 9CD8FCF22A47333B591D96A2B848B73F #test",
        )
        self.assertEqual(
            PUBLIC_CHANNEL_SECRET_HEX,
            "8B3387E9C5CDEA6AC9E5EDBAA115CD72",
        )

    def test_outbound_messages_are_one_line_and_preserve_utf8(self) -> None:
        text = "Meet Kitsu 🦊 at 20:30"
        self.assertEqual(
            chat_send_direct_command(CONTACT_ID.lower(), text),
            f"chat send dm {CONTACT_ID} {text}",
        )
        self.assertEqual(
            chat_send_channel_command(0, text),
            f"chat send ch 0 {text}",
        )
        max_direct = chat_send_direct_command(CONTACT_ID, "D" * DIRECT_TEXT_MAX_BYTES)
        max_channel = chat_send_channel_command(0, "C" * CHANNEL_TEXT_MAX_BYTES)
        self.assertLessEqual(len(max_direct.encode("utf-8")), MAX_INPUT_BYTES)
        self.assertLessEqual(len(max_channel.encode("utf-8")), MAX_INPUT_BYTES)

    def test_text_limits_count_utf8_bytes_not_code_points(self) -> None:
        self.assertEqual(len(("🦊" * 32).encode("utf-8")), 128)
        chat_send_direct_command(CONTACT_ID, "🦊" * 32)
        chat_send_channel_command(0, "🦊" * 32)
        for builder in (
            lambda: chat_send_direct_command(CONTACT_ID, "🦊" * 33),
            lambda: chat_send_channel_command(0, "🦊" * 33),
            lambda: chat_send_direct_command(CONTACT_ID, "x\nsecret"),
            lambda: chat_send_channel_command(0, "x\0secret"),
        ):
            with self.assertRaises(ContractError):
                builder()


class RecordContractTest(unittest.TestCase):
    def status_value(self) -> dict:
        return {
            "protocol": 1,
            "available": True,
            "meshcore_version": "1.17.1",
            "session": SESSION,
            "contacts": 2,
            "contact_capacity": CONTACT_CAPACITY,
            "channels": 1,
            "channel_capacity": CHANNEL_CAPACITY,
            "messages": 3,
            "inbox_capacity": INBOX_CAPACITY,
            "dropped_messages": 0,
            "time_valid": True,
            "tx_unlocked": False,
            "tx_ready": False,
            "direct_text_max_bytes": DIRECT_TEXT_MAX_BYTES,
            "channel_text_max_bytes": CHANNEL_TEXT_MAX_BYTES,
        }

    def test_chat_status_matches_real_transport_bounds(self) -> None:
        parsed = parse_device_line(record("KITSU_CHAT", self.status_value()))
        self.assertIsInstance(parsed, ChatStatus)
        assert isinstance(parsed, ChatStatus)
        self.assertEqual(parsed.contact_capacity, 12)
        self.assertEqual(parsed.channel_capacity, 4)
        self.assertEqual(parsed.inbox_capacity, 24)
        self.assertEqual(parsed.direct_text_max_bytes, 128)
        self.assertEqual(parsed.channel_text_max_bytes, 128)
        self.assertFalse(parsed.tx_unlocked)

        invalid = self.status_value()
        invalid.update({"tx_ready": True, "tx_unlocked": False})
        with self.assertRaisesRegex(ContractError, "volatile gate"):
            parse_device_line(record("KITSU_CHAT", invalid))

    def test_contact_query_is_the_only_routine_record_with_full_public_key(self) -> None:
        value = {
            "protocol": 1,
            "index": 0,
            "id": CONTACT_ID,
            "public_key": PUBLIC_KEY,
            "name": "Alice McFox",
            "role": "client",
            "favorite": False,
            "last_advert": 1_775_638_400,
            "last_heard": 1_775_638_415,
            "route_hint": "direct",
            "dm_capable": True,
        }
        parsed = parse_device_line(record("KITSU_CONTACT", value))
        self.assertEqual(
            parsed,
            ContactRecord(
                0,
                CONTACT_ID,
                PUBLIC_KEY,
                "Alice McFox",
                ContactRole.CLIENT,
                False,
                1_775_638_400,
                1_775_638_415,
                "direct",
                True,
            ),
        )
        self.assertEqual(
            parse_device_line(record("KITSU_CONTACT_END", {"protocol": 1, "count": 1})),
            ContactEnd(1),
        )

        value["id"] = "FFFFFFFFFFFF"
        with self.assertRaisesRegex(ContractError, "first six"):
            parse_device_line(record("KITSU_CONTACT", value))

    def test_discovered_contact_and_inbound_sender_accept_32_utf8_bytes(self) -> None:
        discovered_name = "é" * 16
        self.assertEqual(len(discovered_name.encode("utf-8")), DISCOVERED_NAME_MAX_BYTES)
        contact = {
            "protocol": 1,
            "index": 0,
            "id": CONTACT_ID,
            "public_key": PUBLIC_KEY,
            "name": discovered_name,
            "role": "client",
            "favorite": False,
            "last_advert": 1,
            "last_heard": 1,
            "route_hint": "flood",
            "dm_capable": True,
        }
        parsed_contact = parse_device_line(record("KITSU_CONTACT", contact))
        assert isinstance(parsed_contact, ContactRecord)
        self.assertEqual(parsed_contact.name, discovered_name)

        direct = {
            "protocol": 1,
            "id": 41,
            "direction": "in",
            "kind": "direct",
            "contact": CONTACT_ID,
            "channel": None,
            "sender": discovered_name,
            "timestamp": 1,
            "text": "Hello",
            "state": "received",
            "route": "flood",
            "snr_db": 1.0,
            "authenticated": True,
        }
        parsed_message = parse_device_line(record("KITSU_MESSAGE", direct))
        assert isinstance(parsed_message, MessageRecord)
        self.assertEqual(parsed_message.sender, discovered_name)

        contact["name"] = discovered_name + "x"
        with self.assertRaisesRegex(ContractError, "32"):
            parse_device_line(record("KITSU_CONTACT", contact))
        direct["sender"] = discovered_name + "x"
        with self.assertRaisesRegex(ContractError, "32"):
            parse_device_line(record("KITSU_MESSAGE", direct))

    def test_channel_records_never_contain_a_secret(self) -> None:
        value = {
            "protocol": 1,
            "index": 0,
            "name": "Public",
            "configured": True,
            "public": True,
            "hash": "11",
        }
        parsed = parse_device_line(record("KITSU_CHANNEL", value))
        self.assertEqual(
            parsed, ChannelRecord(0, "Public", True, True, "11", None)
        )
        self.assertNotIn("secret", value)
        self.assertEqual(
            parse_device_line(record("KITSU_CHANNEL_END", {"protocol": 1, "count": 4})),
            ChannelEnd(4),
        )
        value["index"] = 1
        with self.assertRaisesRegex(ContractError, "only channel zero"):
            parse_device_line(record("KITSU_CHANNEL", value))

        value.update(
            {
                "index": 1,
                "name": "Ops",
                "public": False,
                "region_scope": "EU",
            }
        )
        parsed = parse_device_line(record("KITSU_CHANNEL", value))
        assert isinstance(parsed, ChannelRecord)
        self.assertEqual(parsed.region_scope, "EU")
        value["region_scope"] = "eu"
        with self.assertRaisesRegex(ContractError, "unsupported"):
            parse_device_line(record("KITSU_CHANNEL", value))

    def test_direct_messages_are_authenticated_but_channel_senders_are_not(self) -> None:
        direct = {
            "protocol": 1,
            "id": 41,
            "direction": "in",
            "kind": "direct",
            "contact": CONTACT_ID,
            "channel": None,
            "sender": "Alice McFox",
            "timestamp": 1_775_638_400,
            "text": "Hello Kitsu",
            "state": "received",
            "route": "direct",
            "snr_db": 7.25,
            "authenticated": True,
        }
        parsed = parse_device_line(record("KITSU_MESSAGE", direct))
        self.assertIsInstance(parsed, MessageRecord)
        assert isinstance(parsed, MessageRecord)
        self.assertTrue(parsed.authenticated)

        channel = dict(direct)
        channel.update(
            {
                "id": 42,
                "kind": "channel",
                "contact": None,
                "channel": 0,
                "sender": "Alice",
                "text": "Hello Public",
                "route": "flood",
                "authenticated": False,
            }
        )
        parsed = parse_device_line(record("KITSU_MESSAGE", channel))
        assert isinstance(parsed, MessageRecord)
        self.assertFalse(parsed.authenticated)

        channel["authenticated"] = True
        with self.assertRaisesRegex(ContractError, "channel senders"):
            parse_device_line(record("KITSU_MESSAGE", channel))

    def test_inbound_stock_client_text_may_use_full_wire_limit(self) -> None:
        value = {
            "protocol": 1,
            "id": 43,
            "direction": "in",
            "kind": "direct",
            "contact": CONTACT_ID,
            "channel": None,
            "sender": "Alice McFox",
            "timestamp": 1,
            "text": "x" * 160,
            "state": "received",
            "route": "flood",
            "snr_db": None,
            "authenticated": True,
        }
        self.assertIsInstance(
            parse_device_line(record("KITSU_MESSAGE", value)), MessageRecord
        )
        value["text"] += "x"
        with self.assertRaisesRegex(ContractError, "160"):
            parse_device_line(record("KITSU_MESSAGE", value))

    def test_message_end_has_boot_session_and_visible_loss_counter(self) -> None:
        parsed = parse_device_line(
            record(
                "KITSU_MESSAGE_END",
                {
                    "protocol": 1,
                    "count": 2,
                    "newest_id": 42,
                    "dropped": 3,
                    "session": SESSION,
                },
            )
        )
        self.assertEqual(parsed, MessageEnd(2, 42, 3, SESSION))

    def test_outbound_terminal_states_and_sender_null_are_explicit(self) -> None:
        value = {
            "protocol": 1,
            "id": 44,
            "direction": "out",
            "kind": "channel",
            "contact": None,
            "channel": 0,
            "sender": None,
            "timestamp": 1_775_638_420,
            "text": "Hello Public",
            "state": "failed",
            "route": "flood",
            "snr_db": None,
            "authenticated": False,
        }
        parsed = parse_device_line(record("KITSU_MESSAGE", value))
        self.assertIsInstance(parsed, MessageRecord)
        assert isinstance(parsed, MessageRecord)
        self.assertEqual(parsed.state, "failed")
        self.assertIsNone(parsed.sender)
        value["state"] = "cancelled"
        self.assertIsInstance(
            parse_device_line(record("KITSU_MESSAGE", value)), MessageRecord
        )
        value["state"] = "delivered"
        with self.assertRaisesRegex(ContractError, "no delivery"):
            parse_device_line(record("KITSU_MESSAGE", value))

    def test_send_result_and_events_correlate_only_by_local_message_id(self) -> None:
        queued = {
            "protocol": 1,
            "action": "send",
            "status": "queued",
            "error": None,
            "message_id": 41,
            "route": "flood",
        }
        self.assertEqual(
            parse_device_line(record("KITSU_CHAT_RESULT", queued)),
            ChatResult("send", "queued", None, 41, "flood"),
        )
        self.assertNotIn("public_key", queued)
        self.assertNotIn("secret", queued)
        self.assertNotIn("expected_ack", queued)

        tx = {
            "protocol": 1,
            "event": "tx",
            "message_id": 41,
            "state": "sent",
            "round_trip_ms": None,
        }
        delivery = {
            "protocol": 1,
            "event": "delivery",
            "message_id": 41,
            "state": "delivered",
            "round_trip_ms": 2840,
        }
        self.assertEqual(
            parse_device_line(record("KITSU_CHAT_EVENT", tx)),
            ChatEvent("tx", 41, "sent", None),
        )
        self.assertEqual(
            parse_device_line(record("KITSU_CHAT_EVENT", delivery)),
            ChatEvent("delivery", 41, "delivered", 2840),
        )

    def test_recognized_json_is_strict_and_unrelated_lines_are_ignored(self) -> None:
        self.assertIsNone(parse_device_line('KITSU_SYNC {"protocol":1}'))
        self.assertIsNone(parse_device_line("DEBUG KITSU_CHAT something"))
        with self.assertRaises(ContractError):
            parse_device_line('KITSU_CHAT {"protocol":1,"protocol":1}')
        with self.assertRaises(ContractError):
            parse_device_line('KITSU_CHAT {"protocol":1,"x":NaN}')
        with self.assertRaises(ContractError):
            parse_device_line("KITSU_CHAT {")


if __name__ == "__main__":
    unittest.main(verbosity=2)
