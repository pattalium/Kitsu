package ptl.kitsu.app.repository

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.fail
import org.junit.Test
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.OwnerCache
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.pairing.BluetoothPairingRepairPolicy
import ptl.kitsu.app.pairing.BluetoothPairingRepairProgress
import ptl.kitsu.app.pairing.ControllerPairingProgress
import ptl.kitsu.app.pairing.ControllerPairingService
import ptl.kitsu.app.pairing.PairingException
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.security.CredentialStore
import ptl.kitsu.app.transport.ConnectResult
import ptl.kitsu.app.transport.KitsuTransport
import ptl.kitsu.app.transport.MockKitsuTransport
import ptl.kitsu.app.transport.TransportException

class OwnerRepositoryPairingRecoveryTest {
    @Test fun repairRetriesExactlyOnceAfterFirstPostBondLocalHostTermination() = runBlocking {
        val fixture = fixture(
            ConnectResult.Failed("gatt_local_host_terminated"),
            ConnectResult.Failed("gatt_timeout"),
            ConnectResult.Connected,
        )
        try {
            assertEquals(
                "gatt_timeout",
                pairingFailure { fixture.repository.repairBluetoothPairing(PROFILE.deviceAddress) },
            )

            assertEquals(2, fixture.transport.connectCount)
            assertEquals(1, fixture.pairing.repairCount)
            assertEquals(PROFILE, fixture.credentials.active())
        } finally {
            fixture.close()
        }
    }

    @Test fun repairNeverRetriesAnotherGattFailure() = runBlocking {
        val fixture = fixture(
            ConnectResult.Failed("gatt_timeout"),
            ConnectResult.Connected,
        )
        try {
            assertEquals(
                "gatt_timeout",
                pairingFailure { fixture.repository.repairBluetoothPairing(PROFILE.deviceAddress) },
            )
            assertEquals(1, fixture.transport.connectCount)
            assertEquals(PROFILE, fixture.credentials.active())
        } finally {
            fixture.close()
        }
    }

    @Test fun explicitForgetUsesSameInvocationProofWithoutAnotherHandshake() = runBlocking {
        val fixture = fixture(
            ConnectResult.Failed("gatt_local_host_terminated"),
            ConnectResult.Failed("controller_authorization_rejected"),
        )
        try {
            assertEquals(
                BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING,
                pairingFailure { fixture.repository.repairBluetoothPairing(PROFILE.deviceAddress) },
            )
            assertEquals(2, fixture.transport.connectCount)

            fixture.repository.forgetController(PROFILE.deviceAddress)

            assertEquals(2, fixture.transport.connectCount)
            assertEquals(0, fixture.transport.remoteForgetCount)
            assertNull(fixture.credentials.active())
            assertEquals(1, fixture.cache.clearCount)
        } finally {
            fixture.close()
        }
    }

    @Test fun ambiguousRepairFailureCannotAuthorizeLocalForget() = runBlocking {
        val fixture = fixture(
            ConnectResult.Failed("controller_auth_failed"),
            ConnectResult.Failed("controller_auth_failed"),
        )
        try {
            assertEquals(
                "controller_auth_failed",
                pairingFailure { fixture.repository.repairBluetoothPairing(PROFILE.deviceAddress) },
            )
            assertEquals(
                "controller_auth_failed",
                transportFailure { fixture.repository.forgetController(PROFILE.deviceAddress) },
            )

            assertEquals(2, fixture.transport.connectCount)
            assertEquals(0, fixture.transport.remoteForgetCount)
            assertNotNull(fixture.credentials.active())
            assertEquals(0, fixture.cache.clearCount)
        } finally {
            fixture.close()
        }
    }

    @Test fun replacingTheCredentialInvalidatesAuthoritativeMissingProof() = runBlocking {
        val fixture = fixture(
            ConnectResult.Failed("controller_authorization_rejected"),
            ConnectResult.Failed("controller_auth_failed"),
        )
        try {
            assertEquals(
                BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING,
                pairingFailure { fixture.repository.repairBluetoothPairing(PROFILE.deviceAddress) },
            )
            val replacement = PROFILE.copy(controllerRootB64 = "replacement-root")
            fixture.credentials.replace(replacement)

            assertEquals(
                "controller_auth_failed",
                transportFailure { fixture.repository.forgetController(PROFILE.deviceAddress) },
            )
            assertEquals(2, fixture.transport.connectCount)
            assertEquals(replacement, fixture.credentials.active())
            assertEquals(0, fixture.cache.clearCount)
        } finally {
            fixture.close()
        }
    }

    @Test fun authoritativeMissingProofDoesNotSurviveRepositoryRecreation() = runBlocking {
        val credentials = MutableCredentials(PROFILE)
        val first = fixtureWithCredentials(
            credentials,
            ConnectResult.Failed("controller_authorization_rejected"),
        )
        try {
            assertEquals(
                BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING,
                pairingFailure { first.repository.repairBluetoothPairing(PROFILE.deviceAddress) },
            )
        } finally {
            first.close()
        }

        val recreated = fixtureWithCredentials(
            credentials,
            ConnectResult.Failed("controller_auth_failed"),
        )
        try {
            assertEquals(
                "controller_auth_failed",
                transportFailure { recreated.repository.forgetController(PROFILE.deviceAddress) },
            )
            assertEquals(1, recreated.transport.connectCount)
            assertEquals(PROFILE, credentials.active())
        } finally {
            recreated.close()
        }
    }

    @Test fun publicConnectInvalidatesAuthoritativeMissingProof() = runBlocking {
        val fixture = fixture(
            ConnectResult.Failed("controller_authorization_rejected"),
            ConnectResult.Failed("controller_auth_failed"),
            ConnectResult.Failed("controller_auth_failed"),
        )
        try {
            assertEquals(
                BluetoothPairingRepairPolicy.SAVED_CONTROLLER_MISSING,
                pairingFailure { fixture.repository.repairBluetoothPairing(PROFILE.deviceAddress) },
            )

            fixture.repository.connectAndRefresh(userInitiated = true)
            assertEquals("controller_auth_failed", fixture.repository.state.value.errorCode)
            assertEquals(
                "controller_auth_failed",
                transportFailure { fixture.repository.forgetController(PROFILE.deviceAddress) },
            )

            assertEquals(3, fixture.transport.connectCount)
            assertEquals(PROFILE, fixture.credentials.active())
            assertEquals(0, fixture.cache.clearCount)
        } finally {
            fixture.close()
        }
    }

    private fun fixture(vararg results: ConnectResult): Fixture =
        fixtureWithCredentials(MutableCredentials(PROFILE), *results)

    private fun fixtureWithCredentials(
        credentials: MutableCredentials,
        vararg results: ConnectResult,
    ): Fixture {
        val transport = SequencedTransport(results.toList())
        val cache = RecordingCache()
        val pairing = RepairPairingService(PROFILE)
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        return Fixture(
            repository = OwnerRepository(
                coordinator = ConnectionCoordinator(transport),
                cache = cache,
                credentials = credentials,
                pairingService = pairing,
                scope = scope,
            ),
            transport = transport,
            credentials = credentials,
            cache = cache,
            pairing = pairing,
            scope = scope,
        )
    }

    private suspend fun pairingFailure(block: suspend () -> Unit): String = try {
        block()
        fail("expected PairingException")
        "unreachable"
    } catch (failure: PairingException) {
        failure.code
    }

    private suspend fun transportFailure(block: suspend () -> Unit): String = try {
        block()
        fail("expected TransportException")
        "unreachable"
    } catch (failure: TransportException) {
        failure.code
    }

    private data class Fixture(
        val repository: OwnerRepository,
        val transport: SequencedTransport,
        val credentials: MutableCredentials,
        val cache: RecordingCache,
        val pairing: RepairPairingService,
        val scope: CoroutineScope,
    ) {
        fun close() = scope.cancel()
    }

    private class SequencedTransport(
        results: List<ConnectResult>,
        private val delegate: MockKitsuTransport = MockKitsuTransport(),
    ) : KitsuTransport by delegate {
        private val results = ArrayDeque(results)
        var connectCount = 0
            private set
        var remoteForgetCount = 0
            private set

        override suspend fun connect(): ConnectResult {
            connectCount += 1
            return results.removeFirstOrNull() ?: error("unexpected_connect")
        }

        override suspend fun forgetController() = delegate.forgetController().also {
            remoteForgetCount += 1
        }
    }

    private class MutableCredentials(initial: BondedCompanion) : CredentialStore {
        private val devices = mutableListOf(initial)
        private var activeAddress: String? = initial.deviceAddress
        private var pending: BondedCompanion? = null
        private var pendingForget: String? = null

        fun active(): BondedCompanion? = devices.firstOrNull {
            it.deviceAddress.equals(activeAddress, ignoreCase = true)
        }

        fun replace(value: BondedCompanion) {
            devices.replaceAll {
                if (it.deviceAddress.equals(value.deviceAddress, ignoreCase = true)) value else it
            }
        }

        override suspend fun bondedCompanion() = active()
        override suspend fun bondedCompanions() = devices.toList()
        override suspend fun saveBondedCompanion(value: BondedCompanion?) {
            devices.clear()
            if (value != null) devices += value
            activeAddress = value?.deviceAddress
        }

        override suspend fun selectBondedCompanion(deviceAddress: String): BondedCompanion? =
            devices.firstOrNull { it.deviceAddress.equals(deviceAddress, ignoreCase = true) }
                ?.also { activeAddress = it.deviceAddress }

        override suspend fun removeBondedCompanion(deviceAddress: String): Boolean {
            val removed = devices.removeAll {
                it.deviceAddress.equals(deviceAddress, ignoreCase = true)
            }
            if (activeAddress.equals(deviceAddress, ignoreCase = true)) {
                activeAddress = devices.firstOrNull()?.deviceAddress
            }
            return removed
        }

        override suspend fun pendingBondedCompanion() = pending
        override suspend fun savePendingBondedCompanion(value: BondedCompanion?) {
            pending = value
        }

        override suspend fun pendingControllerForgetAddress() = pendingForget
        override suspend fun savePendingControllerForgetAddress(deviceAddress: String?) {
            pendingForget = deviceAddress
        }
    }

    private class RecordingCache : OwnerCache {
        var clearCount = 0
            private set
        override fun write(snapshot: CacheSnapshot) = Unit
        override fun read(): CacheSnapshot? = null
        override fun clear() {
            clearCount += 1
        }
    }

    private class RepairPairingService(
        private val profile: BondedCompanion,
    ) : ControllerPairingService {
        var repairCount = 0
            private set

        override suspend fun pairController(
            label: String,
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override suspend fun finishPendingPairing(
            onProgress: (ControllerPairingProgress) -> Unit,
        ): BondedCompanion = error("not used")

        override suspend fun repairBluetoothPairing(
            deviceAddress: String,
            onProgress: (BluetoothPairingRepairProgress) -> Unit,
        ): BondedCompanion {
            repairCount += 1
            assertEquals(profile.deviceAddress, deviceAddress)
            return profile
        }

        override fun cancelPairing() = Unit
    }

    private companion object {
        val PROFILE = BondedCompanion(
            deviceAddress = "00:11:22:33:44:55",
            displayName = "Kitsu",
            controllerIdB64 = "controller-id",
            controllerRootB64 = "controller-root",
        )
    }
}
