package ptl.kitsu.app.ui

import java.util.Base64
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.Peer
import ptl.kitsu.app.model.RepeatSource

class RepeatSourcePresentationPolicyTest {
    @Test fun oneTokenLabelsOnlyOneDistinctAuthenticatedFullKeyMatch() {
        val peer = peer("Hyperion Prime", 0xA1, 1)
        val message = message(listOf(RepeatSource("A1")))

        assertEquals(
            "Returned repeat heard · likely Hyperion Prime",
            RepeatSourcePresentationPolicy.visibleLine(message, listOf(peer)),
        )
        val detail = requireNotNull(RepeatSourcePresentationPolicy.detail(message, listOf(peer)))
        assertTrue(detail.contains("last-hop path token A1"))
        assertTrue(detail.contains("uniquely matches one current authenticated full-key peer"))
        assertTrue(detail.contains("likely Hyperion Prime"))
        assertTrue(detail.contains("not a list of all repeaters or recipients"))
    }

    @Test fun anonymousOrAmbiguousTokensNeverInventAnIdentity() {
        val message = message(listOf(RepeatSource("A1")))
        val ambiguous = listOf(
            peer("Hyperion Prime", 0xA1, 1),
            peer("Different repeater", 0xA1, 2),
        )

        assertEquals("Returned repeat heard", RepeatSourcePresentationPolicy.visibleLine(message, emptyList()))
        assertEquals("Returned repeat heard", RepeatSourcePresentationPolicy.visibleLine(message, ambiguous))
        val detail = requireNotNull(RepeatSourcePresentationPolicy.detail(message, ambiguous))
        assertTrue(detail.contains("does not uniquely match"))
        assertFalse(detail.contains("Hyperion Prime"))
        assertFalse(detail.contains("Different repeater"))
    }

    @Test fun duplicatePeerRowsAreDeduplicatedByFullKeyNotByDisplayName() {
        val sameKey = peer("Hyperion Prime", 0xA1, 1)
        val sameNameDifferentKey = peer("Hyperion Prime", 0xB2, 2)
        val message = message(listOf(RepeatSource("A1")))

        assertEquals(
            "Returned repeat heard · likely Hyperion Prime",
            RepeatSourcePresentationPolicy.visibleLine(message, listOf(sameKey, sameKey, sameNameDifferentKey)),
        )
    }

    @Test fun multipleOrTruncatedSourcesStayGenericAndExplainTheBound() {
        val message = message(
            sources = listOf(
                RepeatSource("01"),
                RepeatSource("0203"),
                RepeatSource("040506"),
                RepeatSource("07"),
            ),
            truncated = true,
            repeatCount = 8,
        )

        assertEquals("Returned repeat heard", RepeatSourcePresentationPolicy.visibleLine(message, emptyList()))
        val detail = requireNotNull(RepeatSourcePresentationPolicy.detail(message, emptyList()))
        assertTrue(detail.contains("4 distinct last-hop path tokens"))
        assertTrue(detail.contains("first four distinct local path tokens"))
    }

    @Test fun meshWideAdvertisementUsesTheSameUniqueFullKeyTrustBoundary() {
        val peer = peer("Hyperion Prime", 0xA1, 1)
        val evidence = LastFloodAdvert(
            emittedAt = 1_787_000_000,
            state = "sent",
            repeatCount = 1,
            observationOpen = true,
            repeatSources = listOf(RepeatSource("A1")),
            repeatSourcesTruncated = false,
        )

        assertEquals(
            "Returned repeat heard · likely Hyperion Prime",
            RepeatSourcePresentationPolicy.visibleLine(evidence, listOf(peer)),
        )
        val detail = requireNotNull(RepeatSourcePresentationPolicy.detail(evidence, listOf(peer)))
        assertTrue(detail.contains("unauthenticated local path tokens"))
        assertTrue(detail.contains("not a list of all repeaters or recipients"))
    }

    private fun peer(name: String, firstByte: Int, tail: Int): Peer {
        val key = ByteArray(32)
        key[0] = firstByte.toByte()
        key[31] = tail.toByte()
        return Peer(
            id = Base64.getUrlEncoder().withoutPadding().encodeToString(key),
            name = name,
        )
    }

    private fun message(
        sources: List<RepeatSource>,
        truncated: Boolean = false,
        repeatCount: Int = 1,
    ) = Message(
        id = "1",
        cursor = "1",
        direction = "outbound",
        channel = "0",
        text = "hello",
        state = "sent",
        occurredAt = 1,
        repeatCount = repeatCount,
        repeatObservationOpen = true,
        repeatSources = sources,
        repeatSourcesTruncated = truncated,
    )
}
