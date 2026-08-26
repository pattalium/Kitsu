from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)
TRANSPORT_HEADER = (ROOT / "src" / "kitsu_mesh_transport.h").read_text(
    encoding="utf-8"
)
POLICY = (ROOT / "src" / "kitsu_rx_rearm_policy.h").read_text(
    encoding="utf-8"
)
WRAPPER_HEADER = (
    ROOT / "lib" / "MeshCore" / "src" / "helpers" / "radiolib"
    / "RadioLibWrappers.h"
).read_text(encoding="utf-8")
WRAPPER_SOURCE = (
    ROOT / "lib" / "MeshCore" / "src" / "helpers" / "radiolib"
    / "RadioLibWrappers.cpp"
).read_text(encoding="utf-8")
UPSTREAM = (ROOT / "lib" / "MeshCore" / "UPSTREAM.md").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
CI = (ROOT / ".github" / "workflows" / "platform-ci.yml").read_text(
    encoding="utf-8"
)


def cpp_function(source: str, signature: str) -> str:
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


class Sx126xStatusReaderSourceTests(unittest.TestCase):
    def test_status_reader_is_guarded_for_non_sx126x_builds(self) -> None:
        self.assertIn("#if !RADIOLIB_EXCLUDE_SX126X", WRAPPER_HEADER)
        self.assertIn("#if !RADIOLIB_EXCLUDE_SX126X", WRAPPER_SOURCE)

    def test_status_reader_uses_exact_tracked_module_transaction(self) -> None:
        reader = cpp_function(
            WRAPPER_SOURCE,
            "bool RadioLibWrapper::readSx126xStatus(",
        )
        for guard in (
            "config.stream",
            "config.statusPos != 1U",
            "RADIOLIB_MODULE_SPI_WIDTH_CMD] != Module::BITS_8",
            "RADIOLIB_MODULE_SPI_WIDTH_STATUS] != Module::BITS_8",
            "RADIOLIB_SX126X_CMD_GET_STATUS",
            "RADIOLIB_SX126X_CMD_NOP",
            "config.parseStatusCb == nullptr",
        ):
            self.assertIn(guard, reader)
        self.assertIn("savedStatusWidth", reader)
        self.assertIn(
            "config.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS] = Module::BITS_0",
            reader,
        )
        normalized = " ".join(reader.split())
        self.assertIn(
            "RADIOLIB_SX126X_CMD_GET_STATUS, &received, 1U, true, false",
            normalized,
        )
        restore = (
            "config.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS] = "
            "savedStatusWidth"
        )
        self.assertIn(restore, normalized)
        self.assertLess(normalized.index(restore), normalized.index("result !="))
        self.assertIn("received == 0x00U", reader)
        self.assertIn("received == 0xFFU", reader)
        for forbidden in ("spiTransfer", "digitalWrite", "getCs", "setCs"):
            self.assertNotIn(forbidden, reader)

    def test_original_callers_keep_void_start_entry_point(self) -> None:
        self.assertIn("int16_t startRecvWithStatus();", WRAPPER_HEADER)
        legacy = cpp_function(WRAPPER_SOURCE, "void RadioLibWrapper::startRecv()")
        self.assertIn("(void)startRecvWithStatus()", legacy)
        status = cpp_function(
            WRAPPER_SOURCE, "int16_t RadioLibWrapper::startRecvWithStatus()"
        )
        self.assertIn("state = STATE_IDLE", status)
        self.assertIn("return err", status)

    def test_single_owner_invariant_and_patch_are_tracked(self) -> None:
        self.assertIn("exactly `C0 00`", UPSTREAM)
        self.assertIn("single-owner invariant", UPSTREAM)
        self.assertIn("restores the original width", UPSTREAM)


class RxRearmPolicySourceTests(unittest.TestCase):
    def test_policy_requires_dio_low_and_positive_failure_evidence(self) -> None:
        retry = cpp_function(POLICY, "constexpr bool shouldRetryRxRearm(")
        self.assertIn("return dio1Low &&", retry)
        self.assertIn("evidence.startCode != 0", retry)
        self.assertIn("!evidence.softwareRx", retry)
        self.assertIn("evidence.chipStatusAvailable", retry)
        self.assertNotIn("!evidence.chipStatusAvailable", retry)

    def test_physical_confirmation_is_fully_conjunctive(self) -> None:
        proof = cpp_function(POLICY, "constexpr bool rxRearmPhysicallyConfirmed(")
        for required in (
            "evidence.startAttempted",
            "evidence.startCode == 0",
            "evidence.softwareRx",
            "evidence.chipStatusAvailable",
            "sx126xStatusIsRx(evidence.chipStatus)",
        ):
            self.assertIn(required, proof)
        self.assertIn("kSx126xChipModeRx = 0x50U", POLICY)

    def test_runtime_has_one_guarded_retry_and_no_broken_get_status(self) -> None:
        resume = cpp_function(TRANSPORT, "bool resumeReceiveNow()")
        self.assertEqual(resume.count("shouldRetryRxRearm("), 1)
        self.assertEqual(resume.count("startAndProbeReceive()"), 2)
        self.assertIn("digitalRead(kLoraDio1) == LOW", resume)
        self.assertNotIn("physical_->getStatus()", resume)
        self.assertIn("rxRearmPhysicallyConfirmed(evidence)", resume)

    def test_acceptance_exposes_last_and_current_physical_state(self) -> None:
        for field in (
            "rxRearmAttempts",
            "rxRearmRetries",
            "rxRearmFailures",
            "lastRxStartAttempts",
            "lastRxStartCodeAvailable",
            "lastRxStartSoftwareState",
            "lastRxChipStatusAvailable",
            "lastTxDoneToRxConfirmedMicrosAvailable",
            "currentRxSoftwareState",
            "currentRxChipStatusAvailable",
        ):
            self.assertIn(field, TRANSPORT_HEADER)
        for key in (
            "rx_rearm_attempts",
            "rx_rearm_retries",
            "rx_rearm_failures",
            "last_rx_start_attempts",
            "last_rx_start_code",
            "last_rx_start_software_state",
            "last_rx_chip_status_available",
            "last_rx_chip_status",
            "last_rx_chip_mode",
            "last_tx_done_to_rx_confirmed_us",
            "current_rx_software_state",
            "current_rx_chip_status_available",
            "current_rx_chip_status",
            "current_rx_chip_mode",
        ):
            self.assertIn(key, MAIN)
        snapshot = cpp_function(
            TRANSPORT,
            "bool KitsuMeshTransport::repeatDiagnostics(",
        )
        self.assertIn("if (impl_->active)", snapshot)
        self.assertIn("impl_->driver.currentReceiveSnapshot(", snapshot)

    def test_version_and_ci_are_pinned(self) -> None:
        self.assertIn('FIRMWARE_VERSION[] = "0.17.4"', MAIN)
        self.assertIn("tests.test_rx_rearm", CI)
        self.assertIn("test_kitsu_rx_rearm.cmd", CI)


if __name__ == "__main__":
    unittest.main(verbosity=2)
