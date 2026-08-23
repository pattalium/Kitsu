package ptl.kitsu.app.repository

import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessagePage
import ptl.kitsu.app.transport.FirmwareBlePayloadMapper
import ptl.kitsu.app.transport.TransportException

class MessageSnapshotLoaderTest {
    @Test fun drainsAProductionSizedRingAcrossThreeBoundedPages() = runTest {
        val afters = mutableListOf<String?>()

        val result = MessageSnapshotLoader.load { after, limit ->
            assertEquals(8, limit)
            afters += after
            when (after) {
                null -> page(1..8, cursor = "8", hasMore = true)
                "8" -> page(9..16, cursor = "16", hasMore = true)
                "16" -> page(17..24, cursor = "24", hasMore = false)
                else -> error("unexpected cursor $after")
            }
        }

        assertEquals(listOf(null, "8", "16"), afters)
        assertEquals((1..24).map(Int::toString), result.map(Message::id))
    }

    @Test fun drainsTwentyFourProductionShapedV1RecordsThroughTheRealMapper() = runTest {
        val result = MessageSnapshotLoader.load { after, limit ->
            assertEquals(8, limit)
            val first = after?.toInt()?.plus(1) ?: 1
            val last = minOf(first + 7, 24)
            FirmwareBlePayloadMapper.messages(
                v1Wire(first..last, hasMore = last < 24).toByteArray(),
            )
        }

        assertEquals(24, result.size)
        assertEquals("peer 1", result.first().senderName)
        assertEquals("0", result[1].channel)
        assertEquals("message 24", result.last().text)
    }

    @Test fun stalledCursorFailsClosed() = runTest {
        val failure = failure {
            MessageSnapshotLoader.load { after, _ ->
                if (after == null) page(1..8, "8", true)
                else MessagePage(items = emptyList(), cursor = "8", hasMore = true)
            }
        }

        assertEquals("messages_snapshot_stalled", failure.code)
    }

    @Test fun cursorCannotSkipPastTheLastReturnedMessage() = runTest {
        val failure = failure {
            MessageSnapshotLoader.load { _, _ -> page(1..8, "9", hasMore = true) }
        }

        assertEquals("messages_snapshot_cursor_binding", failure.code)
    }

    @Test fun itemsMustMoveForwardInJournalOrder() = runTest {
        val failure = failure {
            MessageSnapshotLoader.load { _, _ ->
                MessagePage(
                    items = listOf(message("1"), message("3"), message("2")),
                    cursor = "2",
                )
            }
        }

        assertEquals("messages_snapshot_order_invalid", failure.code)
    }

    @Test fun firstItemOnNextPageMustMoveForwardFromRequestedCursor() = runTest {
        val failure = failure {
            MessageSnapshotLoader.load { after, _ ->
                if (after == null) page(5..8, "8", true)
                else MessagePage(items = listOf(message("4")), cursor = "4")
            }
        }

        assertEquals("messages_snapshot_order_invalid", failure.code)
    }

    @Test fun duplicateStableIdAcrossPagesFailsClosed() = runTest {
        val failure = failure {
            MessageSnapshotLoader.load { after, _ ->
                if (after == null) page(1..8, "8", true)
                else MessagePage(listOf(message("8")), cursor = "9")
            }
        }

        assertEquals("messages_snapshot_duplicate", failure.code)
    }

    @Test fun gapDiscardsPartialAttemptAndRestartsExactlyOnce() = runTest {
        var attempt = 0
        val afters = mutableListOf<String?>()

        val result = MessageSnapshotLoader.load { after, _ ->
            afters += after
            if (after == null) {
                attempt += 1
                page(1..8, "8", true)
            } else if (attempt == 1) {
                page(9..9, "9", false, gap = true)
            } else {
                page(9..10, "10", false)
            }
        }

        assertEquals(listOf(null, "8", null, "8"), afters)
        assertEquals((1..10).map(Int::toString), result.map(Message::id))
    }

    @Test fun repeatedGapFailsWithoutReturningAPartialSnapshot() = runTest {
        val failure = failure {
            MessageSnapshotLoader.load { after, _ ->
                if (after == null) page(1..8, "8", true)
                else page(9..9, "9", false, gap = true)
            }
        }

        assertEquals("messages_snapshot_changed", failure.code)
    }

    @Test fun moreThanTwentyFourUniqueMessagesFailsClosed() = runTest {
        val failure = failure {
            MessageSnapshotLoader.load { after, _ -> when (after) {
                null -> page(1..8, "8", true)
                "8" -> page(9..16, "16", true)
                "16" -> page(17..24, "24", true)
                else -> page(25..25, "25", false)
            } }
        }

        assertEquals("messages_snapshot_overflow", failure.code)
    }

    @Test fun transportCannotReturnMoreItemsThanRequestedForOnePage() = runTest {
        val failure = failure {
            MessageSnapshotLoader.load { _, _ -> page(1..9, "9", false) }
        }

        assertEquals("messages_snapshot_page_overflow", failure.code)
    }

    @Test fun uint32WrapFromMaxToOneIsForwardProgress() = runTest {
        val result = MessageSnapshotLoader.load { after, _ ->
            if (after == null) {
                MessagePage(
                    items = listOf(message("4294967294"), message("4294967295")),
                    cursor = "4294967295",
                    hasMore = true,
                )
            } else {
                MessagePage(items = listOf(message("1")), cursor = "1")
            }
        }

        assertEquals(listOf("4294967294", "4294967295", "1"), result.map(Message::id))
    }

    @Test fun v2JournalRevisionDriftRestartsBeforeCommitting() = runTest {
        var attempt = 0
        val result = MessageSnapshotLoader.load { after, _ ->
            if (after == null) {
                attempt += 1
                page(1..8, "8", true, journalRevision = if (attempt == 1) "40" else "41")
            } else {
                page(9..10, "10", false, journalRevision = "41")
            }
        }

        assertEquals((1..10).map(Int::toString), result.map(Message::id))
        assertEquals(2, attempt)
    }

    @Test fun emptyV2SnapshotAcceptsZeroRevisionWithNonzeroSession() = runTest {
        val result = MessageSnapshotLoader.load { after, limit ->
            assertEquals(null, after)
            assertEquals(8, limit)
            MessagePage(
                items = emptyList(),
                cursor = null,
                journalSession = "7",
                journalRevision = "0",
                protocolVersion = 2,
            )
        }

        assertTrue(result.isEmpty())
    }

    @Test fun snapshotReturnsTheExactStableSessionRevisionAndProtocol() = runTest {
        val result = MessageSnapshotLoader.loadSnapshot { _, _ ->
            MessagePage(
                items = listOf(message("1").copy(journalSession = "7")),
                cursor = "1",
                journalSession = "7",
                journalRevision = "11",
                protocolVersion = 2,
            )
        }

        assertEquals(2, result.protocolVersion)
        assertEquals("7", result.journalSession)
        assertEquals("11", result.journalRevision)
        assertEquals("7", result.items.single().journalSession)
    }

    @Test fun v3SnapshotRetainsLocalRepeatObservationAndJournalNamespace() = runTest {
        val result = MessageSnapshotLoader.loadSnapshot { _, _ ->
            MessagePage(
                items = listOf(
                    message("1").copy(
                        journalSession = "8",
                        direction = "outbound",
                        channel = "0",
                        repeatCount = 3,
                    ),
                ),
                cursor = "1",
                journalSession = "8",
                journalRevision = "12",
                protocolVersion = 3,
            )
        }

        assertEquals(3, result.protocolVersion)
        assertEquals("8", result.journalSession)
        assertEquals("12", result.journalRevision)
        assertEquals(3, result.items.single().repeatCount)
    }

    @Test fun v2JournalSessionDriftRestartsBeforeCommitting() = runTest {
        var attempt = 0
        val result = MessageSnapshotLoader.load { after, _ ->
            if (after == null) {
                attempt += 1
                page(1..8, "8", true, journalRevision = "50", journalSession = attempt.toString())
            } else {
                page(9..10, "10", false, journalRevision = "50", journalSession = "2")
            }
        }

        assertEquals((1..10).map(Int::toString), result.map(Message::id))
        assertEquals(2, attempt)
    }

    private fun page(
        ids: IntRange,
        cursor: String,
        hasMore: Boolean,
        gap: Boolean = false,
        journalRevision: String? = null,
        journalSession: String? = journalRevision?.let { "1" },
    ) = MessagePage(
        items = ids.map { message(it.toString()) },
        cursor = cursor,
        hasMore = hasMore,
        cursorExpired = gap,
        journalSession = journalSession,
        journalRevision = journalRevision,
        protocolVersion = if (journalRevision == null) 1 else 2,
    )

    private fun message(id: String) = Message(
        id = id,
        cursor = id,
        direction = "inbound",
        text = "message $id",
        state = "received",
        occurredAt = 1,
    )

    private fun v1Wire(ids: IntRange, hasMore: Boolean): String {
        val peer = "A".repeat(43)
        val items = ids.joinToString(",") { id ->
            if (id % 2 == 1) {
                """{"message_id":"$id","timestamp":17870000$id,"inbound":true,"kind":"direct","authenticated":true,"sender_name":"peer $id","peer_id":"$peer","channel_slot":null,"text":"message $id","state":"received"}"""
            } else {
                """{"message_id":"$id","timestamp":17870000$id,"inbound":true,"kind":"channel","authenticated":false,"sender_name":"channel sender","peer_id":null,"channel_slot":0,"text":"message $id","state":"received"}"""
            }
        }
        return """{"schema":"kitsu.messages.v1","items":[$items],"cursor":"${ids.last}","has_more":$hasMore,"gap":false}"""
    }

    private suspend fun failure(block: suspend () -> Unit): TransportException {
        val throwable = runCatching { block() }.exceptionOrNull()
        assertTrue(throwable is TransportException)
        return throwable as TransportException
    }
}
