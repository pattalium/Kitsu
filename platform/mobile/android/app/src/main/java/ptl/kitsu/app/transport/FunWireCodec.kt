package ptl.kitsu.app.transport

import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.put
import ptl.kitsu.app.model.ExpeditionDuration
import ptl.kitsu.app.model.FunState
import ptl.kitsu.app.model.FunStatePolicy
import ptl.kitsu.app.model.PartyJoinCommand
import ptl.kitsu.app.model.PartyRoundCommand
import ptl.kitsu.app.model.PartySignalChoice
import ptl.kitsu.app.model.StoryTrigger

/** Exact, bounded v1 contract shared by every authenticated fun operation. */
internal object FunWireCodec {
    private val json = Json {
        ignoreUnknownKeys = false
        explicitNulls = true
        isLenient = false
        coerceInputValues = false
    }

    fun state(payload: ByteArray): FunState {
        if (payload.isEmpty() || payload.size > FunStatePolicy.MAX_RESPONSE_BYTES) {
            throw TransportException("malformed_fun_state")
        }
        val wire = try {
            StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(java.nio.ByteBuffer.wrap(payload))
                .toString()
        } catch (failure: Throwable) {
            throw TransportException("malformed_fun_state", failure)
        }
        val value = try {
            val element = json.parseToJsonElement(wire)
            if (!hasExactShape(element)) throw IllegalArgumentException("unexpected_fun_shape")
            json.decodeFromJsonElement<FunState>(element)
        } catch (failure: Throwable) {
            throw TransportException("malformed_fun_state", failure)
        }
        FunStatePolicy.validationError(value)?.let { error ->
            throw TransportException(error)
        }
        return value
    }

    fun expeditionStartBody(duration: ExpeditionDuration): JsonObject = buildJsonObject {
        put("duration", when (duration) {
            ExpeditionDuration.SHORT -> "short"
            ExpeditionDuration.MEDIUM -> "medium"
            ExpeditionDuration.LONG -> "long"
        })
    }

    fun storyStartBody(trigger: StoryTrigger): JsonObject = buildJsonObject {
        put("trigger", when (trigger) {
            StoryTrigger.QUIET -> "quiet"
            StoryTrigger.EXPEDITION -> "expedition"
            StoryTrigger.NEARBY -> "nearby"
        })
    }

    fun storyAdvanceBody(storyId: Int): JsonObject {
        requireStoryId(storyId)
        return buildJsonObject { put("story_id", storyId) }
    }

    fun storyChooseBody(storyId: Int, choice: Int): JsonObject {
        requireStoryId(storyId)
        if (choice !in 0..2) throw TransportException("invalid_fun_story_choice")
        return buildJsonObject {
            put("story_id", storyId)
            put("choice", choice)
        }
    }

    fun partyJoinBody(command: PartyJoinCommand): JsonObject {
        if (!FunStatePolicy.validPartyJoin(command)) {
            throw TransportException("invalid_fun_party_host")
        }
        return buildJsonObject {
            put("host_device_id", command.hostDeviceId)
            put("session_nonce", command.sessionNonce)
        }
    }

    fun partyChooseBody(command: PartyRoundCommand): JsonObject {
        if (!FunStatePolicy.validPartyRound(command)) {
            throw TransportException("invalid_fun_party_choice")
        }
        return buildJsonObject {
            put("round", command.round)
            put("choice", when (command.choice) {
                PartySignalChoice.SWEEP -> "sweep"
                PartySignalChoice.LISTEN -> "listen"
                PartySignalChoice.PULSE -> "pulse"
                PartySignalChoice.NONE -> error("validated above")
            })
        }
    }

    private fun requireStoryId(storyId: Int) {
        if (!FunStatePolicy.validStoryId(storyId)) {
            throw TransportException("invalid_fun_story_id")
        }
    }

    private fun hasExactShape(element: JsonElement): Boolean {
        val root = element as? JsonObject ?: return false
        if (!root.exactKeys("schema", "expedition", "story", "party")) return false

        val expedition = root["expedition"] as? JsonObject ?: return false
        if (!expedition.exactKeys(
                "status", "duration", "expedition_id", "total_seconds", "remaining_seconds",
                "progress_percent", "report",
            ) || !expedition.nullOrExactObject(
                "report", "expedition_id", "headline", "detail", "affection_delta",
                "personality_axis", "personality_delta", "encounter_catalog_index",
            )
        ) return false

        val story = root["story"] as? JsonObject ?: return false
        if (!story.exactKeys("status", "beat", "resolution") ||
            !story.nullOrExactObject("beat", "story_id", "scene", "line1", "line2", "choices") ||
            !story.nullOrExactObject(
                "resolution", "story_id", "line1", "line2", "tone", "affection_delta",
                "energy_delta", "curiosity_delta", "personality_match",
            )
        ) return false

        val party = root["party"] as? JsonObject ?: return false
        if (!party.exactKeys(
                "role", "phase", "host_device_id", "session_nonce", "discovered_hosts",
                "participant_count", "participants", "round", "local_choice", "reward",
            )
        ) return false
        val hosts = party["discovered_hosts"] as? JsonArray ?: return false
        if (hosts.any { host ->
                (host as? JsonObject)?.exactKeys(
                    "host_device_id", "session_nonce", "participant_count",
                    "join_window_seconds", "rssi", "last_seen_age_ms",
                ) != true
            }
        ) return false
        val participants = party["participants"] as? JsonArray ?: return false
        if (participants.any { participant ->
                (participant as? JsonObject)?.exactKeys(
                    "device_id", "submitted_round", "local",
                ) != true
            }
        ) return false
        return (party["reward"] as? JsonObject)?.exactKeys(
            "tier", "score", "maximum_score", "bond_awarded", "party_bond",
            "eligible_unique_peers", "current_streak_days", "longest_streak_days",
        ) == true
    }

    private fun JsonObject.exactKeys(vararg expected: String): Boolean =
        keys == expected.toSet()

    private fun JsonObject.nullOrExactObject(key: String, vararg expected: String): Boolean {
        val value = this[key] ?: return false
        return value === JsonNull || (value as? JsonObject)?.exactKeys(*expected) == true
    }
}
