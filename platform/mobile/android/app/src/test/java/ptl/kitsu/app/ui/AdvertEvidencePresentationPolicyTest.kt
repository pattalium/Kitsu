package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.LastNearbyAdvert
import ptl.kitsu.app.model.Peer
import ptl.kitsu.app.model.RepeatSource

class AdvertEvidencePresentationPolicyTest {
    @Test fun everyFrozenAdvertisementLifecycleHasExactVisibleCopy() {
        assertEquals(
            "No Mesh-wide advertisement recorded",
            AdvertEvidencePresentationPolicy.present(null).statusLine,
        )
        assertEquals("Queued", present("queued").statusLine)

        val openZero = present("sent", repeatCount = 0, observationOpen = true)
        assertEquals("Sent · listening for repeats", openZero.statusLine)
        assertEquals("Listening", openZero.badge)

        val openOne = present("sent", repeatCount = 1, observationOpen = true)
        assertEquals("Sent · heard 1 repeat", openOne.statusLine)
        assertEquals("Listening", openOne.badge)
        assertEquals(
            "Sent · heard 7 repeats",
            present("sent", repeatCount = 7, observationOpen = true).statusLine,
        )

        val closedZero = present("sent", repeatCount = 0, observationOpen = false)
        assertEquals("Sent · no matching repeat recorded", closedZero.statusLine)
        assertEquals("Closed", closedZero.badge)
        assertTrue(closedZero.detail.contains("heard no matching returned copy"))
        assertTrue(closedZero.detail.contains("A repeater may still have forwarded the advertisement"))
        assertTrue(closedZero.detail.contains("Recipient reception remains unconfirmed"))
        assertEquals(
            "Sent · heard 1 repeat",
            present("sent", repeatCount = 1, observationOpen = false).statusLine,
        )
        assertEquals(
            "Sent · heard 255+ repeats",
            present("sent", repeatCount = 255, observationOpen = false).statusLine,
        )

        val failed = present("tx_failed")
        assertEquals("Transmission failed", failed.statusLine)
        assertEquals("Failed", failed.badge)
    }

    @Test fun saturatedAdvertisementCountIsPresentedAsALowerBoundWhileOpenOrClosed() {
        val open = present("sent", repeatCount = 255, observationOpen = true)
        val closed = present("sent", repeatCount = 255, observationOpen = false)

        assertEquals("Sent · heard 255+ repeats", open.statusLine)
        assertTrue(
            open.detail.contains("at least 255 matching rebroadcast packet copies"),
        )
        assertEquals("Sent · heard 255+ repeats", closed.statusLine)
        assertTrue(
            closed.detail.contains("at least 255 matching rebroadcast packet copies"),
        )
    }

    @Test fun everyAdvertisementExplanationScopesEvidenceToLocalPacketCopies() {
        val presentations = listOf(
            AdvertEvidencePresentationPolicy.present(null),
            present("queued"),
            present("sent", repeatCount = 0, observationOpen = true),
            present("sent", repeatCount = 1, observationOpen = true),
            present("sent", repeatCount = 0, observationOpen = false),
            present("sent", repeatCount = 4, observationOpen = false),
            present("tx_failed"),
        )

        presentations.forEach { presentation ->
            assertTrue(presentation.detail.contains("matching rebroadcast packet copies heard locally"))
            assertTrue(presentation.detail.contains("does not identify unique repeaters"))
            assertTrue(presentation.detail.contains("confirm recipient delivery"))
        }
    }

    @Test fun nearbyLifecycleIsZeroHopAndNeverClaimsRepeatOrReceptionEvidence() {
        val cases = mapOf(
            null to "No Nearby advertisement recorded",
            "queued" to "Queued",
            "sent" to "Sent nearby",
            "tx_failed" to "Transmission failed",
        )
        cases.forEach { (state, expected) ->
            val evidence = state?.let { LastNearbyAdvert(1_787_000_000, it) }
            val presentation = NearbyAdvertPresentationPolicy.present(evidence)
            assertEquals(expected, presentation.statusLine)
            assertTrue(presentation.detail.contains("zero-hop"))
            assertTrue(presentation.detail.contains("repeat evidence does not apply"))
            assertTrue(presentation.detail.contains("does not confirm that another Kitsu received it"))
        }
    }

    @Test fun meshWideSourceDetailNamesOnlyAUniqueCurrentFullKeyMatch() {
        val evidence = LastFloodAdvert(
            emittedAt = 1_787_000_000,
            state = "sent",
            repeatCount = 1,
            observationOpen = true,
            repeatSources = listOf(RepeatSource("00")),
            repeatSourcesTruncated = false,
        )
        val peer = Peer("A".repeat(43), "Hyperion Prime")
        val presentation = AdvertEvidencePresentationPolicy.present(evidence, listOf(peer))

        assertTrue(presentation.detail.contains("Path-token matches are unauthenticated"))
        assertTrue(presentation.detail.contains("likely current full-key peer"))
        assertTrue(presentation.detail.contains("does not identify unique repeaters"))
    }

    private fun present(
        state: String,
        repeatCount: Int? = null,
        observationOpen: Boolean = false,
    ) = AdvertEvidencePresentationPolicy.present(
        LastFloodAdvert(
            emittedAt = 1_787_000_000,
            state = state,
            repeatCount = repeatCount,
            observationOpen = observationOpen,
        ),
    )
}
