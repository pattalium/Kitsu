package ptl.kitsu.app.transport

import java.util.UUID

/** Rejects callbacks from a closed/replaced GATT or the wrong characteristic. */
internal object GattCallbackBindingPolicy {
    fun accepts(activeGatt: Any?, callbackGatt: Any): Boolean = activeGatt === callbackGatt

    fun accepts(activeGatt: Any?, callbackGatt: Any, expected: UUID, actual: UUID): Boolean =
        accepts(activeGatt, callbackGatt) && expected == actual
}

/** A derived envelope session may only be published onto the GATT that negotiated it. */
internal object GattSessionPublicationPolicy {
    fun accepts(
        activeGatt: Any?,
        expectedGatt: Any,
        activeGeneration: Long,
        expectedGeneration: Long,
    ): Boolean = activeGatt === expectedGatt && activeGeneration == expectedGeneration
}

/** Keeps an authenticated firmware clock rejection distinguishable from transport loss. */
internal object ClockSyncFailurePolicy {
    fun code(failure: Throwable): String =
        (failure as? TransportException)?.code ?: "clock_sync_failed"

    /** A clock rejection is a warning while the authenticated GATT session still exists. */
    fun connectResult(code: String, authenticatedSessionActive: Boolean): ConnectResult =
        if (authenticatedSessionActive) ConnectResult.Connected else ConnectResult.Failed(code)
}

/** Guarantees decoded controller-root material is erased on success, failure, or early return. */
internal object ControllerRootUsePolicy {
    inline fun <T> withZeroized(root: ByteArray, block: () -> T): T = try {
        block()
    } finally {
        root.fill(0)
    }
}

/** A timer posted by a detached link may never expire a replacement link's decoder. */
internal object GattFrameTimeoutGenerationPolicy {
    fun accepts(activeGeneration: Long, scheduledGeneration: Long): Boolean =
        activeGeneration == scheduledGeneration
}

/** Maps Android's numeric GATT failures to stable, actionable app diagnostics. */
internal object GattStatusPolicy {
    fun connectionFailure(status: Int, authenticatedSessionActive: Boolean): String = when (status) {
        // GATT_CONN_TERMINATE_PEER_USER says only that the peripheral ended the
        // link. NimBLE uses this reason for ordinary firmware-requested closes;
        // it is not evidence that Android's SMP bond is stale. Keep the phase so
        // a post-authentication drop remains distinguishable from handshake loss.
        0x13 -> if (authenticatedSessionActive) {
            "gatt_peer_terminated"
        } else {
            "gatt_peer_terminated_before_auth"
        }
        // GATT_CONN_TERMINATE_LOCAL_HOST. Immediately after a new LE bond,
        // Android can close its bonding link while the app's first GATT is
        // opening. Pairing may retry this one condition once after the device
        // is observed advertising again; ordinary connections never retry it.
        0x16 -> "gatt_local_host_terminated"
        else -> "gatt_status_$status"
    }

    fun notificationSubscriptionFailure(status: Int): String = when (status) {
        // A secured CCCD can return either ATT error while Android is holding a
        // missing/stale SMP bond. Never expose those as an opaque descriptor error.
        0x05, 0x0f -> "bluetooth_pairing_repair_required"
        else -> "notify_descriptor_write_failed"
    }
}

/** Allows one fresh-bond recovery and rejects every broader GATT retry loop. */
internal object FreshBondGattRetryPolicy {
    fun shouldRetry(freshBond: Boolean, retriesUsed: Int, failureCode: String?): Boolean =
        freshBond && retriesUsed == 0 && failureCode == "gatt_local_host_terminated"

    fun shouldRetryBeforeGrant(
        freshBond: Boolean,
        retriesUsed: Int,
        failureCode: String?,
        pairingPendingSeen: Boolean,
        candidateStored: Boolean,
    ): Boolean = !pairingPendingSeen && !candidateStored &&
        shouldRetry(freshBond, retriesUsed, failureCode)
}

/** Preserves an active-link disconnect reason across an interrupted GATT write. */
internal object GattWriteFailurePolicy {
    private const val DISCONNECTED_WITHOUT_GATT_ERROR = -1

    fun completionStatus(disconnectStatus: Int): Int =
        disconnectStatus.takeIf { it != 0 } ?: DISCONNECTED_WITHOUT_GATT_ERROR

    fun pairingFailure(lastGattFailureCode: String?): String =
        lastGattFailureCode ?: "gatt_write_failed"
}

internal data class FreshBondGattResult<T>(
    val device: T,
    val result: ConnectResult,
    val retriesUsed: Int,
)

/** Waits for post-bond advertising and owns the single allowed status-22 retry. */
internal object FreshBondGattConnector {
    suspend fun <T> open(
        freshBond: Boolean,
        initialDevice: T,
        awaitAdvertisement: suspend () -> T,
        connect: suspend (T) -> ConnectResult,
    ): FreshBondGattResult<T> {
        var device = if (freshBond) awaitAdvertisement() else initialDevice
        var retriesUsed = 0
        while (true) {
            val result = connect(device)
            val code = (result as? ConnectResult.Failed)?.code
            if (!FreshBondGattRetryPolicy.shouldRetry(freshBond, retriesUsed, code)) {
                return FreshBondGattResult(device, result, retriesUsed)
            }
            retriesUsed += 1
            device = awaitAdvertisement()
        }
    }
}

/** Separates an explicit firmware rejection from ambiguous handshake loss. */
internal object ControllerHandshakeFailurePolicy {
    private val authorizationRejections = setOf(
        "controller_rejected",
        "controller_proof_rejected",
    )

    fun code(handshakeCode: String?, distinguishPendingRejection: Boolean): String = when {
        handshakeCode != null && handshakeCode in authorizationRejections && distinguishPendingRejection ->
            "pending_controller_rejected"
        handshakeCode != null && handshakeCode in authorizationRejections ->
            "controller_authorization_rejected"
        else -> "controller_auth_failed"
    }
}
