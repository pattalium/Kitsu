import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
SESSION = (ROOT / "src" / "kitsu_ble_session.cpp").read_text(encoding="utf-8")


class CaretakerPairingIntegrationTest(unittest.TestCase):
    def test_role_is_selected_locally_and_forwarded_to_the_session(self):
        actions = MAIN.split("enum class ConnectionAction", 1)[1].split("};", 1)[0]
        self.assertIn("Bluetooth = 0", actions)
        self.assertIn("PairCaretaker", actions)

        bridge = MAIN.split("class FirmwareBleBridge", 1)[1].split(
            "FirmwareBleBridge companionBle", 1
        )[0]
        open_pairing = bridge.split("bool openPairing(", 1)[1].split(
            "void closePairing", 1
        )[0]
        self.assertIn("ControllerRole role", open_pairing)
        self.assertIn("ControllerRole::Owner", open_pairing)
        self.assertIn("ControllerRole::Caretaker", open_pairing)
        self.assertIn("kBlePairingWindowMaximumMs, now, role", open_pairing)

        execute = MAIN.split("void executeConnectionAction()", 1)[1].split(
            "void executeGameMenuItem", 1
        )[0]
        self.assertIn("ConnectionAction::Bluetooth", execute)
        self.assertIn("ControllerRole::Owner", execute)
        self.assertIn("ConnectionAction::PairCaretaker", execute)
        self.assertIn("ControllerRole::Caretaker", execute)

    def test_device_surfaces_name_caretaker_during_confirmation_and_result(self):
        connect = MAIN.split("void renderConnect()", 1)[1].split(
            "void renderInbox", 1
        )[0]
        self.assertIn('uiTextCentered("PAIR", 28)', connect)
        self.assertIn('uiTextCentered("CARETAKER", 39)', connect)

        pair_screen = MAIN.split("void renderPairPhone()", 1)[1].split(
            "void renderControllerManager", 1
        )[0]
        self.assertIn('uiTextCentered("PAIR", 2)', pair_screen)
        self.assertIn('uiTextCentered("CARETAKER", 11)', pair_screen)
        self.assertIn("session.physicalConfirmationPending", pair_screen)
        self.assertIn('caretaker ? "CARETAKER" : "PHONE READY"', pair_screen)
        self.assertIn("session.pairingCompleted", pair_screen)
        self.assertIn('caretaker ? "CARETAKER" : "PHONE"', pair_screen)
        self.assertIn('uiTextCentered("PAIRED", 58, 2)', pair_screen)

    def test_remote_request_has_no_role_selector_and_v2_commit_only_echoes_it(self):
        pair_request = SESSION.split("bool KitsuBleSession::handlePairRequest", 1)[
            1
        ].split("bool KitsuBleSession::makePendingPairingProof", 1)[0]
        schema = pair_request.split("static const char* const schema[]", 1)[1].split(
            ";", 1
        )[0]
        self.assertNotIn('"role"', schema)
        self.assertIn("pairingVersionForRole(pairingWindowRole_)", pair_request)
        self.assertIn("pendingPairingRole_ = pairingWindowRole_", pair_request)

        pair_commit = SESSION.split("bool KitsuBleSession::handlePairCommit", 1)[
            1
        ].split("bool KitsuBleSession::sendAuthenticated", 1)[0]
        self.assertIn('stringFieldEquals(fields, count, "role", "caretaker")', pair_commit)
        self.assertIn("makePendingPairingProof(\"client\"", pair_commit)
        self.assertIn("makePendingPairingProof(\"ok\"", pair_commit)
        self.assertIn("pendingPairingRole_", pair_commit)
        self.assertIn(r'\"role\":\"caretaker\"', pair_commit)


if __name__ == "__main__":
    unittest.main()
