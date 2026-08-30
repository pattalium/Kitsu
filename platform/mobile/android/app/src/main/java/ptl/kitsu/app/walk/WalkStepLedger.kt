package ptl.kitsu.app.walk

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.util.Locale
import java.util.zip.CRC32
import ptl.kitsu.app.model.PetFeaturePolicy

internal data class WalkStepRouteKey(
    val deviceAddress: String,
    val routeId: Long,
)

internal data class WalkStepCheckpoint(
    val deviceAddress: String,
    val routeId: Long,
    val lastCounter: Long?,
    val stepsTotal: Long,
) {
    val key: WalkStepRouteKey
        get() = WalkStepRouteKey(deviceAddress, routeId)
}

internal data class WalkStepCheckpointSet(
    /** Least-recently bound first. There is at most one current route per device. */
    val checkpoints: List<WalkStepCheckpoint> = emptyList(),
    val activeKey: WalkStepRouteKey? = null,
) {
    init {
        WalkStepCheckpointCodec.requireValid(this)
    }
}

/**
 * Immutable reducer for Android's one global cumulative counter.
 *
 * Only [persisted.activeKey] receives sensor deltas. Selecting or binding another physical device
 * drops the previous active key before its next route is bound, and a returning device is rebased
 * to the current phone counter. A restored active key is preserved so process-death recovery can
 * credit steps taken while the app was not running.
 */
internal data class WalkStepLedger(
    val persisted: WalkStepCheckpointSet = WalkStepCheckpointSet(),
    val selectedDeviceAddress: String? = persisted.activeKey?.deviceAddress,
    val latestCounter: Long? = null,
    /** A physical-device switch needs one fresh sensor callback before any delta is attributable. */
    val freshBaselineRequired: Boolean = false,
) {
    init {
        selectedDeviceAddress?.let(WalkStepDeviceIdentity::requireCanonical)
        require(latestCounter == null || latestCounter >= 0L) { "invalid_step_counter" }
    }

    val activeCheckpoint: WalkStepCheckpoint?
        get() = persisted.activeKey?.let { key ->
            persisted.checkpoints.firstOrNull { it.key == key }
        }

    /** Stops crediting the previously selected board without deleting its durable checkpoint. */
    fun selectDevice(deviceAddress: String): WalkStepLedgerTransition {
        val canonical = WalkStepDeviceIdentity.normalize(deviceAddress)
        if (canonical == selectedDeviceAddress) {
            return WalkStepLedgerTransition(this, storageChanged = false)
        }
        val nextPersisted = persisted.copy(activeKey = null)
        return WalkStepLedgerTransition(
            next = copy(
                persisted = nextPersisted,
                selectedDeviceAddress = canonical,
                freshBaselineRequired = true,
            ),
            storageChanged = nextPersisted != persisted,
            deviceRebased = true,
        )
    }

    fun bindRoute(
        deviceAddress: String,
        routeId: Long,
        firmwareStepsTotal: Long,
    ): WalkStepLedgerTransition {
        val canonical = WalkStepDeviceIdentity.normalize(deviceAddress)
        require(PetFeaturePolicy.validOperationId(routeId)) { "invalid_walk_route" }
        require(firmwareStepsTotal in 0L..PetFeaturePolicy.MAX_WALK_STEPS) {
            "invalid_walk_steps_total"
        }
        val key = WalkStepRouteKey(canonical, routeId)
        val existing = persisted.checkpoints.firstOrNull { it.key == key }
        val continuingRestoredOrLiveRoute = selectedDeviceAddress == canonical &&
            persisted.activeKey == key
        val physicalDeviceSwitch = selectedDeviceAddress != null &&
            selectedDeviceAddress != canonical
        val nextCheckpoint = if (continuingRestoredOrLiveRoute && existing != null) {
            existing.copy(
                lastCounter = existing.lastCounter ?: latestCounter,
                stepsTotal = maxOf(existing.stepsTotal, firmwareStepsTotal),
            )
        } else {
            WalkStepCheckpoint(
                deviceAddress = canonical,
                routeId = routeId,
                // The most recent callback may have been batched before a physical board switch.
                // Require a fresh post-switch sample rather than assigning an ambiguous delta to
                // either board. A new route on the already-selected board may use the known sample.
                lastCounter = if (freshBaselineRequired || physicalDeviceSwitch) {
                    null
                } else {
                    latestCounter
                },
                stepsTotal = maxOf(existing?.stepsTotal ?: 0L, firmwareStepsTotal),
            )
        }
        val retained = persisted.checkpoints
            .filterNot { it.deviceAddress == canonical }
            .plus(nextCheckpoint)
            .takeLast(MAX_RETAINED_DEVICES)
        val nextPersisted = WalkStepCheckpointSet(retained, key)
        return WalkStepLedgerTransition(
            next = copy(
                persisted = nextPersisted,
                selectedDeviceAddress = canonical,
                freshBaselineRequired = false,
            ),
            storageChanged = nextPersisted != persisted,
            deviceRebased = !continuingRestoredOrLiveRoute,
        )
    }

    fun clearRoute(deviceAddress: String, routeId: Long): WalkStepLedgerTransition {
        val canonical = WalkStepDeviceIdentity.normalize(deviceAddress)
        require(PetFeaturePolicy.validOperationId(routeId)) { "invalid_walk_route" }
        val key = WalkStepRouteKey(canonical, routeId)
        if (persisted.checkpoints.none { it.key == key }) {
            return WalkStepLedgerTransition(this, storageChanged = false)
        }
        val nextPersisted = WalkStepCheckpointSet(
            checkpoints = persisted.checkpoints.filterNot { it.key == key },
            activeKey = persisted.activeKey?.takeUnless { it == key },
        )
        return WalkStepLedgerTransition(
            next = copy(persisted = nextPersisted),
            storageChanged = true,
        )
    }

    fun clearDevice(deviceAddress: String): WalkStepLedgerTransition {
        val canonical = WalkStepDeviceIdentity.normalize(deviceAddress)
        if (persisted.checkpoints.none { it.deviceAddress == canonical }) {
            return WalkStepLedgerTransition(this, storageChanged = false)
        }
        val nextPersisted = WalkStepCheckpointSet(
            checkpoints = persisted.checkpoints.filterNot { it.deviceAddress == canonical },
            activeKey = persisted.activeKey?.takeUnless { it.deviceAddress == canonical },
        )
        return WalkStepLedgerTransition(
            next = copy(persisted = nextPersisted),
            storageChanged = true,
        )
    }

    fun observeCounter(counter: Long): WalkStepLedgerTransition {
        require(counter >= 0L) { "invalid_step_counter" }
        val current = activeCheckpoint ?: return WalkStepLedgerTransition(
            next = copy(latestCounter = counter),
            storageChanged = false,
        )
        val previousCounter = current.lastCounter
        val rebased = previousCounter != null && counter < previousCounter
        val nextTotal = when {
            previousCounter == null || rebased -> current.stepsTotal
            else -> saturatingRouteTotal(current.stepsTotal, counter - previousCounter)
        }
        val nextCheckpoint = current.copy(
            lastCounter = counter,
            stepsTotal = nextTotal,
        )
        val nextPersisted = persisted.copy(
            checkpoints = persisted.checkpoints.map {
                if (it.key == current.key) nextCheckpoint else it
            },
        )
        return WalkStepLedgerTransition(
            next = copy(persisted = nextPersisted, latestCounter = counter),
            storageChanged = nextPersisted != persisted,
            counterRebased = rebased,
        )
    }

    private fun saturatingRouteTotal(current: Long, delta: Long): Long =
        current + minOf(delta, PetFeaturePolicy.MAX_WALK_STEPS - current)

    companion object {
        const val MAX_RETAINED_DEVICES = 3
    }
}

internal data class WalkStepLedgerTransition(
    val next: WalkStepLedger,
    val storageChanged: Boolean,
    val counterRebased: Boolean = false,
    val deviceRebased: Boolean = false,
)

internal object WalkStepDeviceIdentity {
    private const val MAX_ADDRESS_BYTES = 64
    private val SAFE_ADDRESS = Regex("^[0-9A-Z:._-]+$")

    fun normalize(deviceAddress: String): String {
        val canonical = deviceAddress.trim().uppercase(Locale.ROOT)
        requireCanonical(canonical)
        return canonical
    }

    fun requireCanonical(deviceAddress: String) {
        require(deviceAddress.isNotEmpty()) { "invalid_walk_device_address" }
        require(deviceAddress.toByteArray(StandardCharsets.UTF_8).size <= MAX_ADDRESS_BYTES) {
            "invalid_walk_device_address"
        }
        require(SAFE_ADDRESS.matches(deviceAddress)) { "invalid_walk_device_address" }
    }

    fun isCanonical(deviceAddress: String): Boolean = runCatching {
        requireCanonical(deviceAddress)
        normalize(deviceAddress) == deviceAddress
    }.getOrDefault(false)

    const val MAX_ENCODED_BYTES = MAX_ADDRESS_BYTES
}

/** Variable-size, versioned, CRC-protected checkpoint set stored only in no-backup storage. */
internal object WalkStepCheckpointCodec {
    private const val MAGIC = 0x4B535432 // KST2
    private const val VERSION = 2
    private const val NO_ACTIVE = -1
    private const val NO_COUNTER = -1L
    private const val HEADER_BYTES = 16
    private const val RECORD_FIXED_BYTES = 4 + 8 + 8 + 8
    private const val CRC_BYTES = 4
    const val ENCODED_MAX_BYTES = HEADER_BYTES +
        WalkStepLedger.MAX_RETAINED_DEVICES *
        (RECORD_FIXED_BYTES + WalkStepDeviceIdentity.MAX_ENCODED_BYTES) + CRC_BYTES

    fun encode(state: WalkStepCheckpointSet): ByteArray {
        requireValid(state)
        val addressBytes = state.checkpoints.map {
            it.deviceAddress.toByteArray(StandardCharsets.UTF_8)
        }
        val payloadBytes = HEADER_BYTES + addressBytes.sumOf { RECORD_FIXED_BYTES + it.size }
        val output = ByteArray(payloadBytes + CRC_BYTES)
        val buffer = ByteBuffer.wrap(output).order(ByteOrder.BIG_ENDIAN)
        buffer.putInt(MAGIC)
        buffer.putInt(VERSION)
        buffer.putInt(state.checkpoints.size)
        buffer.putInt(state.activeKey?.let { key ->
            state.checkpoints.indexOfFirst { it.key == key }
        } ?: NO_ACTIVE)
        state.checkpoints.forEachIndexed { index, checkpoint ->
            val encodedAddress = addressBytes[index]
            buffer.putInt(encodedAddress.size)
            buffer.put(encodedAddress)
            buffer.putLong(checkpoint.routeId)
            buffer.putLong(checkpoint.lastCounter ?: NO_COUNTER)
            buffer.putLong(checkpoint.stepsTotal)
        }
        buffer.putInt(crc32(output, payloadBytes).toInt())
        return output
    }

    fun decode(bytes: ByteArray): WalkStepCheckpointSet? = runCatching {
        if (bytes.size !in (HEADER_BYTES + CRC_BYTES)..ENCODED_MAX_BYTES) return null
        val payloadBytes = bytes.size - CRC_BYTES
        val expectedCrc = ByteBuffer.wrap(bytes, payloadBytes, CRC_BYTES)
            .order(ByteOrder.BIG_ENDIAN).int.toLong() and 0xffff_ffffL
        if (expectedCrc != crc32(bytes, payloadBytes)) return null

        val buffer = ByteBuffer.wrap(bytes, 0, payloadBytes).order(ByteOrder.BIG_ENDIAN)
        if (buffer.int != MAGIC || buffer.int != VERSION) return null
        val count = buffer.int
        val activeIndex = buffer.int
        if (count !in 0..WalkStepLedger.MAX_RETAINED_DEVICES) return null
        if (activeIndex !in NO_ACTIVE until count) return null
        val checkpoints = buildList {
            repeat(count) {
                if (buffer.remaining() < RECORD_FIXED_BYTES) return null
                val addressLength = buffer.int
                if (addressLength !in 1..WalkStepDeviceIdentity.MAX_ENCODED_BYTES ||
                    buffer.remaining() < addressLength + (RECORD_FIXED_BYTES - 4)
                ) return null
                val addressBytes = ByteArray(addressLength).also(buffer::get)
                val address = String(addressBytes, StandardCharsets.UTF_8)
                add(
                    WalkStepCheckpoint(
                        deviceAddress = address,
                        routeId = buffer.long,
                        lastCounter = buffer.long.let { if (it == NO_COUNTER) null else it },
                        stepsTotal = buffer.long,
                    ),
                )
            }
        }
        if (buffer.hasRemaining()) return null
        WalkStepCheckpointSet(
            checkpoints = checkpoints,
            activeKey = activeIndex.takeIf { it != NO_ACTIVE }?.let(checkpoints::get)?.key,
        )
    }.getOrNull()

    fun requireValid(state: WalkStepCheckpointSet) {
        require(state.checkpoints.size <= WalkStepLedger.MAX_RETAINED_DEVICES) {
            "too_many_walk_step_checkpoints"
        }
        require(state.checkpoints.map { it.deviceAddress }.distinct().size == state.checkpoints.size) {
            "duplicate_walk_step_device"
        }
        state.checkpoints.forEach(::requireValid)
        require(state.activeKey == null || state.checkpoints.any { it.key == state.activeKey }) {
            "missing_active_walk_step_checkpoint"
        }
    }

    private fun requireValid(checkpoint: WalkStepCheckpoint) {
        WalkStepDeviceIdentity.requireCanonical(checkpoint.deviceAddress)
        require(PetFeaturePolicy.validOperationId(checkpoint.routeId)) {
            "invalid_walk_step_checkpoint"
        }
        require(checkpoint.lastCounter == null || checkpoint.lastCounter >= 0L) {
            "invalid_walk_step_checkpoint"
        }
        require(checkpoint.stepsTotal in 0L..PetFeaturePolicy.MAX_WALK_STEPS) {
            "invalid_walk_step_checkpoint"
        }
    }

    private fun crc32(bytes: ByteArray, length: Int): Long = CRC32().run {
        update(bytes, 0, length)
        value
    }
}
