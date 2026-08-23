from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = (ROOT / "src" / "kitsu_mesh_transport.cpp").read_text(
    encoding="utf-8"
)
TURNAROUND = (ROOT / "src" / "kitsu_tx_turnaround.h").read_text(
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


class TxTurnaroundSourceTests(unittest.TestCase):
    def test_physical_tx_is_completed_and_rx_rearmed_inside_start(self) -> None:
        start = cpp_function(TRANSPORT, "bool startSendRaw(")
        for required in (
            "txTurnaroundCompletion_.reset()",
            "getEstAirtimeFor(length)",
            "txTurnaroundTimeoutMs(estimatedAirtimeMs)",
            "runSynchronousTxTurnaround(",
            "CustomSX1262Wrapper::startSendRaw(bytes, length)",
            "pollRadioDio1WithDiagnostics()",
            "CustomSX1262Wrapper::isSendComplete()",
            "CustomSX1262Wrapper::onSendFinished()",
            "resumeReceiveNow()",
            "::yield()",
            "TxTurnaroundResult::Completed",
            "TxTurnaroundResult::StartFailed",
            "TxTurnaroundResult::TimedOut",
        ):
            self.assertIn(required, start)

    def test_dispatcher_consumes_only_the_independent_completion_pair(self) -> None:
        complete = cpp_function(TRANSPORT, "bool isSendComplete() override")
        self.assertIn("txTurnaroundCompletion_.takeForDispatcher()", complete)
        self.assertNotIn("CustomSX1262Wrapper::isSendComplete", complete)

        finished = cpp_function(TRANSPORT, "void onSendFinished() override")
        self.assertIn(
            "txTurnaroundCompletion_.consumeReportedFinish()", finished
        )
        self.assertNotIn("CustomSX1262Wrapper::onSendFinished", finished)

    def test_timeout_has_final_irq_poll_and_balanced_cleanup(self) -> None:
        self.assertIn("// One final level-latched DIO1 sample", TURNAROUND)
        final = TURNAROUND.split(
            "// One final level-latched DIO1 sample", 1
        )[1]
        self.assertLess(final.index("pollIrq();"), final.index("finishTx();"))
        self.assertLess(final.index("finishTx();"), final.index("rearmRx();"))
        self.assertIn("completion.reset();", final)

    def test_rx_proof_reads_physical_sx1262_mode_in_task_context(self) -> None:
        resume = cpp_function(TRANSPORT, "bool resumeReceiveNow()")
        for required in (
            "startAndProbeReceive()",
            "readSx126xStatus(*physical_",
            "shouldRetryRxRearm(evidence",
            "digitalRead(kLoraDio1) == LOW",
            "rxRearmPhysicallyConfirmed(evidence)",
        ):
            self.assertIn(required, resume)
        self.assertNotIn("physical_->getStatus()", resume)
        self.assertNotIn("attachInterrupt", resume)

    def test_completed_tx_proof_is_latched_before_dispatcher_log(self) -> None:
        start = cpp_function(TRANSPORT, "bool startSendRaw(")
        self.assertIn("turnaroundRearmPhysicalRxConfirmed_ = resumeReceiveNow()", start)
        self.assertIn("lastCompletedTxPhysicalRxConfirmed_", start)
        log_tx = cpp_function(TRANSPORT, "void logTx(::mesh::Packet* packet")
        self.assertIn("lastCompletedTxRxChipStatus", log_tx)
        self.assertNotIn("resumeReceiveNow()", log_tx)

    def test_acceptance_diagnostics_separate_software_and_physical_rx(self) -> None:
        for required in (
            "physical_rx_confirmed_after_tx",
            "sync_turnaround_completed",
            "sync_turnaround_start_failures",
            "sync_turnaround_timeouts",
            "rx_rearm_attempts",
            "rx_rearm_retries",
            "rx_rearm_failures",
            "last_rx_start_code",
            "last_rx_start_software_state",
            "last_rx_chip_status",
            "last_tx_done_to_start_receive_us",
            "last_tx_done_to_rx_confirmed_us",
            "current_rx_chip_status",
        ):
            self.assertIn(required, MAIN)

    def test_ci_executes_source_and_native_turnaround_gates(self) -> None:
        self.assertIn("tests.test_tx_turnaround", CI)
        self.assertIn("test_kitsu_tx_turnaround.cmd", CI)


if __name__ == "__main__":
    unittest.main(verbosity=2)
