package ptl.kitsu.app.transport

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.ExpeditionDuration
import ptl.kitsu.app.model.FUN_EXPEDITION_CLAIM_OPERATION
import ptl.kitsu.app.model.FUN_EXPEDITION_START_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_BEGIN_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_CHOOSE_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_HOST_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_JOIN_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_LEAVE_OPERATION
import ptl.kitsu.app.model.FUN_PARTY_SCAN_OPERATION
import ptl.kitsu.app.model.FUN_STATE_GET_OPERATION
import ptl.kitsu.app.model.FUN_STORY_ADVANCE_OPERATION
import ptl.kitsu.app.model.FUN_STORY_CHOOSE_OPERATION
import ptl.kitsu.app.model.FUN_STORY_START_OPERATION
import ptl.kitsu.app.model.FunStatePolicy
import ptl.kitsu.app.model.PartyJoinCommand
import ptl.kitsu.app.model.PartyRoundCommand
import ptl.kitsu.app.model.PartySignalChoice
import ptl.kitsu.app.model.StoryTrigger

class FunWireCodecTest {
    @Test
    fun exactIdleStateParsesAndUnknownOrMissingFieldsAreRejected() {
        val state = FunWireCodec.state(IDLE_STATE.toByteArray())
        assertEquals("kitsu.fun-state.v1", state.schema)
        assertEquals(0L, state.party.reward.partyBond)

        listOf(
            IDLE_STATE.replaceFirst("\"schema\":", "\"future\":1,\"schema\":"),
            IDLE_STATE.replace(",\"report\":null", ""),
            IDLE_STATE.replace("\"schema\":\"kitsu.fun-state.v1\"", "\"schema\":\"wrong\""),
        ).forEachIndexed { index, invalid ->
            val thrown = runCatching { FunWireCodec.state(invalid.toByteArray()) }.exceptionOrNull()
            assertTrue("invalid exact-shape case $index was accepted", thrown is TransportException)
            val failure = thrown as TransportException
            assertTrue(failure.code == "malformed_fun_state" || failure.code == "invalid_fun_schema")
        }
    }

    @Test
    fun expeditionAndStoryStateMustObeyTheirCrossFieldPhases() {
        val returned = FunWireCodec.state(RETURNED_AND_CHOOSING.toByteArray())
        assertEquals(4_294_967_295L, returned.expedition.expeditionId)
        assertEquals(3, returned.story.beat?.choices?.size)

        val scoutingWithReport = RETURNED_AND_CHOOSING
            .replace("\"status\":\"returned\"", "\"status\":\"scouting\"")
            .replace("\"remaining_seconds\":0", "\"remaining_seconds\":1")
            .replace("\"progress_percent\":100", "\"progress_percent\":99")
        assertEquals(
            "invalid_fun_expedition_scouting",
            (runCatching { FunWireCodec.state(scoutingWithReport.toByteArray()) }
                .exceptionOrNull() as TransportException).code,
        )

        val choosingWithoutChoices = RETURNED_AND_CHOOSING.replace(
            "\"choices\":[\"FOLLOW\",\"WAIT\",\"CALL\"]",
            "\"choices\":[]",
        )
        assertEquals(
            "invalid_fun_story_choosing",
            (runCatching { FunWireCodec.state(choosingWithoutChoices.toByteArray()) }
                .exceptionOrNull() as TransportException).code,
        )
    }

    @Test
    fun partyCompletionCarriesBoundedParticipantsRoundAndIncentive() {
        val state = FunWireCodec.state(PARTY_COMPLETE.toByteArray())
        assertEquals(2, state.party.participantCount)
        assertEquals(3, state.party.round)
        assertEquals(4, state.party.reward.bondAwarded)
        assertEquals(42L, state.party.reward.partyBond)

        val badRound = PARTY_COMPLETE.replace("\"round\":3", "\"round\":2")
        assertEquals(
            "invalid_fun_party_complete",
            (runCatching { FunWireCodec.state(badRound.toByteArray()) }
                .exceptionOrNull() as TransportException).code,
        )

        val duplicateParticipant = PARTY_COMPLETE.replace("KT12AF", "KT0001")
        assertEquals(
            "invalid_fun_party",
            (runCatching { FunWireCodec.state(duplicateParticipant.toByteArray()) }
                .exceptionOrNull() as TransportException).code,
        )
    }

    @Test
    fun scanIsCappedAtFourFreshJoinableHostsAndTwoKiB() {
        val fourHosts = (1..4).joinToString(",") { index ->
            """{"host_device_id":"KT000$index","session_nonce":$index,"participant_count":1,"join_window_seconds":600,"rssi":-70.5,"last_seen_age_ms":120000}"""
        }
        val payload = IDLE_STATE.replace("\"discovered_hosts\":[]", "\"discovered_hosts\":[$fourHosts]")
        assertTrue(payload.toByteArray().size < FunStatePolicy.MAX_RESPONSE_BYTES)
        assertEquals(4, FunWireCodec.state(payload.toByteArray()).party.discoveredHosts.size)

        // Independently exercise the raw response ceiling: parsing never allocates an unbounded DTO.
        val oversized = ByteArray(FunStatePolicy.MAX_RESPONSE_BYTES + 1) { ' '.code.toByte() }
        assertEquals(
            "malformed_fun_state",
            (runCatching { FunWireCodec.state(oversized) }.exceptionOrNull() as TransportException).code,
        )
        // A fifth host is invalid even when the wire itself remains below the byte ceiling.
        val explicitFive = IDLE_STATE.replace(
            "\"discovered_hosts\":[]",
            "\"discovered_hosts\":[$fourHosts,{\"host_device_id\":\"KT0005\",\"session_nonce\":5,\"participant_count\":1,\"join_window_seconds\":600,\"rssi\":-70.5,\"last_seen_age_ms\":1}]",
        )
        assertEquals(
            "invalid_fun_party",
            (runCatching { FunWireCodec.state(explicitFive.toByteArray()) }
                .exceptionOrNull() as TransportException).code,
        )
    }

    @Test
    fun malformedUtf8AndOversizedAuthoredTextAreRejected() {
        val malformedUtf8 = byteArrayOf('{'.code.toByte(), 0xC3.toByte(), 0x28, '}'.code.toByte())
        assertEquals(
            "malformed_fun_state",
            (runCatching { FunWireCodec.state(malformedUtf8) }
                .exceptionOrNull() as TransportException).code,
        )

        val oversizedLine = RETURNED_AND_CHOOSING.replace("SIGNAL FOUND", "A".repeat(65))
        assertEquals(
            "invalid_fun_expedition",
            (runCatching { FunWireCodec.state(oversizedLine.toByteArray()) }
                .exceptionOrNull() as TransportException).code,
        )
    }

    @Test
    fun requestBodiesAndOperationNamesAreFrozen() {
        assertEquals("{\"duration\":\"medium\"}", FunWireCodec.expeditionStartBody(ExpeditionDuration.MEDIUM).toString())
        assertEquals("{\"trigger\":\"nearby\"}", FunWireCodec.storyStartBody(StoryTrigger.NEARBY).toString())
        assertEquals("6", FunWireCodec.storyAdvanceBody(6).getValue("story_id").jsonPrimitive.content)
        assertEquals(
            "{\"story_id\":6,\"choice\":2}",
            FunWireCodec.storyChooseBody(6, 2).toString(),
        )
        assertEquals(
            "{\"host_device_id\":\"KT12AF\",\"session_nonce\":4294967295}",
            FunWireCodec.partyJoinBody(PartyJoinCommand("KT12AF", 4_294_967_295L)).toString(),
        )
        assertEquals(
            "{\"round\":3,\"choice\":\"pulse\"}",
            FunWireCodec.partyChooseBody(PartyRoundCommand(3, PartySignalChoice.PULSE)).toString(),
        )
        assertEquals(
            listOf(
                "fun.state.get.v1",
                "fun.expedition.start.v1",
                "fun.expedition.claim.v1",
                "fun.story.start.v1",
                "fun.story.advance.v1",
                "fun.story.choose.v1",
                "fun.party.scan.v1",
                "fun.party.host.v1",
                "fun.party.join.v1",
                "fun.party.begin.v1",
                "fun.party.choose.v1",
                "fun.party.leave.v1",
            ),
            listOf(
                FUN_STATE_GET_OPERATION,
                FUN_EXPEDITION_START_OPERATION,
                FUN_EXPEDITION_CLAIM_OPERATION,
                FUN_STORY_START_OPERATION,
                FUN_STORY_ADVANCE_OPERATION,
                FUN_STORY_CHOOSE_OPERATION,
                FUN_PARTY_SCAN_OPERATION,
                FUN_PARTY_HOST_OPERATION,
                FUN_PARTY_JOIN_OPERATION,
                FUN_PARTY_BEGIN_OPERATION,
                FUN_PARTY_CHOOSE_OPERATION,
                FUN_PARTY_LEAVE_OPERATION,
            ),
        )

        listOf(
            runCatching { FunWireCodec.storyAdvanceBody(0) }.exceptionOrNull(),
            runCatching { FunWireCodec.storyChooseBody(1, 3) }.exceptionOrNull(),
            runCatching { FunWireCodec.partyJoinBody(PartyJoinCommand("bad", 1)) }.exceptionOrNull(),
            runCatching { FunWireCodec.partyChooseBody(PartyRoundCommand(0, PartySignalChoice.NONE)) }.exceptionOrNull(),
        ).forEach { assertTrue(it is TransportException) }
    }

    @Test
    fun transportDefaultsKeepExistingMocksSourceCompatible() = runBlocking {
        val transport = MockKitsuTransport()
        val failure = runCatching { transport.funState() }.exceptionOrNull() as TransportException
        assertEquals("firmware_operation_unavailable", failure.code)
    }

    private companion object {
        val IDLE_STATE = """{
            "schema":"kitsu.fun-state.v1",
            "expedition":{"status":"idle","duration":null,"expedition_id":null,"total_seconds":0,"remaining_seconds":0,"progress_percent":0,"report":null},
            "story":{"status":"idle","beat":null,"resolution":null},
            "party":{"role":"none","phase":"idle","host_device_id":null,"session_nonce":null,"discovered_hosts":[],"participant_count":0,"participants":[],"round":0,"local_choice":"none","reward":{"tier":"none","score":0,"maximum_score":0,"bond_awarded":0,"party_bond":0,"eligible_unique_peers":0,"current_streak_days":0,"longest_streak_days":0}}
        }""".trimIndent()

        val RETURNED_AND_CHOOSING = """{
            "schema":"kitsu.fun-state.v1",
            "expedition":{"status":"returned","duration":"long","expedition_id":4294967295,"total_seconds":28800,"remaining_seconds":0,"progress_percent":100,"report":{"expedition_id":4294967295,"headline":"SIGNAL FOUND","detail":"A NEW TRAIL HUMMED","affection_delta":3,"personality_axis":"curiosity","personality_delta":2,"encounter_catalog_index":20}},
            "story":{"status":"choosing","beat":{"story_id":6,"scene":1,"line1":"HOW SHOULD","line2":"WE ANSWER?","choices":["FOLLOW","WAIT","CALL"]},"resolution":null},
            "party":{"role":"none","phase":"idle","host_device_id":null,"session_nonce":null,"discovered_hosts":[],"participant_count":0,"participants":[],"round":0,"local_choice":"none","reward":{"tier":"none","score":0,"maximum_score":0,"bond_awarded":0,"party_bond":9,"eligible_unique_peers":0,"current_streak_days":1,"longest_streak_days":2}}
        }""".trimIndent()

        val PARTY_COMPLETE = """{
            "schema":"kitsu.fun-state.v1",
            "expedition":{"status":"idle","duration":null,"expedition_id":null,"total_seconds":0,"remaining_seconds":0,"progress_percent":0,"report":null},
            "story":{"status":"idle","beat":null,"resolution":{"story_id":1,"line1":"WE FOUND IT","line2":"TOGETHER","tone":"warm","affection_delta":2,"energy_delta":0,"curiosity_delta":1,"personality_match":true}},
            "party":{"role":"host","phase":"complete","host_device_id":"KT0001","session_nonce":4294967295,"discovered_hosts":[],"participant_count":2,"participants":[{"device_id":"KT0001","submitted_round":3,"local":true},{"device_id":"KT12AF","submitted_round":3,"local":false}],"round":3,"local_choice":"pulse","reward":{"tier":"resonant","score":24,"maximum_score":24,"bond_awarded":4,"party_bond":42,"eligible_unique_peers":1,"current_streak_days":3,"longest_streak_days":4}}
        }""".trimIndent()
    }
}
