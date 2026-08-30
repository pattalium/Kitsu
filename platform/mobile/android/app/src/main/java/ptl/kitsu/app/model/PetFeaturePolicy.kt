package ptl.kitsu.app.model

/** Bounds and cross-field rules from the firmware's schema-1 pet APIs. */
object PetFeaturePolicy {
    const val UINT16_MAX = 65_535
    const val UINT32_MAX = 4_294_967_295L
    const val MAX_PROFILE_RESPONSE_BYTES = 4 * 1024
    const val MAX_FOCUS_RESPONSE_BYTES = 1024
    const val MAX_WALK_RESPONSE_BYTES = 2 * 1024
    const val MAX_NICKNAME_BYTES = 12
    const val MAX_TEXT_BYTES = 64
    const val MIN_FOCUS_MINUTES = 5
    const val MAX_FOCUS_MINUTES = 120
    const val MIN_WALK_TARGET_STEPS = 100L
    const val MAX_WALK_STEPS = 100_000L
    const val MAX_WALK_DISTANCE_METERS = 10_000_000L
    const val MAX_KNOWN_ZONES = 5
    const val MAX_WALK_DECISIONS = 3
    const val MAX_WALK_JOURNAL = 5

    fun profileValidationError(value: CompanionProfile): String? {
        if (!value.ok || value.schema != PET_FEATURE_SCHEMA_VERSION) {
            return "invalid_companion_profile_header"
        }
        if (!validProfileNickname(value.nickname)) {
            return "invalid_companion_profile_nickname"
        }
        with(value.personality) {
            if (warmth !in 0..100 || playfulness !in 0..100 ||
                boldness !in 0..100 || curiosity !in 0..100
            ) return "invalid_companion_profile_personality"
        }
        with(value.bond) {
            val expectedBank = when {
                level >= 5 -> 3
                level >= 3 -> 2
                level >= 1 -> 1
                else -> 0
            }
            if (level !in 0..10 || xp !in 0..630 || speechStage !in 0..3 ||
                dialogueBank != expectedBank
            ) return "invalid_companion_profile_bond"
        }
        if (value.ritual?.days?.let { it !in 3..255 } == true) {
            return "invalid_companion_profile_ritual"
        }
        val preferences = value.preferences
        if (!validPreference(preferences.quietOrPlay) ||
            !validPreference(preferences.dawnOrNight) ||
            !validPreference(preferences.homeOrExplore)
        ) return "invalid_companion_profile_preferences"

        value.checkIn.question?.let { question ->
            val expected = when (question.kind) {
                CompanionQuestionKind.QUIET_OR_PLAY -> "QUIET" to "PLAY"
                CompanionQuestionKind.DAWN_OR_NIGHT -> "DAWN" to "NIGHT"
                CompanionQuestionKind.HOME_OR_EXPLORE -> "HOME" to "EXPLORE"
            }
            if (question.option0 != expected.first || question.option1 != expected.second) {
                return "invalid_companion_profile_question"
            }
        }
        val comfort = value.checkIn.comfort
        val expectedComfort = when (comfort.kind) {
            CompanionComfortKind.NONE -> "I'M ALL RIGHT" to "RIGHT HERE"
            CompanionComfortKind.TIRED -> "A BIT TIRED" to "STAY NEAR"
            CompanionComfortKind.LONELY -> "I MISSED YOU" to "HOLD CLOSE"
            CompanionComfortKind.RESTLESS -> "SO MUCH STATIC" to "HELP ME SETTLE"
        }
        if (comfort.line1 != expectedComfort.first || comfort.line2 != expectedComfort.second) {
            return "invalid_companion_profile_comfort"
        }

        with(value.goal) {
            if (progress !in 0..255 || target !in 1..255) {
                return "invalid_companion_profile_goal"
            }
        }
        with(value.development) {
            if (momentum !in -12..12 || totalActions !in 0L..UINT32_MAX ||
                streakDays !in 0..UINT16_MAX || perfectDays !in 0..UINT16_MAX ||
                bests.dailyActions !in 0..UINT16_MAX || bests.dailyVariety !in 0..8 ||
                bests.careRhythm !in 0..255
            ) return "invalid_companion_profile_development"
        }
        with(value.settings.quietHours) {
            if (startMinute !in 0..1439 || endMinute !in 0..1439) {
                return "invalid_companion_profile_quiet_hours"
            }
        }
        value.latestMemory?.let { memory ->
            if (memory.sequence !in 0..UINT16_MAX || memory.event !in 0..11 ||
                !validText(memory.line1, MAX_TEXT_BYTES, allowEmpty = true) ||
                !validText(memory.line2, MAX_TEXT_BYTES, allowEmpty = true)
            ) return "invalid_companion_profile_memory"
        }
        return null
    }

    fun focusValidationError(value: FocusSessionState): String? {
        if (!value.ok || value.schema != PET_FEATURE_SCHEMA_VERSION) {
            return "invalid_focus_state_header"
        }
        if (value.sessionId !in 0L..UINT32_MAX || value.elapsedMs !in 0L..UINT32_MAX ||
            value.remainingMs !in 0L..UINT32_MAX || value.sequence !in 0L..UINT32_MAX ||
            !validText(value.prompt.title, MAX_TEXT_BYTES, allowEmpty = true) ||
            !validText(value.prompt.detail, MAX_TEXT_BYTES, allowEmpty = true)
        ) return "invalid_focus_state_bounds"

        if (value.sessionId == 0L) {
            return if (
                value.phase == FocusPhase.IDLE && value.completion == FocusCompletion.NONE &&
                value.focusMinutes == 0 && value.breakMinutes == 0 &&
                value.elapsedMs == 0L && value.remainingMs == 0L && value.sequence == 0L &&
                value.prompt == IDLE_FOCUS_PROMPT
            ) null else "invalid_focus_state_fresh"
        }

        if (value.focusMinutes !in MIN_FOCUS_MINUTES..MAX_FOCUS_MINUTES ||
            value.breakMinutes != recommendedBreakMinutes(value.focusMinutes) ||
            value.sequence == 0L
        ) return "invalid_focus_state_session"

        val focusDuration = value.focusMinutes * 60_000L
        val totalDuration = (value.focusMinutes + value.breakMinutes) * 60_000L
        if (value.elapsedMs !in 0L..totalDuration) return "invalid_focus_state_elapsed"

        return when (value.phase) {
            FocusPhase.FOCUS -> if (
                value.completion == FocusCompletion.NONE &&
                value.elapsedMs < focusDuration &&
                value.remainingMs == totalDuration - value.elapsedMs &&
                value.prompt == ACTIVE_FOCUS_PROMPT
            ) null else "invalid_focus_state_focus"

            FocusPhase.BREAK -> if (
                value.completion == FocusCompletion.NONE &&
                value.elapsedMs in focusDuration until totalDuration &&
                value.remainingMs == totalDuration - value.elapsedMs &&
                value.prompt == BREAK_FOCUS_PROMPT
            ) null else "invalid_focus_state_break"

            FocusPhase.COMPLETED -> {
                val validCompletion = when (value.completion) {
                    FocusCompletion.NATURAL -> value.elapsedMs == totalDuration
                    FocusCompletion.STOPPED -> value.elapsedMs < totalDuration
                    FocusCompletion.NONE, FocusCompletion.CANCELLED -> false
                }
                val expectedPrompt = if (value.completion == FocusCompletion.STOPPED) {
                    STOPPED_FOCUS_PROMPT
                } else {
                    COMPLETED_FOCUS_PROMPT
                }
                if (validCompletion && value.remainingMs == 0L && value.prompt == expectedPrompt) {
                    null
                } else {
                    "invalid_focus_state_completed"
                }
            }

            FocusPhase.IDLE -> {
                val validCompletion = when (value.completion) {
                    FocusCompletion.NATURAL -> value.elapsedMs == totalDuration
                    FocusCompletion.STOPPED, FocusCompletion.CANCELLED ->
                        value.elapsedMs < totalDuration
                    FocusCompletion.NONE -> false
                }
                if (validCompletion && value.remainingMs == 0L &&
                    value.prompt == IDLE_FOCUS_PROMPT
                ) null else "invalid_focus_state_idle"
            }
        }
    }

    fun walkValidationError(value: WalkAdventureState): String? {
        if (!value.ok || value.schema != PET_FEATURE_SCHEMA_VERSION) {
            return "invalid_walk_state_header"
        }
        if (value.routeId !in 0L..UINT32_MAX || value.steps !in 0L..MAX_WALK_STEPS ||
            value.targetSteps !in 0L..MAX_WALK_STEPS || value.progressPercent !in 0..100 ||
            value.distanceMeters !in 0L..MAX_WALK_DISTANCE_METERS ||
            value.decisionCount !in 0..MAX_WALK_DECISIONS || value.branch !in 0..255 ||
            !validBranch(value.branch, value.decisionCount) ||
            value.currentZone !in 0L..UINT32_MAX || value.homeZone !in 0L..UINT32_MAX ||
            value.knownZones !in 0..MAX_KNOWN_ZONES ||
            value.totalDistanceMeters !in 0L..UINT32_MAX ||
            value.journalCount !in 0..MAX_WALK_JOURNAL ||
            value.totalDistanceMeters < value.distanceMeters ||
            value.postcard?.let(::validPostcard) == false
        ) return "invalid_walk_state_bounds"

        if (value.privacy == WalkPrivacy.OFF && (
                value.currentZone != 0L || value.homeZone != 0L || value.knownZones != 0 ||
                    value.distanceMeters != 0L || value.totalDistanceMeters != 0L
                )
        ) return "invalid_walk_state_privacy"
        if (value.currentZone != 0L && value.knownZones == 0) {
            return "invalid_walk_state_zones"
        }

        return when (value.phase) {
            WalkPhase.IDLE -> if (
                value.outcome == WalkOutcome.NONE && value.routeId == 0L &&
                value.steps == 0L && value.targetSteps == 0L &&
                value.progressPercent == 0 && value.distanceMeters == 0L &&
                value.terrain == WalkTerrain.MEADOW &&
                value.objective == WalkObjective.EXPLORE &&
                value.risk == WalkRisk.BALANCED && value.weather == WalkWeather.UNKNOWN &&
                value.personality == PetPersonality.GENTLE &&
                value.decisionCount == 0 && value.branch == 0 && value.postcard == null
            ) null else "invalid_walk_state_idle"

            WalkPhase.ACTIVE -> if (
                validLiveRoute(value) && value.postcard == null
            ) null else "invalid_walk_state_active"

            WalkPhase.AWAITING_RESCUE -> if (
                validLiveRoute(value) && value.risk == WalkRisk.BOLD && value.postcard == null
            ) null else "invalid_walk_state_rescue"

            WalkPhase.RETURNED -> if (
                value.routeId in 1L..UINT32_MAX &&
                value.targetSteps in MIN_WALK_TARGET_STEPS..MAX_WALK_STEPS &&
                value.outcome != WalkOutcome.NONE && value.postcard != null &&
                postcardMatchesOutcome(value.postcard, value.outcome) && value.journalCount >= 1
            ) null else "invalid_walk_state_returned"
        }
    }

    fun validNicknameRequest(value: String): Boolean =
        value.length <= MAX_NICKNAME_BYTES && value.all { character ->
            character.code in 0x20..0x7e && character != '"' && character != '\\'
        }

    fun validFocusStart(command: FocusStartCommand): Boolean =
        validOperationId(command.sessionId) &&
            command.minutes in MIN_FOCUS_MINUTES..MAX_FOCUS_MINUTES

    fun validWalkStart(command: WalkStartCommand): Boolean =
        command.targetSteps in MIN_WALK_TARGET_STEPS..MAX_WALK_STEPS

    fun validWalkSync(command: WalkSyncCommand): Boolean =
        validOperationId(command.routeId) && command.stepsTotal in 0L..MAX_WALK_STEPS

    fun validWalkLocation(command: WalkLocationCommand): Boolean =
        validOperationId(command.routeId) && validOperationId(command.zoneToken) &&
            command.stepsTotal in 0L..MAX_WALK_STEPS &&
            command.distanceMetersTotal in 0L..MAX_WALK_DISTANCE_METERS

    fun validWalkDecision(command: WalkDecisionCommand): Boolean =
        validOperationId(command.routeId)

    fun validOperationId(value: Long): Boolean = value in 1L..UINT32_MAX

    fun recommendedBreakMinutes(focusMinutes: Int): Int = when {
        focusMinutes <= 25 -> 5
        focusMinutes <= 50 -> 10
        else -> 15
    }

    private fun validProfileNickname(value: String): Boolean =
        value.length <= MAX_NICKNAME_BYTES && value.all { it.code in 0x20..0x7e }

    private fun validPreference(value: Int?): Boolean = value == null || value in 0..1

    private fun validText(value: String, maxBytes: Int, allowEmpty: Boolean): Boolean {
        if (!allowEmpty && value.isEmpty()) return false
        if (value.any { it.isISOControl() || it in '\uD800'..'\uDFFF' }) return false
        return value.toByteArray(Charsets.UTF_8).size <= maxBytes
    }

    private fun validBranch(branch: Int, decisionCount: Int): Boolean {
        var remaining = branch
        repeat(decisionCount) {
            if (remaining % 5 !in 1..4) return false
            remaining /= 5
        }
        return remaining == 0
    }

    private fun validLiveRoute(value: WalkAdventureState): Boolean =
        value.routeId in 1L..UINT32_MAX &&
            value.targetSteps in MIN_WALK_TARGET_STEPS..MAX_WALK_STEPS &&
            value.outcome == WalkOutcome.NONE

    private fun validPostcard(value: WalkPostcard): Boolean =
        validText(value.title, MAX_TEXT_BYTES, allowEmpty = false) &&
            validText(value.line, MAX_TEXT_BYTES, allowEmpty = false) &&
            value in ALL_POSTCARDS

    private fun postcardMatchesOutcome(postcard: WalkPostcard, outcome: WalkOutcome): Boolean =
        postcard in when (outcome) {
            WalkOutcome.COMPLETE -> COMPLETE_POSTCARDS
            WalkOutcome.PARTIAL -> PARTIAL_POSTCARDS
            WalkOutcome.EARLY_RETURN -> EARLY_POSTCARDS
            WalkOutcome.RESCUED -> RESCUE_POSTCARDS
            WalkOutcome.NONE -> emptySet()
        }

    private val IDLE_FOCUS_PROMPT = FocusPrompt("", "", false)
    private val ACTIVE_FOCUS_PROMPT = FocusPrompt("FOCUS TIME", "STAY WITH IT", false)
    private val BREAK_FOCUS_PROMPT = FocusPrompt(
        "FOCUS COMPLETE",
        "TRY PULSE BREATHING",
        true,
    )
    private val STOPPED_FOCUS_PROMPT = FocusPrompt(
        "SESSION STOPPED",
        "READY WHEN YOU ARE",
        false,
    )
    private val COMPLETED_FOCUS_PROMPT = FocusPrompt("SESSION COMPLETE", "NICE WORK", false)

    private val COMPLETE_POSTCARDS = setOf(
        WalkPostcard("MEADOW NOTE", "THE PATH KEPT GOING"),
        WalkPostcard("FOREST NOTE", "LEAVES HID THE SIGNAL"),
        WalkPostcard("HIGH NOTE", "THE VIEW WAS WORTH IT"),
        WalkPostcard("TOWN NOTE", "I FOUND A NEW CORNER"),
    )
    private val PARTIAL_POSTCARDS = setOf(
        WalkPostcard("HALF A TRAIL", "I BROUGHT BACK A CLUE"),
        WalkPostcard("SMALL DETOUR", "NEXT TIME I GO FARTHER"),
    )
    private val EARLY_POSTCARDS = setOf(
        WalkPostcard("EARLY POST", "HOME WON THIS ROUND"),
        WalkPostcard("SAFE RETURN", "I TOOK THE QUIET WAY"),
    )
    private val RESCUE_POSTCARDS = setOf(
        WalkPostcard("RESCUE ECHO", "A FRIEND FOUND MY SIGNAL"),
        WalkPostcard("PARTY RESCUE", "EVERYONE ANSWERED"),
    )
    private val ALL_POSTCARDS =
        COMPLETE_POSTCARDS + PARTIAL_POSTCARDS + EARLY_POSTCARDS + RESCUE_POSTCARDS
}
