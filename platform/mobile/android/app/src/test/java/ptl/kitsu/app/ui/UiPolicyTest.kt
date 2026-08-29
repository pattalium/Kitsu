package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.blePermissionErrorCode

class UiPolicyTest {
    @Test fun darkIsTheOnlyDefaultAndSystemIsExplicit() {
        assertEquals(KitsuThemePreference.DARK, KitsuThemePreference.fromStorage(null))
        assertEquals(KitsuThemePreference.DARK, KitsuThemePreference.fromStorage("unexpected"))
        assertEquals(KitsuThemePreference.SYSTEM, KitsuThemePreference.fromStorage("system"))
        assertTrue(KitsuThemePreference.DARK.useDarkColors(systemIsDark = false))
        assertTrue(KitsuThemePreference.SYSTEM.useDarkColors(systemIsDark = true))
        assertFalse(KitsuThemePreference.SYSTEM.useDarkColors(systemIsDark = false))
    }

    @Test fun adaptiveNavigationThresholdIsStable() {
        assertFalse(shouldUseNavigationRail(719))
        assertTrue(shouldUseNavigationRail(720))
        assertTrue(shouldUseNavigationRail(1_200))
    }

    @Test fun moderationReportRoundTripsItsMessageOrSenderBinding() {
        val report = ModerationReport(
            appId = "ptl.kitsu.app",
            appVersion = "test-version",
            createdAtEpoch = 1_787_000_000,
            deviceId = "KTDEAD",
            reportType = ReportType.SENDER,
            reason = ReportReason.HARASSMENT_OR_HATE,
            note = "Repeated direct abuse",
            messageId = "11",
            cursor = "11",
            direction = "inbound",
            peerId = "A".repeat(43),
            channel = null,
            text = "message evidence",
            state = "received",
            occurredAt = 1_787_000_001,
        )
        val encoded = ModerationReportCodec.encode(report)
        assertTrue(encoded.contains("\"report_type\": \"sender\""))
        assertEquals(report, ModerationReportCodec.decode(encoded))
        assertTrue(
            runCatching { report.copy(peerId = null) }.isFailure,
        )
    }

    @Test fun fullControllerCopyKeepsRecoveryPhysicalAndActionable() {
        assertEquals(
            "Kitsu already has four controllers. On Kitsu: CONNECT > CONTROLLERS; remove a slot; reopen Pair Phone; retry.",
            "controller_full".humanized(),
        )
    }

    @Test fun existingControllerCannotBeSilentlyReissued() {
        assertEquals(
            "This Kitsu already has a saved controller on this phone. Use Repair Bluetooth pairing, or explicitly forget the old controller before issuing a new one.",
            "controller_already_saved_use_repair_or_forget".humanized(),
        )
    }

    @Test fun transientReplayPressureIsNotPresentedAsPersistentStorageFailure() {
        val busy = "Too many recent actions are still protected. Wait a moment and retry."
        val unavailable = "Durable action storage is unavailable."
        assertEquals(busy, "idempotency_busy".humanized())
        assertEquals(busy, advertiseStatusCopy("idempotency_busy"))
        assertEquals(unavailable, "idempotency_unavailable".humanized())
        assertEquals(unavailable, advertiseStatusCopy("idempotency_unavailable"))
    }

    @Test fun bluetoothPermissionDenialRetainsTheInitiatingFlow() {
        assertEquals("bluetooth_permission_required", blePermissionErrorCode(pairing = false))
        assertEquals("pairing_bluetooth_permission_required", blePermissionErrorCode(pairing = true))
        assertEquals(
            "repair_bluetooth_permission_required",
            blePermissionErrorCode(pairing = true, repair = true),
        )
    }
}
