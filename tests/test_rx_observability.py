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
IRQ_POLL = (ROOT / "src" / "kitsu_radio_irq_poll.h").read_text(
    encoding="utf-8"
)
CUSTOM_SX1262 = (
    ROOT
    / "lib"
    / "MeshCore"
    / "src"
    / "helpers"
    / "radiolib"
    / "CustomSX1262.h"
).read_text(encoding="utf-8")
WRAPPER_HEADER = (
    ROOT
    / "lib"
    / "MeshCore"
    / "src"
    / "helpers"
    / "radiolib"
    / "RadioLibWrappers.h"
).read_text(encoding="utf-8")
WRAPPER_SOURCE = (
    ROOT
    / "lib"
    / "MeshCore"
    / "src"
    / "helpers"
    / "radiolib"
    / "RadioLibWrappers.cpp"
).read_text(encoding="utf-8")
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
CI = (ROOT / ".github" / "workflows" / "platform-ci.yml").read_text(
    encoding="utf-8"
)
UPSTREAM = (ROOT / "lib" / "MeshCore" / "UPSTREAM.md").read_text(
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


class RxObservabilitySourceTests(unittest.TestCase):
    def test_version_and_ci_identity_are_current(self) -> None:
        self.assertIn('FIRMWARE_VERSION[] = "0.20.1"', MAIN)
        self.assertIn("tests.test_rx_observability", CI)

    def test_vendor_patch_documents_bounded_non_clearing_sampling(self) -> None:
        self.assertIn("Local RX observability patch", UPSTREAM)
        self.assertIn("non-clearing GetIrqStatus", UPSTREAM)
        self.assertIn("no more than 10 Hz", UPSTREAM)
        self.assertIn("No IRQ is cleared", UPSTREAM)

    def test_dio_poll_distinguishes_poll_high_edge_and_callback(self) -> None:
        for field in ("polls", "highPolls", "highEdges", "callbacks"):
            self.assertIn(f"uint32_t {field}", IRQ_POLL)
        poll = cpp_function(IRQ_POLL, "bool poll(bool asserted)")
        self.assertIn("incrementSaturating(diagnostics_.polls)", poll)
        self.assertIn("incrementSaturating(diagnostics_.highPolls)", poll)
        self.assertIn("!wasAsserted_", poll)
        self.assertIn("incrementSaturating(diagnostics_.highEdges)", poll)
        self.assertLess(
            poll.index("callback();"),
            poll.index("incrementSaturating(diagnostics_.callbacks)"),
        )
        saturating = cpp_function(
            IRQ_POLL, "static void incrementSaturating(uint32_t& value)"
        )
        self.assertIn("value != UINT32_MAX", saturating)

    def test_irq_snapshots_are_non_clearing_and_bounded(self) -> None:
        observe = cpp_function(
            CUSTOM_SX1262, "uint32_t observeIrqFlags(bool dioAsserted"
        )
        self.assertIn("getIrqFlags()", observe)
        self.assertNotIn("clearIrq", observe)
        receiving = cpp_function(CUSTOM_SX1262, "bool isReceiving()")
        self.assertIn("observeIrqFlags()", receiving)
        self.assertNotIn("uint32_t irq = getIrqFlags()", receiving)

        driver_loop = cpp_function(TRANSPORT, "void loop() override")
        self.assertLess(
            driver_loop.index("pollRadioDio1WithDiagnostics()"),
            driver_loop.index("CustomSX1262Wrapper::loop()"),
        )
        dio_capture = cpp_function(
            TRANSPORT, "bool pollRadioDio1WithDiagnostics()"
        )
        self.assertIn("physical_->observeIrqFlags(true)", dio_capture)
        self.assertIn("kStuckDioIrqSampleIntervalMs", dio_capture)
        low_rate = cpp_function(
            TRANSPORT, "void serviceLowRateIrqObservation(bool dioAsserted)"
        )
        self.assertIn("kLowRateIrqSampleIntervalMs", low_rate)
        self.assertIn("irqObservationWindowOpen(now)", low_rate)
        self.assertIn("physical_->observeIrqFlags(false, true)", low_rate)
        self.assertIn(
            "static constexpr uint32_t kIrqObservationWindowMs = 120000UL",
            TRANSPORT,
        )
        self.assertIn(
            "static constexpr uint32_t kLowRateIrqSampleIntervalMs = 100UL",
            TRANSPORT,
        )

    def test_irq_bits_are_counted_as_observations(self) -> None:
        record = cpp_function(
            CUSTOM_SX1262, "void recordIrqFlags(uint32_t irq"
        )
        for flag in (
            "RADIOLIB_SX126X_IRQ_RX_DONE",
            "RADIOLIB_SX126X_IRQ_CRC_ERR",
            "RADIOLIB_SX126X_IRQ_HEADER_ERR",
            "RADIOLIB_SX126X_IRQ_TIMEOUT",
            "RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED",
            "RADIOLIB_SX126X_IRQ_HEADER_VALID",
            "RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID",
        ):
            self.assertIn(flag, record)
        self.assertNotIn("clearIrq", record)
        self.assertIn("recordBitObservations(_irqDiagnostics.dio, irq)", record)
        self.assertIn(
            "recordBitObservations(_irqDiagnostics.lowRate, irq)", record
        )

    def test_radiolib_classifies_each_receive_stage(self) -> None:
        self.assertIn("n_recv = n_sent = n_recv_errors = 0", WRAPPER_HEADER)
        recv = cpp_function(WRAPPER_SOURCE, "int RadioLibWrapper::recvRaw(")
        for field in (
            "recvRawAttempts",
            "interruptReadyAttempts",
            "packetLengthSamples",
            "packetLengthZero",
            "readDataAttempts",
            "successfulReads",
            "readDataErrors",
            "rxRestartAttempts",
            "rxRestartSuccesses",
            "rxRestartErrors",
            "lastRxRestartResult",
        ):
            self.assertIn(f"_receiveDiagnostics.{field}", recv)
        self.assertLess(
            recv.index("_radio->getPacketLength()"),
            recv.index("_radio->readData(bytes, len)"),
        )
        self.assertLess(
            recv.index("_radio->readData(bytes, len)"),
            recv.index("_radio->startReceive()"),
        )

    def test_short_successful_physical_frames_are_not_conflated(self) -> None:
        gated_recv = cpp_function(
            TRANSPORT, "int recvRaw(uint8_t* bytes, int capacity) override"
        )
        self.assertEqual(gated_recv.count("recordShortFrameRejected(length)"), 2)
        record = cpp_function(TRANSPORT, "void recordShortFrameRejected(")
        self.assertIn("incrementSaturating(shortFrameRejected_)", record)
        self.assertIn("lastShortFrameLength_", record)

    def test_authenticated_and_serial_status_share_plaintext_free_fields(self) -> None:
        append = cpp_function(MAIN, "void appendReceiveObservability(")
        for key in (
            "dio1_high_edges",
            "irq_dio_asserted_samples",
            "irq_low_rate_samples",
            "last_irq_flags",
            "last_dio_irq_flags",
            "last_low_rate_irq_flags",
            "irq_rx_done_observations",
            "irq_crc_error_observations",
            "irq_header_error_observations",
            "irq_timeout_observations",
            "irq_preamble_observations",
            "irq_header_valid_observations",
            "dio_irq_rx_done_observations",
            "dio_irq_crc_error_observations",
            "dio_irq_header_error_observations",
            "low_rate_irq_rx_done_observations",
            "low_rate_irq_crc_error_observations",
            "low_rate_irq_header_error_observations",
            "low_rate_irq_preamble_observations",
            "low_rate_irq_header_valid_observations",
            "recv_interrupt_ready_attempts",
            "recv_packet_length_zero",
            "recv_read_data_attempts",
            "recv_successful_reads",
            "recv_read_data_errors",
            "last_recv_rx_restart_result",
            "recv_rx_restart_errors",
            "short_frame_rejected",
            "last_short_frame_length",
            "max_mesh_loop_gap_ms",
        ):
            self.assertIn(key, append)
        for forbidden in ("payload", "message", "text", "packet_hex"):
            self.assertNotIn(forbidden, append.lower())
        self.assertEqual(MAIN.count("appendReceiveObservability(output,"), 2)

    def test_max_mesh_loop_gap_is_entry_to_entry_and_wrap_safe(self) -> None:
        loop = cpp_function(TRANSPORT, "void KitsuMeshTransport::loop()")
        self.assertIn("now - impl_->lastMeshLoopAtMs", loop)
        self.assertIn("gap > impl_->maxMeshLoopGapMs", loop)
        snapshot = cpp_function(
            TRANSPORT, "bool KitsuMeshTransport::repeatDiagnostics("
        )
        self.assertIn(
            "output.maxMeshLoopGapMs = impl_->maxMeshLoopGapMs", snapshot
        )

    def test_public_diagnostic_contract_contains_no_content_buffers(self) -> None:
        start = TRANSPORT_HEADER.index("uint32_t dio1Polls")
        end = TRANSPORT_HEADER.index("uint32_t scopedFloodTxDoneFrames", start)
        contract = TRANSPORT_HEADER[start:end].lower()
        for forbidden in ("payload", "message", "plaintext", "text[", "char "):
            self.assertNotIn(forbidden, contract)


if __name__ == "__main__":
    unittest.main(verbosity=2)
