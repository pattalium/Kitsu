package ptl.kitsu.app.repository

import ptl.kitsu.app.model.ActionReceipt
import ptl.kitsu.app.model.AdvertiseScope
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.LastNearbyAdvert
import ptl.kitsu.app.model.MeshState
import ptl.kitsu.app.transport.TransportException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class AdvertiseSubmissionTest {
    @Test
    fun queuedReceiptReturnsWithoutWaitingForAnyRefresh() = runTest {
        val expected = queuedReceipt()

        val actual = awaitAdvertiseReceipt(1_000L) { expected }

        assertSame(expected, actual)
        assertEquals(30_000L, ADVERTISE_COOLDOWN_MILLIS)
    }

    @Test
    fun wholeSubmissionDeadlineFailsClosedWhenReceiptNeverArrives() = runTest {
        val failure = runCatching {
            awaitAdvertiseReceipt(100L) {
                delay(Long.MAX_VALUE)
                queuedReceipt()
            }
        }.exceptionOrNull()

        assertTrue(failure is TransportException)
        assertEquals(ADVERTISE_RESULT_UNKNOWN, (failure as TransportException).code)
    }

    @Test
    fun lostTransportReceiptMapsToResultUnknown() = runTest {
        val transportFailure = TransportException("request_timeout")

        val failure = runCatching {
            awaitAdvertiseReceipt(1_000L) { throw transportFailure }
        }.exceptionOrNull()

        assertTrue(failure is TransportException)
        failure as TransportException
        assertEquals(ADVERTISE_RESULT_UNKNOWN, failure.code)
        assertSame(transportFailure, failure.cause)
    }

    @Test
    fun confirmedFirmwareRejectionRemainsExact() = runTest {
        val rejection = TransportException("advertise_cooldown")

        val failure = runCatching {
            awaitAdvertiseReceipt(1_000L) { throw rejection }
        }.exceptionOrNull()

        assertSame(rejection, failure)
    }

    @Test
    fun lifecycleCancellationIsNotConvertedOrRecovered() = runTest {
        val entered = CompletableDeferred<Unit>()
        val observed = CompletableDeferred<Throwable>()
        val cancellation = CancellationException("screen_closed")
        val job = launch {
            try {
                awaitAdvertiseReceipt(20_000L) {
                    entered.complete(Unit)
                    awaitCancellation()
                }
            } catch (failure: Throwable) {
                observed.complete(failure)
                throw failure
            }
        }
        entered.await()

        job.cancel(cancellation)
        job.join()

        val failure = observed.await()
        assertTrue(failure is CancellationException)
        assertFalse(failure is TransportException)
        assertEquals("screen_closed", failure.message)
    }

    @Test fun nearbyCooldownNeverErasesThePriorMeshWideEvidence() {
        val prior = LastFloodAdvert(1_787_000_000, "sent", repeatCount = 3, observationOpen = false)
        val current = OwnerState(
            status = KitsuStatus(
                deviceId = "KTDEAD",
                companionName = "Fox",
                mesh = MeshState(
                    advertiseReady = true,
                    lastFloodAdvert = prior,
                ),
                updatedAt = 1,
            ),
        )

        val accepted = acceptedAdvertiseState(current, AdvertiseScope.NEARBY, refreshedStatus = null)

        assertEquals(prior, accepted.status?.mesh?.lastFloodAdvert)
        assertEquals(ADVERTISE_COOLDOWN_MILLIS, accepted.status?.mesh?.advertiseRetryAfterMs)
        assertEquals(ADVERTISE_COOLDOWN, accepted.status?.mesh?.advertiseError)
    }

    @Test fun meshWideReceiptCommitsOnlyTheAuthenticatedRefreshedEvidence() {
        val prior = LastFloodAdvert(1_787_000_000, "sent", repeatCount = 3, observationOpen = false)
        val queued = LastFloodAdvert(1_787_000_100, "queued")
        val current = OwnerState(
            status = KitsuStatus(
                deviceId = "KTDEAD",
                companionName = "Fox",
                mesh = MeshState(lastFloodAdvert = prior),
                updatedAt = 1,
            ),
        )
        val refreshed = requireNotNull(current.status).copy(
            firmwareVersion = "0.16.0",
            mesh = requireNotNull(current.status).mesh.copy(lastFloodAdvert = queued),
            updatedAt = 2,
        )

        val accepted = acceptedAdvertiseState(current, AdvertiseScope.MESH, refreshed)

        assertEquals(queued, accepted.status?.mesh?.lastFloodAdvert)
        assertEquals(2L, accepted.status?.updatedAt)
        assertTrue(accepted.messageMarkReadSupported)
    }

    @Test fun nearbyReceiptCommitsOnlyItsAuthenticatedRecordAndRetainsFloodEvidence() {
        val prior = LastFloodAdvert(1_787_000_000, "sent", repeatCount = 3, observationOpen = false)
        val nearby = LastNearbyAdvert(1_787_000_100, "queued")
        val currentStatus = KitsuStatus(
            deviceId = "KTDEAD",
            companionName = "Fox",
            mesh = MeshState(lastFloodAdvert = prior),
            updatedAt = 1,
        )
        val refreshed = currentStatus.copy(
            firmwareVersion = "0.16.1",
            mesh = currentStatus.mesh.copy(lastNearbyAdvert = nearby),
            updatedAt = 2,
        )

        val accepted = acceptedAdvertiseState(
            OwnerState(status = currentStatus),
            AdvertiseScope.NEARBY,
            refreshed,
        )

        assertEquals(nearby, accepted.status?.mesh?.lastNearbyAdvert)
        assertEquals(prior, accepted.status?.mesh?.lastFloodAdvert)
        assertEquals(2L, accepted.status?.updatedAt)
    }

    private fun queuedReceipt() = ActionReceipt(
        clientRequestId = "00112233-4455-6677-8899-aabbccddeeff",
        accepted = true,
        state = "queued",
    )
}
