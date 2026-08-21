package app.kitsu.mobile.relay

import app.kitsu.mobile.security.BondedCompanion
import app.kitsu.mobile.transport.TransportException
import java.util.Base64
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MobileRelayContractTest {
    @Test fun gatewayIdentityIsStableCanonicalAndInstallationBound() {
        val first = MobileRelayWirePolicy.gatewayId("00112233-4455-6677-8899-aabbccddeeff")
        val repeated = MobileRelayWirePolicy.gatewayId("00112233-4455-6677-8899-aabbccddeeff")
        val other = MobileRelayWirePolicy.gatewayId("10112233-4455-6677-8899-aabbccddeeff")

        assertEquals(first, repeated)
        assertNotEquals(first, other)
        assertTrue(MobileRelayWirePolicy.canonicalUuid(first))
        val routes = MobileRelayHttpRoutes()
        assertEquals("/v1/mobile-relays/{installation_id}", routes.binding)
        assertEquals(
            "/v1/mobile-relays/{installation_id}/enrollments/{enrollment_id}/claim",
            routes.claim,
        )
        assertEquals("/v1/mobile-relays/{installation_id}/envelopes", routes.envelopes)
        assertEquals("/v1/mobile-relays/{installation_id}/session", routes.session)
        assertEquals("mobile.relay.exchange", MobileRelayBleOperations().exchange)
    }

    @Test fun pullReassemblesExactOrderedChunks() = runTest {
        val payload = ByteArray(MOBILE_RELAY_CHUNK_BYTES + 17) { (it % 251).toByte() }
        val actual = MobileRelayTransfer.pull(MobileRelayPullKind.UPLINK) { _, offset ->
            val end = minOf(offset + MOBILE_RELAY_CHUNK_BYTES, payload.size)
            MobileRelayChunk(
                schema = MobileRelayWirePolicy.CHUNK_SCHEMA,
                kind = "uplink",
                available = true,
                offset = offset,
                total = payload.size,
                dataB64 = Base64.getUrlEncoder().withoutPadding()
                    .encodeToString(payload.copyOfRange(offset, end)),
                final = end == payload.size,
            )
        }
        assertArrayEquals(payload, actual)
    }

    @Test fun pullRejectsOffsetMismatch() = runTest {
        val failure = runCatching {
            MobileRelayTransfer.pull(MobileRelayPullKind.UPLINK) { _, _ ->
                MobileRelayChunk(
                    schema = MobileRelayWirePolicy.CHUNK_SCHEMA,
                    kind = "uplink",
                    available = true,
                    offset = 1,
                    total = 1,
                    dataB64 = "AA",
                    final = true,
                )
            }
        }.exceptionOrNull() as TransportException
        assertEquals("malformed_relay_chunk", failure.code)
    }

    @Test fun pushUsesBoundedChunksAndRequiresAcceptedReceipts() = runTest {
        val payload = ByteArray(MOBILE_RELAY_CHUNK_BYTES + 3) { it.toByte() }
        val offsets = mutableListOf<Int>()
        MobileRelayTransfer.push(MobileRelayPushKind.DOWNLINK, payload) { kind, offset, total, data, final ->
            assertEquals(MobileRelayPushKind.DOWNLINK, kind)
            assertTrue(data.size <= MOBILE_RELAY_CHUNK_BYTES)
            offsets += offset
            MobileRelayReceipt(
                schema = MobileRelayWirePolicy.RECEIPT_SCHEMA,
                kind = "downlink",
                accepted = true,
                nextOffset = offset + data.size,
                complete = final && offset + data.size == total,
            )
        }
        assertEquals(listOf(0, MOBILE_RELAY_CHUNK_BYTES), offsets)
    }

    @Test fun bondedListKeepsNewestThreeAndReplacesMatchingDevice() {
        fun bond(index: Int) = BondedCompanion(
            deviceAddress = "00:00:00:00:00:0$index",
            displayName = "Kitsu $index",
            controllerIdB64 = "controller-$index",
            controllerRootB64 = "root-$index",
        )
        val selected = (1..4).fold(emptyList<BondedCompanion>()) { current, index ->
            MobileRelayBondPolicy.upsert(current, bond(index))
        }
        assertEquals(listOf(bond(2), bond(3), bond(4)), selected)
        assertEquals(
            listOf(bond(2), bond(4), bond(3).copy(displayName = "Renamed")),
            MobileRelayBondPolicy.upsert(selected, bond(3).copy(displayName = "Renamed")),
        )
    }

    @Test fun backendReceiptMapsToExactFirmwareGatewayAck() {
        val exact = MobileRelayWirePolicy.gatewayAcknowledgement("42", "7")
            .toString(Charsets.UTF_8)
        assertEquals(
            "{\"v\":1,\"type\":\"gateway_ack\",\"spool_record_id\":\"42\",\"device_sequence\":\"7\"}",
            exact,
        )
    }
}
