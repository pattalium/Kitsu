package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.blePermissionErrorCode
import ptl.kitsu.app.connection.ConnectionState
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.transport.ConnectionMode
import ptl.kitsu.app.update.FirmwareInstallStage

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

    @Test fun connectedClockWarningIsRetryableAndNeverPresentedAsOffline() {
        val presentation = connectionPresentation(
            OwnerState(
                connection = ConnectionState(
                    mode = ConnectionMode.DIRECT_BLE,
                    connected = true,
                    detail = "selected_kitsu_reachable",
                    warning = "system_clock_failed",
                ),
            ),
        )

        assertEquals("Connected", presentation.label)
        assertEquals(StatusTone.ACTIVE, presentation.tone)
        assertEquals(
            "Kitsu rejected clock synchronization. Bluetooth stays connected; synchronize time and retry.",
            presentation.detail,
        )
    }

    @Test fun authenticatedPeerTerminationIsAnOrdinaryDisconnectNotPairingAttention() {
        val presentation = connectionPresentation(
            OwnerState(
                connection = ConnectionState(
                    mode = ConnectionMode.OFFLINE,
                    connected = false,
                    detail = "gatt_peer_terminated",
                ),
                activeDeviceAddress = "00:11:22:33:44:55",
                errorCode = "gatt_peer_terminated",
            ),
        )

        assertEquals("Disconnected", presentation.label)
        assertEquals(StatusTone.NEUTRAL, presentation.tone)
        assertEquals("Kitsu ended the Bluetooth connection", presentation.detail)
    }

    @Test fun independentBluetoothSecurityFailureStillNeedsAttention() {
        val presentation = connectionPresentation(
            OwnerState(
                connection = ConnectionState(
                    mode = ConnectionMode.OFFLINE,
                    connected = false,
                    detail = "bluetooth_pairing_repair_required",
                ),
                activeDeviceAddress = "00:11:22:33:44:55",
                errorCode = "bluetooth_pairing_repair_required",
            ),
        )

        assertEquals("Needs attention", presentation.label)
        assertEquals(StatusTone.NEGATIVE, presentation.tone)
        assertEquals("Bluetooth security pairing needs repair", presentation.detail)
    }

    @Test fun completedFirmwareRequiresAnExplicitOtherSlotConfirmation() {
        assertEquals(
            FirmwareInstallAction("Install on selected Kitsu", false),
            firmwareInstallAction(FirmwareInstallStage.IMPORTED),
        )
        assertEquals(
            FirmwareInstallAction("Install on selected Kitsu", false),
            firmwareInstallAction(FirmwareInstallStage.FAILED),
        )
        assertEquals(
            FirmwareInstallAction("Install again on other slot", true),
            firmwareInstallAction(FirmwareInstallStage.COMPLETE),
        )
        assertEquals(null, firmwareInstallAction(FirmwareInstallStage.REBOOTING))
    }

    @Test fun staleOtherSlotConfirmationHasSafeReselectionCopy() {
        assertEquals(
            "Kitsu's firmware state changed. Review the selected Kitsu, then confirm Install again once more.",
            "firmware_reinstall_confirmation_stale".humanized(),
        )
    }
}
