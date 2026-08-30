package ptl.kitsu.app.widget

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.max
import ptl.kitsu.app.model.CompanionAction
import ptl.kitsu.app.model.CompanionCheckIn
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.CompanionRequestState
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkPhase

/** The widget receives snapshots only. It never owns a transport or initiates an operation. */
data class KitsuStatusWidgetSource(
    val status: KitsuStatus?,
    val connected: Boolean,
    val snapshotAtEpochSeconds: Long?,
    val companionProfile: CompanionProfile? = null,
    val companionCheckIn: CompanionCheckIn? = companionProfile?.checkIn,
    val focusSession: FocusSessionState? = null,
    val walkAdventure: WalkAdventureState? = null,
)

data class KitsuStatusWidgetPresentation(
    val petName: String,
    val moodText: String,
    val energyText: String,
    val energyPercent: Int,
    val batteryText: String,
    val freshnessText: String,
    val portraitPackId: Long?,
    val requestText: String?,
    val focusText: String?,
    val walkText: String?,
) {
    val contentDescription: String = listOfNotNull(
        petName,
        moodText,
        energyText,
        batteryText,
        freshnessText,
        requestText,
        focusText,
        walkText,
    ).joinToString(". ")
}

object KitsuStatusWidgetPresentationPolicy {
    fun present(
        source: KitsuStatusWidgetSource,
        nowEpochSeconds: Long = System.currentTimeMillis() / 1_000L,
        zoneId: ZoneId = ZoneId.systemDefault(),
        locale: Locale = Locale.getDefault(),
    ): KitsuStatusWidgetPresentation {
        val status = source.status
        val profile = source.companionProfile
        val petName = profile?.nickname?.takeIf(String::isNotBlank)
            ?: status?.companionName?.takeIf(String::isNotBlank)
            ?: "Your Kitsu"
        val mood = profile?.mood?.name ?: status?.mood
        val energy = status?.needs?.energy?.coerceIn(0, 100) ?: 0
        val observedAt = source.snapshotAtEpochSeconds?.takeIf { it > 0L }
            ?: status?.updatedAt?.takeIf { it > 0L }

        return KitsuStatusWidgetPresentation(
            petName = petName,
            moodText = moodText(mood),
            energyText = status?.let { "Energy $energy%" } ?: "Energy —",
            energyPercent = energy,
            batteryText = batteryText(status),
            freshnessText = freshnessText(
                connected = source.connected,
                observedAt = observedAt,
                nowEpochSeconds = nowEpochSeconds,
                zoneId = zoneId,
                locale = locale,
            ),
            portraitPackId = status?.packId?.toLongOrNull()?.takeIf { status.packReady },
            requestText = requestText(source.companionCheckIn),
            focusText = focusText(source.focusSession),
            walkText = walkText(source.walkAdventure),
        )
    }

    private fun moodText(raw: String?): String = when {
        raw.isNullOrBlank() || raw.equals("UNKNOWN", ignoreCase = true) -> "Mood unknown"
        raw.equals("SLEEPING", ignoreCase = true) -> "Mood · Sleeping"
        else -> "Mood · ${humanLabel(raw)}"
    }

    private fun batteryText(status: KitsuStatus?): String = when {
        status == null -> "Battery —"
        status.batteryPercent != null -> "Battery ${status.batteryPercent.coerceIn(0, 100)}%"
        status.batteryMillivolts != null -> String.format(
            Locale.ROOT,
            "Battery %.2f V",
            status.batteryMillivolts.coerceAtLeast(0) / 1_000.0,
        )
        else -> "Battery —"
    }

    private fun freshnessText(
        connected: Boolean,
        observedAt: Long?,
        nowEpochSeconds: Long,
        zoneId: ZoneId,
        locale: Locale,
    ): String {
        if (observedAt == null) {
            return if (connected) "Connected · waiting for status" else "Open Kitsu to load status"
        }
        val age = (nowEpochSeconds - observedAt).coerceAtLeast(0L)
        val relative = when {
            age < 60L -> "just now"
            age < 3_600L -> "${max(1L, age / 60L)}m ago"
            age < 86_400L -> "${age / 3_600L}h ago"
            age < 604_800L -> "${age / 86_400L}d ago"
            else -> DateTimeFormatter.ofPattern("MMM d, HH:mm", locale)
                .withZone(zoneId)
                .format(Instant.ofEpochSecond(observedAt))
        }
        return if (connected) "Connected · updated $relative" else "Last synced $relative"
    }

    private fun requestText(checkIn: CompanionCheckIn?): String? {
        val request = checkIn?.request ?: return null
        if (request.state != CompanionRequestState.PENDING) return null
        return "Request · ${actionLabel(request.action)}"
    }

    private fun focusText(state: FocusSessionState?): String? = when (state?.phase) {
        FocusPhase.FOCUS -> "Focus · ${remainingMinutes(state.remainingMs)}m left"
        FocusPhase.BREAK -> "Break · ${remainingMinutes(state.remainingMs)}m left"
        FocusPhase.COMPLETED -> "Focus complete"
        FocusPhase.IDLE, null -> null
    }

    private fun walkText(state: WalkAdventureState?): String? = when (state?.phase) {
        WalkPhase.ACTIVE -> "Walk · ${state.progressPercent.coerceIn(0, 100)}% · ${state.steps.coerceAtLeast(0L)} steps"
        WalkPhase.AWAITING_RESCUE -> "Walk needs help"
        WalkPhase.RETURNED -> "Walk returned · ${humanLabel(state.outcome.name)}"
        WalkPhase.IDLE, null -> null
    }

    private fun remainingMinutes(remainingMs: Long): Long =
        max(1L, (remainingMs.coerceAtLeast(0L) + 59_999L) / 60_000L)

    private fun actionLabel(action: CompanionAction): String = when (action) {
        CompanionAction.PET -> "Pet me"
        CompanionAction.FEED -> "Feed me"
        CompanionAction.PLAY -> "Play"
        CompanionAction.LISTEN -> "Listen"
        CompanionAction.SLEEP -> "Sleep"
        CompanionAction.WAKE -> "Wake me"
        CompanionAction.MEET -> "Meet"
        // The wire value stays compatible while owner-facing copy remains neutral.
        CompanionAction.GIFT -> "Spend time together"
    }

    private fun humanLabel(raw: String): String = raw.trim()
        .lowercase(Locale.ROOT)
        .replace('_', ' ')
        .replaceFirstChar { character ->
            if (character.isLowerCase()) character.titlecase(Locale.ROOT) else character.toString()
        }
}
