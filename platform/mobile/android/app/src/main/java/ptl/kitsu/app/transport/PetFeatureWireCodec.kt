package ptl.kitsu.app.transport

import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.put
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.FocusStartCommand
import ptl.kitsu.app.model.PetFeaturePolicy
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkDecision
import ptl.kitsu.app.model.WalkDecisionCommand
import ptl.kitsu.app.model.WalkLocationCommand
import ptl.kitsu.app.model.WalkObjective
import ptl.kitsu.app.model.WalkPrivacy
import ptl.kitsu.app.model.WalkRisk
import ptl.kitsu.app.model.WalkStartCommand
import ptl.kitsu.app.model.WalkSyncCommand
import ptl.kitsu.app.model.WalkTerrain
import ptl.kitsu.app.model.WalkWeather

/** Strict schema-1 codec for pet-only authenticated firmware operations. */
internal object PetFeatureWireCodec {
    private val json = Json {
        ignoreUnknownKeys = false
        explicitNulls = true
        isLenient = false
        coerceInputValues = false
    }
    private val errorCode = Regex("^[a-z][a-z0-9_]{0,63}$")

    fun profile(payload: ByteArray): CompanionProfile {
        val value = decodeSuccess<CompanionProfile>(
            payload = payload,
            maxBytes = PetFeaturePolicy.MAX_PROFILE_RESPONSE_BYTES,
            malformedCode = "malformed_companion_profile",
            exactShape = ::hasExactProfileShape,
        )
        PetFeaturePolicy.profileValidationError(value)?.let { throw TransportException(it) }
        return value
    }

    fun focus(payload: ByteArray): FocusSessionState {
        val value = decodeSuccess<FocusSessionState>(
            payload = payload,
            maxBytes = PetFeaturePolicy.MAX_FOCUS_RESPONSE_BYTES,
            malformedCode = "malformed_focus_state",
            exactShape = ::hasExactFocusShape,
        )
        PetFeaturePolicy.focusValidationError(value)?.let { throw TransportException(it) }
        return value
    }

    fun walk(payload: ByteArray): WalkAdventureState {
        val value = decodeSuccess<WalkAdventureState>(
            payload = payload,
            maxBytes = PetFeaturePolicy.MAX_WALK_RESPONSE_BYTES,
            malformedCode = "malformed_walk_state",
            exactShape = ::hasExactWalkShape,
        )
        PetFeaturePolicy.walkValidationError(value)?.let { throw TransportException(it) }
        return value
    }

    fun emptyBody(): JsonObject = buildJsonObject {}

    fun nicknameBody(nickname: String): JsonObject {
        if (!PetFeaturePolicy.validNicknameRequest(nickname)) {
            throw TransportException("invalid_companion_nickname")
        }
        return buildJsonObject { put("nickname", nickname) }
    }

    fun requestAnswerBody(accept: Boolean): JsonObject = buildJsonObject {
        put("accept", accept)
    }

    fun questionAnswerBody(choice: Int): JsonObject {
        if (choice !in 0..1) throw TransportException("invalid_companion_question_choice")
        return buildJsonObject { put("choice", choice) }
    }

    fun focusStartBody(command: FocusStartCommand): JsonObject {
        if (!PetFeaturePolicy.validFocusStart(command)) {
            throw TransportException("invalid_focus_start")
        }
        return buildJsonObject {
            put("session_id", command.sessionId)
            put("minutes", command.minutes)
        }
    }

    fun focusSessionBody(sessionId: Long): JsonObject {
        requireOperationId(sessionId, "invalid_focus_session")
        return buildJsonObject { put("session_id", sessionId) }
    }

    fun walkStartBody(command: WalkStartCommand): JsonObject {
        if (!PetFeaturePolicy.validWalkStart(command)) {
            throw TransportException("invalid_walk_start")
        }
        return buildJsonObject {
            put("terrain", terrainWire(command.terrain))
            put("objective", objectiveWire(command.objective))
            put("risk", riskWire(command.risk))
            put("weather", weatherWire(command.weather))
            put("target_steps", command.targetSteps)
            put("commute_safe", command.commuteSafe)
        }
    }

    fun walkSyncBody(command: WalkSyncCommand): JsonObject {
        if (!PetFeaturePolicy.validWalkSync(command)) {
            throw TransportException("invalid_walk_sync")
        }
        return buildJsonObject {
            put("route_id", command.routeId)
            put("steps_total", command.stepsTotal)
        }
    }

    fun walkLocationBody(command: WalkLocationCommand): JsonObject {
        if (!PetFeaturePolicy.validWalkLocation(command)) {
            throw TransportException("invalid_walk_location")
        }
        return buildJsonObject {
            put("route_id", command.routeId)
            put("zone_token", command.zoneToken)
            put("steps_total", command.stepsTotal)
            put("distance_meters_total", command.distanceMetersTotal)
        }
    }

    fun walkDecisionBody(command: WalkDecisionCommand): JsonObject {
        if (!PetFeaturePolicy.validWalkDecision(command)) {
            throw TransportException("invalid_walk_decision")
        }
        return buildJsonObject {
            put("route_id", command.routeId)
            put("decision", decisionWire(command.decision))
        }
    }

    fun walkRouteBody(routeId: Long): JsonObject {
        requireOperationId(routeId, "invalid_walk_route")
        return buildJsonObject { put("route_id", routeId) }
    }

    fun walkPrivacyBody(mode: WalkPrivacy): JsonObject = buildJsonObject {
        put("mode", privacyWire(mode))
    }

    fun walkHomeBody(zoneToken: Long): JsonObject {
        requireOperationId(zoneToken, "invalid_walk_zone")
        return buildJsonObject { put("zone_token", zoneToken) }
    }

    private inline fun <reified T> decodeSuccess(
        payload: ByteArray,
        maxBytes: Int,
        malformedCode: String,
        exactShape: (JsonElement) -> Boolean,
    ): T {
        if (payload.isEmpty() || payload.size > maxBytes) {
            throw TransportException(malformedCode)
        }
        val wire = try {
            StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(payload))
                .toString()
        } catch (failure: Throwable) {
            throw TransportException(malformedCode, failure)
        }
        val element = try {
            json.parseToJsonElement(wire)
        } catch (failure: Throwable) {
            throw TransportException(malformedCode, failure)
        }
        val root = element as? JsonObject ?: throw TransportException(malformedCode)
        firmwareError(root, malformedCode)?.let { throw TransportException(it) }
        if (!exactShape(element)) throw TransportException(malformedCode)
        return try {
            json.decodeFromJsonElement(element)
        } catch (failure: Throwable) {
            throw TransportException(malformedCode, failure)
        }
    }

    private fun firmwareError(root: JsonObject, malformedCode: String): String? {
        if (root.keys != ERROR_KEYS) return null
        val ok = root["ok"] as? JsonPrimitive ?: throw TransportException(malformedCode)
        val error = root["error"] as? JsonPrimitive ?: throw TransportException(malformedCode)
        if (ok.isString || ok.booleanOrNull != false || !error.isString ||
            !errorCode.matches(error.content)
        ) throw TransportException(malformedCode)
        return error.content
    }

    private fun hasExactProfileShape(element: JsonElement): Boolean {
        val root = element as? JsonObject ?: return false
        if (!root.exactKeys(
                "ok", "schema", "nickname", "personality", "mood", "bond", "favorite",
                "routine", "ritual", "preferences", "check_in", "goal", "development",
                "settings", "latest_memory",
            )
        ) return false
        if (!(root["personality"] as? JsonObject).hasKeys(
                "kind", "warmth", "playfulness", "boldness", "curiosity",
            ) || !(root["bond"] as? JsonObject).hasKeys(
                "level", "xp", "speech_stage", "dialogue_bank",
            )
        ) return false
        if (!root.nullOrExactObject("favorite", "action", "time") ||
            !root.nullOrExactObject("routine", "action", "time") ||
            !root.nullOrExactObject("ritual", "action", "time", "days") ||
            !(root["preferences"] as? JsonObject).hasKeys(
                "quiet_or_play", "dawn_or_night", "home_or_explore",
            )
        ) return false

        val checkIn = root["check_in"] as? JsonObject ?: return false
        if (!checkIn.exactKeys("request", "question", "comfort", "callback_ready") ||
            !(checkIn["request"] as? JsonObject).hasKeys("state", "action") ||
            !checkIn.nullOrExactObject("question", "kind", "option0", "option1") ||
            !(checkIn["comfort"] as? JsonObject).hasKeys("kind", "line1", "line2")
        ) return false

        if (!(root["goal"] as? JsonObject).hasKeys("kind", "action", "progress", "target")) {
            return false
        }
        val development = root["development"] as? JsonObject ?: return false
        if (!development.exactKeys(
                "momentum", "total_actions", "streak_days", "perfect_days", "bests",
            ) || !(development["bests"] as? JsonObject).hasKeys(
                "daily_actions", "daily_variety", "care_rhythm",
            )
        ) return false
        val settings = root["settings"] as? JsonObject ?: return false
        if (!settings.exactKeys("quick_action", "quiet_hours") ||
            !(settings["quiet_hours"] as? JsonObject).hasKeys(
                "enabled", "start_minute", "end_minute",
            )
        ) return false
        return root.nullOrExactObject(
            "latest_memory", "sequence", "event", "line1", "line2",
        )
    }

    private fun hasExactFocusShape(element: JsonElement): Boolean {
        val root = element as? JsonObject ?: return false
        return root.exactKeys(
            "ok", "schema", "phase", "completion", "session_id", "focus_minutes",
            "break_minutes", "elapsed_ms", "remaining_ms", "sequence", "prompt",
        ) && (root["prompt"] as? JsonObject).hasKeys(
            "title", "detail", "recommend_pulse_breathing",
        )
    }

    private fun hasExactWalkShape(element: JsonElement): Boolean {
        val root = element as? JsonObject ?: return false
        return root.exactKeys(
            "ok", "schema", "phase", "outcome", "route_id", "steps", "target_steps",
            "progress_percent", "distance_meters", "terrain", "objective", "risk",
            "weather", "personality", "decision_count", "branch", "privacy",
            "current_zone", "home_zone", "known_zones", "total_distance_meters",
            "journal_count", "postcard",
        ) && root.nullOrExactObject("postcard", "title", "line")
    }

    private fun JsonObject.exactKeys(vararg expected: String): Boolean = keys == expected.toSet()

    private fun JsonObject?.hasKeys(vararg expected: String): Boolean =
        this?.exactKeys(*expected) == true

    private fun JsonObject.nullOrExactObject(key: String, vararg expected: String): Boolean {
        val value = this[key] ?: return false
        return value === JsonNull || (value as? JsonObject)?.exactKeys(*expected) == true
    }

    private fun requireOperationId(value: Long, code: String) {
        if (!PetFeaturePolicy.validOperationId(value)) throw TransportException(code)
    }

    private fun terrainWire(value: WalkTerrain): String = when (value) {
        WalkTerrain.MEADOW -> "meadow"
        WalkTerrain.FOREST -> "forest"
        WalkTerrain.RIDGE -> "ridge"
        WalkTerrain.WATERFRONT -> "waterfront"
        WalkTerrain.TOWN -> "town"
    }

    private fun objectiveWire(value: WalkObjective): String = when (value) {
        WalkObjective.EXPLORE -> "explore"
        WalkObjective.FOLLOW_SIGNAL -> "follow_signal"
        WalkObjective.MEET_CREATURE -> "meet_creature"
        WalkObjective.COMMUNITY -> "community"
        WalkObjective.RETURN_HOME -> "return_home"
    }

    private fun riskWire(value: WalkRisk): String = when (value) {
        WalkRisk.CAREFUL -> "careful"
        WalkRisk.BALANCED -> "balanced"
        WalkRisk.BOLD -> "bold"
    }

    private fun weatherWire(value: WalkWeather): String = when (value) {
        WalkWeather.UNKNOWN -> "unknown"
        WalkWeather.CLEAR -> "clear"
        WalkWeather.RAIN -> "rain"
        WalkWeather.WIND -> "wind"
        WalkWeather.SNOW -> "snow"
    }

    private fun decisionWire(value: WalkDecision): String = when (value) {
        WalkDecision.CONTINUE -> "continue"
        WalkDecision.DETOUR -> "detour"
        WalkDecision.HELP -> "help"
        WalkDecision.RETURN -> "return"
    }

    private fun privacyWire(value: WalkPrivacy): String = when (value) {
        WalkPrivacy.OFF -> "off"
        WalkPrivacy.COARSE -> "coarse"
        WalkPrivacy.PRECISE_TRANSIENT -> "precise_transient"
    }

    private val ERROR_KEYS = setOf("ok", "error")
}
