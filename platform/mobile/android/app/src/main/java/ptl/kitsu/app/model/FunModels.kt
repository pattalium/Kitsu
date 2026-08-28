package ptl.kitsu.app.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

const val FUN_STATE_SCHEMA = "kitsu.fun-state.v1"

const val FUN_STATE_GET_OPERATION = "fun.state.get.v1"
const val FUN_EXPEDITION_START_OPERATION = "fun.expedition.start.v1"
const val FUN_EXPEDITION_CLAIM_OPERATION = "fun.expedition.claim.v1"
const val FUN_STORY_START_OPERATION = "fun.story.start.v1"
const val FUN_STORY_ADVANCE_OPERATION = "fun.story.advance.v1"
const val FUN_STORY_CHOOSE_OPERATION = "fun.story.choose.v1"
const val FUN_PARTY_SCAN_OPERATION = "fun.party.scan.v1"
const val FUN_PARTY_HOST_OPERATION = "fun.party.host.v1"
const val FUN_PARTY_JOIN_OPERATION = "fun.party.join.v1"
const val FUN_PARTY_BEGIN_OPERATION = "fun.party.begin.v1"
const val FUN_PARTY_CHOOSE_OPERATION = "fun.party.choose.v1"
const val FUN_PARTY_LEAVE_OPERATION = "fun.party.leave.v1"

@Serializable
enum class ExpeditionDuration {
    @SerialName("short") SHORT,
    @SerialName("medium") MEDIUM,
    @SerialName("long") LONG,
}

@Serializable
enum class ExpeditionStatus {
    @SerialName("idle") IDLE,
    @SerialName("scouting") SCOUTING,
    @SerialName("returned") RETURNED,
}

@Serializable
enum class ExpeditionPersonalityAxis {
    @SerialName("warmth") WARMTH,
    @SerialName("playfulness") PLAYFULNESS,
    @SerialName("boldness") BOLDNESS,
    @SerialName("curiosity") CURIOSITY,
}

@Serializable
data class ExpeditionReport(
    @SerialName("expedition_id") val expeditionId: Long,
    val headline: String,
    val detail: String,
    @SerialName("affection_delta") val affectionDelta: Int,
    @SerialName("personality_axis") val personalityAxis: ExpeditionPersonalityAxis,
    @SerialName("personality_delta") val personalityDelta: Int,
    @SerialName("encounter_catalog_index") val encounterCatalogIndex: Int?,
)

@Serializable
data class ExpeditionFunState(
    val status: ExpeditionStatus,
    val duration: ExpeditionDuration?,
    @SerialName("expedition_id") val expeditionId: Long?,
    @SerialName("total_seconds") val totalSeconds: Long,
    @SerialName("remaining_seconds") val remainingSeconds: Long,
    @SerialName("progress_percent") val progressPercent: Int,
    val report: ExpeditionReport?,
)

@Serializable
enum class StoryStatus {
    @SerialName("idle") IDLE,
    @SerialName("reading") READING,
    @SerialName("choosing") CHOOSING,
}

@Serializable
enum class StoryTrigger {
    @SerialName("quiet") QUIET,
    @SerialName("expedition") EXPEDITION,
    @SerialName("nearby") NEARBY,
}

@Serializable
enum class StoryTone {
    @SerialName("warm") WARM,
    @SerialName("curious") CURIOUS,
    @SerialName("brave") BRAVE,
    @SerialName("playful") PLAYFUL,
    @SerialName("calm") CALM,
}

@Serializable
data class StoryBeat(
    @SerialName("story_id") val storyId: Int,
    val scene: Int,
    val line1: String,
    val line2: String,
    val choices: List<String>,
)

@Serializable
data class StoryResolution(
    @SerialName("story_id") val storyId: Int,
    val line1: String,
    val line2: String,
    val tone: StoryTone,
    @SerialName("affection_delta") val affectionDelta: Int,
    @SerialName("energy_delta") val energyDelta: Int,
    @SerialName("curiosity_delta") val curiosityDelta: Int,
    @SerialName("personality_match") val personalityMatch: Boolean,
)

@Serializable
data class StoryFunState(
    val status: StoryStatus,
    val beat: StoryBeat?,
    val resolution: StoryResolution?,
)

@Serializable
enum class PartyRole {
    @SerialName("none") NONE,
    @SerialName("host") HOST,
    @SerialName("guest") GUEST,
}

@Serializable
enum class PartyPhase {
    @SerialName("idle") IDLE,
    @SerialName("joining") JOINING,
    @SerialName("lobby") LOBBY,
    @SerialName("round") ROUND,
    @SerialName("complete") COMPLETE,
    @SerialName("cancelled") CANCELLED,
    @SerialName("expired") EXPIRED,
    @SerialName("unavailable") UNAVAILABLE,
}

@Serializable
enum class PartySignalChoice {
    @SerialName("none") NONE,
    @SerialName("sweep") SWEEP,
    @SerialName("listen") LISTEN,
    @SerialName("pulse") PULSE,
}

@Serializable
enum class PartyRewardTier {
    @SerialName("none") NONE,
    @SerialName("faded") FADED,
    @SerialName("trace") TRACE,
    @SerialName("found") FOUND,
    @SerialName("resonant") RESONANT,
}

data class PartyJoinCommand(
    val hostDeviceId: String,
    val sessionNonce: Long,
)

data class PartyRoundCommand(
    val round: Int,
    val choice: PartySignalChoice,
)

@Serializable
data class DiscoveredPartyHost(
    @SerialName("host_device_id") val hostDeviceId: String,
    @SerialName("session_nonce") val sessionNonce: Long,
    @SerialName("participant_count") val participantCount: Int,
    @SerialName("join_window_seconds") val joinWindowSeconds: Int,
    val rssi: Double,
    @SerialName("last_seen_age_ms") val lastSeenAgeMs: Long,
)

@Serializable
data class PartyParticipant(
    @SerialName("device_id") val deviceId: String,
    @SerialName("submitted_round") val submittedRound: Int,
    val local: Boolean,
)

@Serializable
data class PartyReward(
    val tier: PartyRewardTier,
    val score: Int,
    @SerialName("maximum_score") val maximumScore: Int,
    @SerialName("bond_awarded") val bondAwarded: Int,
    @SerialName("party_bond") val partyBond: Long,
    @SerialName("eligible_unique_peers") val eligibleUniquePeers: Int,
    @SerialName("current_streak_days") val currentStreakDays: Int,
    @SerialName("longest_streak_days") val longestStreakDays: Int,
)

@Serializable
data class PartyFunState(
    val role: PartyRole,
    val phase: PartyPhase,
    @SerialName("host_device_id") val hostDeviceId: String?,
    @SerialName("session_nonce") val sessionNonce: Long?,
    @SerialName("discovered_hosts") val discoveredHosts: List<DiscoveredPartyHost>,
    @SerialName("participant_count") val participantCount: Int,
    val participants: List<PartyParticipant>,
    val round: Int,
    @SerialName("local_choice") val localChoice: PartySignalChoice,
    val reward: PartyReward,
)

@Serializable
data class FunState(
    val schema: String,
    val expedition: ExpeditionFunState,
    val story: StoryFunState,
    val party: PartyFunState,
)

object FunStatePolicy {
    const val MAX_RESPONSE_BYTES = 2 * 1024
    const val MAX_DISCOVERED_HOSTS = 4
    const val MAX_PARTICIPANTS = 4
    const val MAX_LAST_SEEN_AGE_MS = 120_000L
    const val MAX_JOIN_WINDOW_SECONDS = 600
    const val MAX_LINE_BYTES = 64
    const val MAX_CHOICE_BYTES = 24
    const val STORY_COUNT = 6
    const val UINT16_MAX = 65_535
    const val MAX_PARTY_BOND_AWARD = 14

    fun validationError(value: FunState): String? =
        expeditionError(value.expedition)
            ?: storyError(value.story)
            ?: partyError(value.party)
            ?: if (value.schema != FUN_STATE_SCHEMA) "invalid_fun_schema" else null

    fun validStoryId(value: Int): Boolean = value in 1..STORY_COUNT

    fun validPartyJoin(command: PartyJoinCommand): Boolean =
        EncounterCodePolicy.validDeviceId(command.hostDeviceId) &&
            command.sessionNonce in 1L..EncounterCodePolicy.UINT32_MAX

    fun validPartyRound(command: PartyRoundCommand): Boolean =
        command.round in 1..3 && command.choice != PartySignalChoice.NONE

    private fun expeditionError(value: ExpeditionFunState): String? {
        if (value.totalSeconds !in 0L..28_800L ||
            value.remainingSeconds !in 0L..value.totalSeconds ||
            value.progressPercent !in 0..100 ||
            value.expeditionId?.let(::validUint32Id) == false ||
            value.report?.let(::validReport) == false
        ) return "invalid_fun_expedition"

        val expectedSeconds = when (value.duration) {
            null -> null
            ExpeditionDuration.SHORT -> 900L
            ExpeditionDuration.MEDIUM -> 7_200L
            ExpeditionDuration.LONG -> 28_800L
        }
        return when (value.status) {
            ExpeditionStatus.IDLE -> if (
                value.duration != null || value.expeditionId != null ||
                value.totalSeconds != 0L || value.remainingSeconds != 0L ||
                value.progressPercent != 0
            ) "invalid_fun_expedition_idle" else null

            ExpeditionStatus.SCOUTING -> if (
                value.expeditionId == null || expectedSeconds != value.totalSeconds ||
                value.remainingSeconds == 0L || value.progressPercent >= 100 || value.report != null
            ) "invalid_fun_expedition_scouting" else null

            ExpeditionStatus.RETURNED -> if (
                value.expeditionId == null || expectedSeconds != value.totalSeconds ||
                value.remainingSeconds != 0L || value.progressPercent != 100 ||
                value.report?.expeditionId != value.expeditionId
            ) "invalid_fun_expedition_returned" else null
        }
    }

    private fun validReport(value: ExpeditionReport): Boolean =
        validUint32Id(value.expeditionId) && validText(value.headline, allowEmpty = false) &&
            validText(value.detail, allowEmpty = false) && value.affectionDelta in 1..3 &&
            value.personalityDelta in 1..2 && value.encounterCatalogIndex?.let { it in 0..20 } != false

    private fun storyError(value: StoryFunState): String? {
        if (value.beat?.let(::validBeat) == false || value.resolution?.let(::validResolution) == false) {
            return "invalid_fun_story"
        }
        return when (value.status) {
            StoryStatus.IDLE -> if (value.beat != null) "invalid_fun_story_idle" else null
            StoryStatus.READING -> if (
                value.beat?.scene != 0 || value.beat.choices.isNotEmpty() || value.resolution != null
            ) "invalid_fun_story_reading" else null
            StoryStatus.CHOOSING -> if (
                value.beat?.scene != 1 || value.beat.choices.size != 3 || value.resolution != null
            ) "invalid_fun_story_choosing" else null
        }
    }

    private fun validBeat(value: StoryBeat): Boolean =
        validStoryId(value.storyId) && value.scene in 0..1 &&
            validText(value.line1, allowEmpty = false) && validText(value.line2, allowEmpty = true) &&
            value.choices.size <= 3 && value.choices.distinct().size == value.choices.size &&
            value.choices.all(::validChoiceText)

    private fun validResolution(value: StoryResolution): Boolean =
        validStoryId(value.storyId) && validText(value.line1, allowEmpty = false) &&
            validText(value.line2, allowEmpty = true) && value.affectionDelta in 1..2 &&
            value.energyDelta in -1..1 && value.curiosityDelta in 0..2

    private fun partyError(value: PartyFunState): String? {
        val reward = value.reward
        if (value.discoveredHosts.size > MAX_DISCOVERED_HOSTS ||
            value.discoveredHosts.any { !validHost(it) } ||
            value.discoveredHosts.map { it.hostDeviceId }.distinct().size != value.discoveredHosts.size ||
            value.participantCount !in 0..MAX_PARTICIPANTS ||
            value.participants.size > MAX_PARTICIPANTS ||
            value.participants.any { !validParticipant(it) } ||
            value.participants.map { it.deviceId }.distinct().size != value.participants.size ||
            value.round !in 0..3 || !validReward(reward)
        ) return "invalid_fun_party"

        if (value.role == PartyRole.NONE) {
            if (value.phase !in setOf(PartyPhase.IDLE, PartyPhase.UNAVAILABLE) ||
                value.hostDeviceId != null || value.sessionNonce != null ||
                value.participantCount != 0 || value.participants.isNotEmpty() ||
                value.round != 0 || value.localChoice != PartySignalChoice.NONE
            ) return "invalid_fun_party_idle"
        } else {
            if (value.hostDeviceId?.let(EncounterCodePolicy::validDeviceId) != true ||
                value.sessionNonce !in 1L..EncounterCodePolicy.UINT32_MAX ||
                value.phase in setOf(PartyPhase.IDLE, PartyPhase.UNAVAILABLE) ||
                value.participantCount !in 1..MAX_PARTICIPANTS ||
                value.participants.size !in 1..value.participantCount ||
                value.participants.count { it.local } != 1 ||
                (value.role == PartyRole.HOST && value.participants.size != value.participantCount) ||
                (value.role == PartyRole.HOST && value.phase == PartyPhase.JOINING)
            ) return "invalid_fun_party_session"
        }

        when (value.phase) {
            PartyPhase.IDLE, PartyPhase.UNAVAILABLE -> Unit
            PartyPhase.JOINING, PartyPhase.LOBBY -> if (
                value.round != 0 || value.localChoice != PartySignalChoice.NONE
            ) return "invalid_fun_party_lobby"
            PartyPhase.ROUND -> if (value.round !in 1..3) return "invalid_fun_party_round"
            PartyPhase.COMPLETE -> if (
                value.round != 3 || value.participantCount !in 2..MAX_PARTICIPANTS ||
                reward.tier == PartyRewardTier.NONE || reward.maximumScore == 0
            ) return "invalid_fun_party_complete"
            PartyPhase.CANCELLED, PartyPhase.EXPIRED -> Unit
        }

        if (value.phase != PartyPhase.COMPLETE && (
                reward.tier != PartyRewardTier.NONE || reward.score != 0 ||
                    reward.maximumScore != 0 || reward.bondAwarded != 0 ||
                    reward.eligibleUniquePeers != 0
                )
        ) return "invalid_fun_party_reward_phase"
        return null
    }

    private fun validHost(value: DiscoveredPartyHost): Boolean =
        EncounterCodePolicy.validDeviceId(value.hostDeviceId) &&
            value.sessionNonce in 1L..EncounterCodePolicy.UINT32_MAX &&
            value.participantCount in 1..MAX_PARTICIPANTS &&
            value.joinWindowSeconds in 1..MAX_JOIN_WINDOW_SECONDS &&
            value.rssi.isFinite() && value.rssi in -200.0..0.0 &&
            value.lastSeenAgeMs in 0L..MAX_LAST_SEEN_AGE_MS

    private fun validParticipant(value: PartyParticipant): Boolean =
        EncounterCodePolicy.validDeviceId(value.deviceId) && value.submittedRound in 0..3

    private fun validReward(value: PartyReward): Boolean =
        value.score in 0..UINT16_MAX && value.maximumScore in 0..UINT16_MAX &&
            value.score <= value.maximumScore && value.bondAwarded in 0..MAX_PARTY_BOND_AWARD &&
            value.partyBond in 0L..EncounterCodePolicy.UINT32_MAX &&
            value.eligibleUniquePeers in 0..3 &&
            value.currentStreakDays in 0..UINT16_MAX &&
            value.longestStreakDays in value.currentStreakDays..UINT16_MAX &&
            (value.eligibleUniquePeers != 0 || value.bondAwarded == 0)

    private fun validUint32Id(value: Long): Boolean = value in 1L..EncounterCodePolicy.UINT32_MAX

    private fun validText(value: String, allowEmpty: Boolean): Boolean {
        val bytes = value.toByteArray(Charsets.UTF_8)
        return (allowEmpty || value.isNotEmpty()) && bytes.size <= MAX_LINE_BYTES &&
            value.none(Char::isISOControl)
    }

    private fun validChoiceText(value: String): Boolean {
        val bytes = value.toByteArray(Charsets.UTF_8)
        return value.isNotEmpty() && bytes.size <= MAX_CHOICE_BYTES && value.none(Char::isISOControl)
    }
}
