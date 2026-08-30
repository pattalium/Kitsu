package ptl.kitsu.app.transport

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.ADVENTURE_HOME_SET_OPERATION
import ptl.kitsu.app.model.ADVENTURE_PRIVACY_SET_OPERATION
import ptl.kitsu.app.model.ADVENTURE_STATE_GET_OPERATION
import ptl.kitsu.app.model.ADVENTURE_WALK_ACK_OPERATION
import ptl.kitsu.app.model.ADVENTURE_WALK_DECIDE_OPERATION
import ptl.kitsu.app.model.ADVENTURE_WALK_FINISH_OPERATION
import ptl.kitsu.app.model.ADVENTURE_WALK_LOCATION_OPERATION
import ptl.kitsu.app.model.ADVENTURE_WALK_START_OPERATION
import ptl.kitsu.app.model.ADVENTURE_WALK_SYNC_OPERATION
import ptl.kitsu.app.model.COMPANION_NICKNAME_SET_OPERATION
import ptl.kitsu.app.model.COMPANION_PROFILE_GET_OPERATION
import ptl.kitsu.app.model.COMPANION_QUESTION_ANSWER_OPERATION
import ptl.kitsu.app.model.COMPANION_REQUEST_ANSWER_OPERATION
import ptl.kitsu.app.model.CompanionQuestionKind
import ptl.kitsu.app.model.FOCUS_ACK_OPERATION
import ptl.kitsu.app.model.FOCUS_CANCEL_OPERATION
import ptl.kitsu.app.model.FOCUS_START_OPERATION
import ptl.kitsu.app.model.FOCUS_STATE_GET_OPERATION
import ptl.kitsu.app.model.FOCUS_STOP_OPERATION
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.FocusStartCommand
import ptl.kitsu.app.model.PetFeaturePolicy
import ptl.kitsu.app.model.WalkDecision
import ptl.kitsu.app.model.WalkDecisionCommand
import ptl.kitsu.app.model.WalkLocationCommand
import ptl.kitsu.app.model.WalkObjective
import ptl.kitsu.app.model.WalkOutcome
import ptl.kitsu.app.model.WalkPrivacy
import ptl.kitsu.app.model.WalkRisk
import ptl.kitsu.app.model.WalkStartCommand
import ptl.kitsu.app.model.WalkSyncCommand
import ptl.kitsu.app.model.WalkTerrain
import ptl.kitsu.app.model.WalkWeather

class PetFeatureWireCodecTest {
    @Test
    fun exactProfileParsesWithCheckInAndUnknownFieldsAreRejected() {
        val profile = PetFeatureWireCodec.profile(PROFILE.toByteArray())
        assertEquals("KITSU", profile.nickname)
        assertEquals(3, profile.bond.level)
        assertEquals(CompanionQuestionKind.HOME_OR_EXPLORE, profile.checkIn.question?.kind)
        assertEquals(11, profile.latestMemory?.event)

        val unknown = PROFILE.replaceFirst("\"nickname\":", "\"future\":1,\"nickname\":")
        assertTransportCode("malformed_companion_profile") {
            PetFeatureWireCodec.profile(unknown.toByteArray())
        }
        val wrongOptions = PROFILE.replace("\"HOME\",\"option1\":\"EXPLORE\"", "\"HERE\",\"option1\":\"THERE\"")
        assertTransportCode("invalid_companion_profile_question") {
            PetFeatureWireCodec.profile(wrongOptions.toByteArray())
        }
    }

    @Test
    fun focusPhasesEnforceTimerMathAndPulsePromptSemantics() {
        val state = PetFeatureWireCodec.focus(FOCUS_BREAK.toByteArray())
        assertEquals(FocusPhase.BREAK, state.phase)
        assertEquals(300_000L, state.remainingMs)
        assertTrue(state.prompt.recommendPulseBreathing)

        val badRemaining = FOCUS_BREAK.replace("\"remaining_ms\":300000", "\"remaining_ms\":299999")
        assertTransportCode("invalid_focus_state_break") {
            PetFeatureWireCodec.focus(badRemaining.toByteArray())
        }
        val automaticBreathing = FOCUS_BREAK.replace(
            "\"recommend_pulse_breathing\":true",
            "\"recommend_pulse_breathing\":false",
        )
        assertTransportCode("invalid_focus_state_break") {
            PetFeatureWireCodec.focus(automaticBreathing.toByteArray())
        }
    }

    @Test
    fun walkStateEnforcesRoutePrivacyBranchAndPostcardSemantics() {
        val active = PetFeatureWireCodec.walk(WALK_ACTIVE.toByteArray())
        assertEquals(2, active.decisionCount)
        assertEquals(8, active.branch)
        assertEquals(WalkPrivacy.COARSE, active.privacy)

        val returned = PetFeatureWireCodec.walk(WALK_RETURNED.toByteArray())
        assertEquals(WalkOutcome.COMPLETE, returned.outcome)
        assertEquals("MEADOW NOTE", returned.postcard?.title)

        val impossibleBranch = WALK_ACTIVE.replace("\"branch\":8", "\"branch\":5")
        assertTransportCode("invalid_walk_state_bounds") {
            PetFeatureWireCodec.walk(impossibleBranch.toByteArray())
        }
        val postcardWhileActive = WALK_ACTIVE.replace(
            "\"postcard\":null",
            "\"postcard\":{\"title\":\"MEADOW NOTE\",\"line\":\"THE PATH KEPT GOING\"}",
        )
        assertTransportCode("invalid_walk_state_active") {
            PetFeatureWireCodec.walk(postcardWhileActive.toByteArray())
        }
    }

    @Test
    fun strictUtf8BoundsAndFirmwareErrorsAreHandledBeforeDtoUse() {
        val malformedUtf8 = byteArrayOf('{'.code.toByte(), 0xC3.toByte(), 0x28, '}'.code.toByte())
        assertTransportCode("malformed_companion_profile") {
            PetFeatureWireCodec.profile(malformedUtf8)
        }
        assertTransportCode("malformed_focus_state") {
            PetFeatureWireCodec.focus(ByteArray(PetFeaturePolicy.MAX_FOCUS_RESPONSE_BYTES + 1))
        }
        assertTransportCode("malformed_walk_state") {
            PetFeatureWireCodec.walk(ByteArray(PetFeaturePolicy.MAX_WALK_RESPONSE_BYTES + 1))
        }
        assertTransportCode("profile_unavailable") {
            PetFeatureWireCodec.profile("{\"ok\":false,\"error\":\"profile_unavailable\"}".toByteArray())
        }
        assertTransportCode("malformed_focus_state") {
            PetFeatureWireCodec.focus(
                "{\"ok\":false,\"error\":\"wrong_phase\",\"future\":1}".toByteArray(),
            )
        }
    }

    @Test
    fun requestBodiesMatchEveryFirmwareParserExactly() {
        assertEquals("{}", PetFeatureWireCodec.emptyBody().toString())
        assertEquals("{\"nickname\":\"KITSU 2\"}", PetFeatureWireCodec.nicknameBody("KITSU 2").toString())
        assertEquals("{\"accept\":false}", PetFeatureWireCodec.requestAnswerBody(false).toString())
        assertEquals("{\"choice\":1}", PetFeatureWireCodec.questionAnswerBody(1).toString())
        assertEquals(
            "{\"session_id\":4294967295,\"minutes\":120}",
            PetFeatureWireCodec.focusStartBody(FocusStartCommand(4_294_967_295L, 120)).toString(),
        )
        assertEquals(
            "{\"session_id\":4294967295}",
            PetFeatureWireCodec.focusSessionBody(4_294_967_295L).toString(),
        )
        assertEquals(
            "{\"terrain\":\"town\",\"objective\":\"return_home\",\"risk\":\"bold\",\"weather\":\"snow\",\"target_steps\":100000,\"commute_safe\":true}",
            PetFeatureWireCodec.walkStartBody(
                WalkStartCommand(
                    terrain = WalkTerrain.TOWN,
                    objective = WalkObjective.RETURN_HOME,
                    risk = WalkRisk.BOLD,
                    weather = WalkWeather.SNOW,
                    targetSteps = 100_000L,
                    commuteSafe = true,
                ),
            ).toString(),
        )
        assertEquals(
            "{\"route_id\":7,\"steps_total\":100000}",
            PetFeatureWireCodec.walkSyncBody(WalkSyncCommand(7L, 100_000L)).toString(),
        )
        assertEquals(
            "{\"route_id\":7,\"zone_token\":9,\"steps_total\":100000,\"distance_meters_total\":10000000}",
            PetFeatureWireCodec.walkLocationBody(
                WalkLocationCommand(7L, 9L, 100_000L, 10_000_000L),
            ).toString(),
        )
        assertEquals(
            "{\"route_id\":7,\"decision\":\"return\"}",
            PetFeatureWireCodec.walkDecisionBody(
                WalkDecisionCommand(7L, WalkDecision.RETURN),
            ).toString(),
        )
        assertEquals("{\"route_id\":7}", PetFeatureWireCodec.walkRouteBody(7L).toString())
        assertEquals(
            "{\"mode\":\"precise_transient\"}",
            PetFeatureWireCodec.walkPrivacyBody(WalkPrivacy.PRECISE_TRANSIENT).toString(),
        )
        assertEquals("{\"zone_token\":9}", PetFeatureWireCodec.walkHomeBody(9L).toString())
    }

    @Test
    fun requestBuildersRejectValuesFirmwareCannotParse() {
        listOf("A\"B", "A\\B", "é", "A".repeat(13), "A\nB").forEach { nickname ->
            assertTransportCode("invalid_companion_nickname") {
                PetFeatureWireCodec.nicknameBody(nickname)
            }
        }
        assertTransportCode("invalid_companion_question_choice") {
            PetFeatureWireCodec.questionAnswerBody(2)
        }
        assertTransportCode("invalid_focus_start") {
            PetFeatureWireCodec.focusStartBody(FocusStartCommand(0L, 25))
        }
        assertTransportCode("invalid_focus_start") {
            PetFeatureWireCodec.focusStartBody(FocusStartCommand(1L, 121))
        }
        assertTransportCode("invalid_walk_start") {
            PetFeatureWireCodec.walkStartBody(
                WalkStartCommand(
                    WalkTerrain.MEADOW,
                    WalkObjective.EXPLORE,
                    WalkRisk.BALANCED,
                    WalkWeather.UNKNOWN,
                    99L,
                    false,
                ),
            )
        }
        assertTransportCode("invalid_walk_location") {
            PetFeatureWireCodec.walkLocationBody(WalkLocationCommand(1L, 0L, 0L, 0L))
        }
        assertTransportCode("invalid_walk_route") {
            PetFeatureWireCodec.walkRouteBody(PetFeaturePolicy.UINT32_MAX + 1L)
        }
    }

    @Test
    fun operationNamesAreFrozenAndPetSpecific() {
        assertEquals(
            listOf(
                "companion.profile.get.v1",
                "companion.profile.nickname.set.v1",
                "companion.request.answer.v1",
                "companion.question.answer.v1",
                "focus.state.get.v1",
                "focus.start.v1",
                "focus.stop.v1",
                "focus.cancel.v1",
                "focus.ack.v1",
                "adventure.state.get.v1",
                "adventure.walk.start.v1",
                "adventure.walk.sync.v1",
                "adventure.walk.location.v1",
                "adventure.walk.decide.v1",
                "adventure.walk.finish.v1",
                "adventure.walk.ack.v1",
                "adventure.privacy.set.v1",
                "adventure.home.set.v1",
            ),
            listOf(
                COMPANION_PROFILE_GET_OPERATION,
                COMPANION_NICKNAME_SET_OPERATION,
                COMPANION_REQUEST_ANSWER_OPERATION,
                COMPANION_QUESTION_ANSWER_OPERATION,
                FOCUS_STATE_GET_OPERATION,
                FOCUS_START_OPERATION,
                FOCUS_STOP_OPERATION,
                FOCUS_CANCEL_OPERATION,
                FOCUS_ACK_OPERATION,
                ADVENTURE_STATE_GET_OPERATION,
                ADVENTURE_WALK_START_OPERATION,
                ADVENTURE_WALK_SYNC_OPERATION,
                ADVENTURE_WALK_LOCATION_OPERATION,
                ADVENTURE_WALK_DECIDE_OPERATION,
                ADVENTURE_WALK_FINISH_OPERATION,
                ADVENTURE_WALK_ACK_OPERATION,
                ADVENTURE_PRIVACY_SET_OPERATION,
                ADVENTURE_HOME_SET_OPERATION,
            ),
        )
    }

    @Test
    fun transportDefaultsDoNotProbeUnsupportedFirmware() = runBlocking {
        val transport = object : KitsuTransport by MockKitsuTransport() {
            override suspend fun companionProfile() = super<KitsuTransport>.companionProfile()
            override suspend fun focusState() = super<KitsuTransport>.focusState()
            override suspend fun walkState() = super<KitsuTransport>.walkState()
        }
        listOf<suspend () -> Any>(
            { transport.companionProfile() },
            { transport.focusState() },
            { transport.walkState() },
        ).forEach { call ->
            val failure = runCatching { call() }.exceptionOrNull() as TransportException
            assertEquals("firmware_operation_unavailable", failure.code)
        }
    }

    private fun assertTransportCode(expected: String, block: () -> Unit) {
        val failure = runCatching(block).exceptionOrNull()
        assertTrue("expected TransportException($expected), got $failure", failure is TransportException)
        assertEquals(expected, (failure as TransportException).code)
    }

    private companion object {
        val PROFILE = """{
            "ok":true,
            "schema":1,
            "nickname":"KITSU",
            "personality":{"kind":"CURIOUS","warmth":70,"playfulness":65,"boldness":40,"curiosity":95},
            "mood":"CONTENT",
            "bond":{"level":3,"xp":100,"speech_stage":2,"dialogue_bank":2},
            "favorite":{"action":"play","time":"evening"},
            "routine":{"action":"feed","time":"morning"},
            "ritual":{"action":"listen","time":"night","days":3},
            "preferences":{"quiet_or_play":1,"dawn_or_night":null,"home_or_explore":0},
            "check_in":{"request":{"state":"pending","action":"pet"},"question":{"kind":"home_or_explore","option0":"HOME","option1":"EXPLORE"},"comfort":{"kind":"none","line1":"I'M ALL RIGHT","line2":"RIGHT HERE"},"callback_ready":false},
            "goal":{"kind":"care","action":"pet","progress":1,"target":2},
            "development":{"momentum":0,"total_actions":1,"streak_days":1,"perfect_days":0,"bests":{"daily_actions":1,"daily_variety":1,"care_rhythm":1}},
            "settings":{"quick_action":"pet","quiet_hours":{"enabled":false,"start_minute":0,"end_minute":0}},
            "latest_memory":{"sequence":1,"event":11,"line1":"NEW SIGNAL","line2":"STILL YOURS"}
        }""".trimIndent()

        val FOCUS_BREAK = """{
            "ok":true,
            "schema":1,
            "phase":"break",
            "completion":"none",
            "session_id":42,
            "focus_minutes":25,
            "break_minutes":5,
            "elapsed_ms":1500000,
            "remaining_ms":300000,
            "sequence":2,
            "prompt":{"title":"FOCUS COMPLETE","detail":"TRY PULSE BREATHING","recommend_pulse_breathing":true}
        }""".trimIndent()

        val WALK_ACTIVE = """{
            "ok":true,
            "schema":1,
            "phase":"active",
            "outcome":"none",
            "route_id":4294967295,
            "steps":500,
            "target_steps":1000,
            "progress_percent":63,
            "distance_meters":100,
            "terrain":"forest",
            "objective":"explore",
            "risk":"balanced",
            "weather":"clear",
            "personality":"CURIOUS",
            "decision_count":2,
            "branch":8,
            "privacy":"coarse",
            "current_zone":7,
            "home_zone":0,
            "known_zones":1,
            "total_distance_meters":100,
            "journal_count":0,
            "postcard":null
        }""".trimIndent()

        val WALK_RETURNED = """{
            "ok":true,
            "schema":1,
            "phase":"returned",
            "outcome":"complete",
            "route_id":7,
            "steps":1000,
            "target_steps":1000,
            "progress_percent":100,
            "distance_meters":0,
            "terrain":"meadow",
            "objective":"explore",
            "risk":"balanced",
            "weather":"clear",
            "personality":"GENTLE",
            "decision_count":0,
            "branch":0,
            "privacy":"off",
            "current_zone":0,
            "home_zone":0,
            "known_zones":0,
            "total_distance_meters":0,
            "journal_count":1,
            "postcard":{"title":"MEADOW NOTE","line":"THE PATH KEPT GOING"}
        }""".trimIndent()
    }
}
