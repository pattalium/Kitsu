package ptl.kitsu.app.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

const val PET_FEATURE_SCHEMA_VERSION = 1

const val COMPANION_PROFILE_GET_OPERATION = "companion.profile.get.v1"
const val COMPANION_NICKNAME_SET_OPERATION = "companion.profile.nickname.set.v1"
const val COMPANION_REQUEST_ANSWER_OPERATION = "companion.request.answer.v1"
const val COMPANION_QUESTION_ANSWER_OPERATION = "companion.question.answer.v1"

const val FOCUS_STATE_GET_OPERATION = "focus.state.get.v1"
const val FOCUS_START_OPERATION = "focus.start.v1"
const val FOCUS_STOP_OPERATION = "focus.stop.v1"
const val FOCUS_CANCEL_OPERATION = "focus.cancel.v1"
const val FOCUS_ACK_OPERATION = "focus.ack.v1"

const val ADVENTURE_STATE_GET_OPERATION = "adventure.state.get.v1"
const val ADVENTURE_WALK_START_OPERATION = "adventure.walk.start.v1"
const val ADVENTURE_WALK_SYNC_OPERATION = "adventure.walk.sync.v1"
const val ADVENTURE_WALK_LOCATION_OPERATION = "adventure.walk.location.v1"
const val ADVENTURE_WALK_DECIDE_OPERATION = "adventure.walk.decide.v1"
const val ADVENTURE_WALK_FINISH_OPERATION = "adventure.walk.finish.v1"
const val ADVENTURE_WALK_ACK_OPERATION = "adventure.walk.ack.v1"
const val ADVENTURE_PRIVACY_SET_OPERATION = "adventure.privacy.set.v1"
const val ADVENTURE_HOME_SET_OPERATION = "adventure.home.set.v1"

@Serializable
enum class PetPersonality {
    @SerialName("GENTLE") GENTLE,
    @SerialName("BOLD") BOLD,
    @SerialName("CURIOUS") CURIOUS,
    @SerialName("PLAYFUL") PLAYFUL,
    @SerialName("SHY") SHY,
    @SerialName("IMPISH") IMPISH,
}

@Serializable
enum class CompanionMood {
    @SerialName("CONTENT") CONTENT,
    @SerialName("DREAMING") DREAMING,
    @SerialName("LISTENING") LISTENING,
    @SerialName("DROWSY") DROWSY,
    @SerialName("LONELY") LONELY,
    @SerialName("CURIOUS") CURIOUS,
    @SerialName("EXCITED") EXCITED,
    @SerialName("DEVOTED") DEVOTED,
    @SerialName("IMPISH") IMPISH,
    @SerialName("LOVED") LOVED,
    @SerialName("SATISFIED") SATISFIED,
    @SerialName("PLAYFUL") PLAYFUL,
    @SerialName("PROUD") PROUD,
    @SerialName("STARTLED") STARTLED,
    @SerialName("AWAKE") AWAKE,
}

@Serializable
enum class CompanionAction {
    @SerialName("pet") PET,
    @SerialName("feed") FEED,
    @SerialName("play") PLAY,
    @SerialName("listen") LISTEN,
    @SerialName("sleep") SLEEP,
    @SerialName("wake") WAKE,
    @SerialName("meet") MEET,
    @SerialName("gift") GIFT,
}

@Serializable
enum class CompanionTimeBucket {
    @SerialName("morning") MORNING,
    @SerialName("day") DAY,
    @SerialName("evening") EVENING,
    @SerialName("night") NIGHT,
}

@Serializable
enum class CompanionRequestState {
    @SerialName("none") NONE,
    @SerialName("pending") PENDING,
    @SerialName("accepted") ACCEPTED,
    @SerialName("declined") DECLINED,
    @SerialName("completed") COMPLETED,
}

@Serializable
enum class CompanionQuestionKind {
    @SerialName("quiet_or_play") QUIET_OR_PLAY,
    @SerialName("dawn_or_night") DAWN_OR_NIGHT,
    @SerialName("home_or_explore") HOME_OR_EXPLORE,
}

@Serializable
enum class CompanionComfortKind {
    @SerialName("none") NONE,
    @SerialName("tired") TIRED,
    @SerialName("lonely") LONELY,
    @SerialName("restless") RESTLESS,
}

@Serializable
enum class CompanionGoalKind {
    @SerialName("care") CARE,
    @SerialName("variety") VARIETY,
    @SerialName("favorite") FAVORITE,
}

@Serializable
enum class CompanionQuickAction {
    @SerialName("pet") PET,
    @SerialName("feed") FEED,
    @SerialName("play") PLAY,
    @SerialName("listen") LISTEN,
    @SerialName("daily") DAILY,
    @SerialName("expedition") EXPEDITION,
}

@Serializable
data class CompanionPersonality(
    val kind: PetPersonality,
    val warmth: Int,
    val playfulness: Int,
    val boldness: Int,
    val curiosity: Int,
)

@Serializable
data class CompanionBond(
    val level: Int,
    val xp: Int,
    @SerialName("speech_stage") val speechStage: Int,
    @SerialName("dialogue_bank") val dialogueBank: Int,
)

@Serializable
data class CompanionFavorite(
    val action: CompanionAction,
    val time: CompanionTimeBucket?,
)

@Serializable
data class CompanionRoutine(
    val action: CompanionAction,
    val time: CompanionTimeBucket,
)

@Serializable
data class CompanionRitual(
    val action: CompanionAction,
    val time: CompanionTimeBucket,
    val days: Int,
)

@Serializable
data class CompanionPreferences(
    @SerialName("quiet_or_play") val quietOrPlay: Int?,
    @SerialName("dawn_or_night") val dawnOrNight: Int?,
    @SerialName("home_or_explore") val homeOrExplore: Int?,
)

@Serializable
data class CompanionRequest(
    val state: CompanionRequestState,
    val action: CompanionAction,
)

@Serializable
data class CompanionQuestion(
    val kind: CompanionQuestionKind,
    val option0: String,
    val option1: String,
)

@Serializable
data class CompanionComfort(
    val kind: CompanionComfortKind,
    val line1: String,
    val line2: String,
)

@Serializable
data class CompanionCheckIn(
    val request: CompanionRequest,
    val question: CompanionQuestion?,
    val comfort: CompanionComfort,
    @SerialName("callback_ready") val callbackReady: Boolean,
)

@Serializable
data class CompanionDailyGoal(
    val kind: CompanionGoalKind,
    val action: CompanionAction,
    val progress: Int,
    val target: Int,
)

@Serializable
data class CompanionPersonalBests(
    @SerialName("daily_actions") val dailyActions: Int,
    @SerialName("daily_variety") val dailyVariety: Int,
    @SerialName("care_rhythm") val careRhythm: Int,
)

@Serializable
data class CompanionDevelopment(
    val momentum: Int,
    @SerialName("total_actions") val totalActions: Long,
    @SerialName("streak_days") val streakDays: Int,
    @SerialName("perfect_days") val perfectDays: Int,
    val bests: CompanionPersonalBests,
)

@Serializable
data class CompanionQuietHours(
    val enabled: Boolean,
    @SerialName("start_minute") val startMinute: Int,
    @SerialName("end_minute") val endMinute: Int,
)

@Serializable
data class CompanionSettings(
    @SerialName("quick_action") val quickAction: CompanionQuickAction,
    @SerialName("quiet_hours") val quietHours: CompanionQuietHours,
)

@Serializable
data class CompanionMemory(
    val sequence: Int,
    val event: Int,
    val line1: String,
    val line2: String,
)

@Serializable
data class CompanionProfile(
    val ok: Boolean,
    val schema: Int,
    val nickname: String,
    val personality: CompanionPersonality,
    val mood: CompanionMood,
    val bond: CompanionBond,
    val favorite: CompanionFavorite?,
    val routine: CompanionRoutine?,
    val ritual: CompanionRitual?,
    val preferences: CompanionPreferences,
    @SerialName("check_in") val checkIn: CompanionCheckIn,
    val goal: CompanionDailyGoal,
    val development: CompanionDevelopment,
    val settings: CompanionSettings,
    @SerialName("latest_memory") val latestMemory: CompanionMemory?,
)

data class FocusStartCommand(
    val sessionId: Long,
    val minutes: Int,
)

@Serializable
enum class FocusPhase {
    @SerialName("idle") IDLE,
    @SerialName("focus") FOCUS,
    @SerialName("break") BREAK,
    @SerialName("completed") COMPLETED,
}

@Serializable
enum class FocusCompletion {
    @SerialName("none") NONE,
    @SerialName("natural") NATURAL,
    @SerialName("stopped") STOPPED,
    @SerialName("cancelled") CANCELLED,
}

@Serializable
data class FocusPrompt(
    val title: String,
    val detail: String,
    @SerialName("recommend_pulse_breathing") val recommendPulseBreathing: Boolean,
)

@Serializable
data class FocusSessionState(
    val ok: Boolean,
    val schema: Int,
    val phase: FocusPhase,
    val completion: FocusCompletion,
    @SerialName("session_id") val sessionId: Long,
    @SerialName("focus_minutes") val focusMinutes: Int,
    @SerialName("break_minutes") val breakMinutes: Int,
    @SerialName("elapsed_ms") val elapsedMs: Long,
    @SerialName("remaining_ms") val remainingMs: Long,
    val sequence: Long,
    val prompt: FocusPrompt,
)

@Serializable
enum class WalkTerrain {
    @SerialName("meadow") MEADOW,
    @SerialName("forest") FOREST,
    @SerialName("ridge") RIDGE,
    @SerialName("waterfront") WATERFRONT,
    @SerialName("town") TOWN,
}

@Serializable
enum class WalkObjective {
    @SerialName("explore") EXPLORE,
    @SerialName("follow_signal") FOLLOW_SIGNAL,
    @SerialName("meet_creature") MEET_CREATURE,
    @SerialName("community") COMMUNITY,
    @SerialName("return_home") RETURN_HOME,
}

@Serializable
enum class WalkRisk {
    @SerialName("careful") CAREFUL,
    @SerialName("balanced") BALANCED,
    @SerialName("bold") BOLD,
}

@Serializable
enum class WalkWeather {
    @SerialName("unknown") UNKNOWN,
    @SerialName("clear") CLEAR,
    @SerialName("rain") RAIN,
    @SerialName("wind") WIND,
    @SerialName("snow") SNOW,
}

@Serializable
enum class WalkPhase {
    @SerialName("idle") IDLE,
    @SerialName("active") ACTIVE,
    @SerialName("awaiting_rescue") AWAITING_RESCUE,
    @SerialName("returned") RETURNED,
}

@Serializable
enum class WalkOutcome {
    @SerialName("none") NONE,
    @SerialName("partial") PARTIAL,
    @SerialName("complete") COMPLETE,
    @SerialName("early_return") EARLY_RETURN,
    @SerialName("rescued") RESCUED,
}

@Serializable
enum class WalkPrivacy {
    @SerialName("off") OFF,
    @SerialName("coarse") COARSE,
    @SerialName("precise_transient") PRECISE_TRANSIENT,
}

@Serializable
enum class WalkDecision {
    @SerialName("continue") CONTINUE,
    @SerialName("detour") DETOUR,
    @SerialName("help") HELP,
    @SerialName("return") RETURN,
}

@Serializable
data class WalkPostcard(
    val title: String,
    val line: String,
)

@Serializable
data class WalkAdventureState(
    val ok: Boolean,
    val schema: Int,
    val phase: WalkPhase,
    val outcome: WalkOutcome,
    @SerialName("route_id") val routeId: Long,
    val steps: Long,
    @SerialName("target_steps") val targetSteps: Long,
    @SerialName("progress_percent") val progressPercent: Int,
    @SerialName("distance_meters") val distanceMeters: Long,
    val terrain: WalkTerrain,
    val objective: WalkObjective,
    val risk: WalkRisk,
    val weather: WalkWeather,
    val personality: PetPersonality,
    @SerialName("decision_count") val decisionCount: Int,
    val branch: Int,
    val privacy: WalkPrivacy,
    @SerialName("current_zone") val currentZone: Long,
    @SerialName("home_zone") val homeZone: Long,
    @SerialName("known_zones") val knownZones: Int,
    @SerialName("total_distance_meters") val totalDistanceMeters: Long,
    @SerialName("journal_count") val journalCount: Int,
    val postcard: WalkPostcard?,
)

data class WalkStartCommand(
    val terrain: WalkTerrain,
    val objective: WalkObjective,
    val risk: WalkRisk,
    val weather: WalkWeather,
    val targetSteps: Long,
    val commuteSafe: Boolean,
)

data class WalkSyncCommand(
    val routeId: Long,
    val stepsTotal: Long,
)

data class WalkLocationCommand(
    val routeId: Long,
    val zoneToken: Long,
    val stepsTotal: Long,
    val distanceMetersTotal: Long,
)

data class WalkDecisionCommand(
    val routeId: Long,
    val decision: WalkDecision,
)
