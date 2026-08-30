package ptl.kitsu.app.notifications

import java.util.Locale
import ptl.kitsu.app.model.CompanionComfortKind
import ptl.kitsu.app.model.CompanionAction
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.CompanionRequestState
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MeshPeerKeyPolicy
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkPhase

data class DirectMessageNotificationSnapshot(
    val deviceAddress: String?,
    val connected: Boolean,
    val journalSession: String?,
    val messages: List<Message>,
    val blockedPeerIds: Set<String> = emptySet(),
)

data class DirectMessageNotificationCursor(
    val binding: String? = null,
    val observedFingerprints: List<String> = emptyList(),
)

data class DirectMessageNotificationEvaluation(
    val cursor: DirectMessageNotificationCursor,
    val newMessages: List<Message>,
)

/**
 * Treats the first authenticated journal snapshot as a baseline, not as new mail.
 * A transient disconnect keeps the baseline, while a device or boot-session change
 * establishes a new one. This prevents both cold-cache replay and cross-device leaks.
 */
object DirectMessageNotificationPolicy {
    const val MAX_OBSERVED_FINGERPRINTS = 512
    const val MAX_NOTIFICATIONS_PER_EVALUATION = 4

    fun evaluate(
        cursor: DirectMessageNotificationCursor,
        snapshot: DirectMessageNotificationSnapshot,
    ): DirectMessageNotificationEvaluation {
        if (!snapshot.connected) return DirectMessageNotificationEvaluation(cursor, emptyList())
        val address = snapshot.deviceAddress?.trim()?.uppercase(Locale.ROOT)
            ?.takeIf(String::isNotEmpty)
            ?: return DirectMessageNotificationEvaluation(cursor, emptyList())
        val session = snapshot.journalSession?.takeIf(String::isNotBlank)
            ?: return DirectMessageNotificationEvaluation(cursor, emptyList())
        val binding = "$address\u0000$session"
        val eligible = snapshot.messages.asSequence()
            .filter { it.direction.equals("inbound", ignoreCase = true) }
            .filter { message ->
                message.peerId?.let(MeshPeerKeyPolicy::isCanonicalBase64Url) == true &&
                    (message.route == null || message.route.equals("direct", ignoreCase = true)) &&
                    message.channel == null && message.journalSession == session
            }
            .sortedWith(compareBy<Message>({ it.occurredAt }, { it.id }))
            .toList()
        val fingerprints = eligible.map { message -> "$session:${message.id}" }
        if (cursor.binding != binding) {
            return DirectMessageNotificationEvaluation(
                cursor = DirectMessageNotificationCursor(
                    binding = binding,
                    observedFingerprints = fingerprints.takeLast(MAX_OBSERVED_FINGERPRINTS),
                ),
                newMessages = emptyList(),
            )
        }

        val observed = cursor.observedFingerprints.toHashSet()
        val fresh = eligible.filter { message ->
            "$session:${message.id}" !in observed && message.peerId !in snapshot.blockedPeerIds
        }
            .takeLast(MAX_NOTIFICATIONS_PER_EVALUATION)
        val nextObserved = (cursor.observedFingerprints + fingerprints)
            .distinct()
            .takeLast(MAX_OBSERVED_FINGERPRINTS)
        return DirectMessageNotificationEvaluation(
            cursor = cursor.copy(observedFingerprints = nextObserved),
            newMessages = fresh,
        )
    }
}

enum class PetNotificationKind { CHECK_IN, FOCUS, WALK }

data class PetNotificationEvent(
    val kind: PetNotificationKind,
    val title: String,
    val text: String,
    val fingerprint: String,
)

data class PetNotificationSnapshot(
    val deviceAddress: String?,
    val connected: Boolean,
    val profile: CompanionProfile?,
    val focus: FocusSessionState?,
    val walk: WalkAdventureState?,
    /** True only after the connected device's live companion feature read completed. */
    val liveStateReady: Boolean = true,
)

data class PetNotificationCursor(
    val deviceBinding: String? = null,
    val fingerprints: Map<PetNotificationKind, String> = emptyMap(),
)

data class PetNotificationEvaluation(
    val cursor: PetNotificationCursor,
    val events: List<PetNotificationEvent>,
)

/** Actionable transition detector for future companion/focus/walk state integration. */
object PetNotificationPolicy {
    fun evaluate(
        cursor: PetNotificationCursor,
        snapshot: PetNotificationSnapshot,
    ): PetNotificationEvaluation {
        if (!snapshot.connected || !snapshot.liveStateReady) {
            return PetNotificationEvaluation(cursor, emptyList())
        }
        val binding = snapshot.deviceAddress?.trim()?.uppercase(Locale.ROOT)
            ?.takeIf(String::isNotEmpty)
            ?: return PetNotificationEvaluation(cursor, emptyList())
        val currentEvents = listOfNotNull(
            checkInEvent(snapshot.profile),
            focusEvent(snapshot.profile, snapshot.focus),
            walkEvent(snapshot.profile, snapshot.walk),
        )
        val currentFingerprints = currentEvents.associate { it.kind to it.fingerprint }
        if (cursor.deviceBinding != binding) {
            return PetNotificationEvaluation(
                cursor = PetNotificationCursor(binding, currentFingerprints),
                events = emptyList(),
            )
        }
        val events = currentEvents.filter { event ->
            cursor.fingerprints[event.kind] != event.fingerprint
        }
        return PetNotificationEvaluation(
            cursor = PetNotificationCursor(binding, currentFingerprints),
            events = events,
        )
    }

    private fun checkInEvent(profile: CompanionProfile?): PetNotificationEvent? {
        profile ?: return null
        val name = profile.nickname.takeIf(String::isNotBlank) ?: "Your Kitsu"
        val checkIn = profile.checkIn
        val request = checkIn.request
        return when {
            request.state == CompanionRequestState.PENDING -> PetNotificationEvent(
                PetNotificationKind.CHECK_IN,
                "$name has a request",
                "${actionLabel(request.action)} when you’re ready.",
                "request:${request.action.name}",
            )
            checkIn.question != null -> PetNotificationEvent(
                PetNotificationKind.CHECK_IN,
                "$name has a question",
                "Open Kitsu to answer privately.",
                "question:${checkIn.question.kind.name}:${checkIn.question.option0}:${checkIn.question.option1}",
            )
            checkIn.comfort.kind != CompanionComfortKind.NONE -> PetNotificationEvent(
                PetNotificationKind.CHECK_IN,
                "$name could use you",
                checkIn.comfort.line1.takeIf(String::isNotBlank) ?: "Open Kitsu to check in.",
                "comfort:${checkIn.comfort.kind.name}:${checkIn.comfort.line1}:${checkIn.comfort.line2}",
            )
            checkIn.callbackReady -> PetNotificationEvent(
                PetNotificationKind.CHECK_IN,
                "$name checked back in",
                "Open Kitsu to see what changed.",
                "callback:${profile.latestMemory?.sequence ?: profile.development.totalActions}",
            )
            else -> null
        }
    }

    private fun focusEvent(
        profile: CompanionProfile?,
        state: FocusSessionState?,
    ): PetNotificationEvent? {
        state ?: return null
        val name = profile?.nickname?.takeIf(String::isNotBlank) ?: "Your Kitsu"
        val (title, text) = when (state.phase) {
            FocusPhase.FOCUS -> "$name started focus time" to state.prompt.detail
            FocusPhase.BREAK -> "Time for a break" to "$name is taking a break with you."
            FocusPhase.COMPLETED -> "Focus session complete" to "Open Kitsu when you are ready."
            FocusPhase.IDLE -> return null
        }
        return PetNotificationEvent(
            kind = PetNotificationKind.FOCUS,
            title = title,
            text = text.takeIf(String::isNotBlank) ?: "Open Kitsu for the latest focus update.",
            fingerprint = "focus:${state.sessionId}:${state.phase.name}:${state.completion.name}",
        )
    }

    private fun walkEvent(
        profile: CompanionProfile?,
        state: WalkAdventureState?,
    ): PetNotificationEvent? {
        state ?: return null
        val name = profile?.nickname?.takeIf(String::isNotBlank) ?: "Your Kitsu"
        val (title, text) = when (state.phase) {
            WalkPhase.ACTIVE -> "$name started a walk" to "${state.steps.coerceAtLeast(0L)} steps so far."
            WalkPhase.AWAITING_RESCUE -> "$name needs help on the walk" to "Open Kitsu to choose what happens next."
            WalkPhase.RETURNED -> "$name returned" to "Walk ${humanLabel(state.outcome.name).lowercase(Locale.ROOT)}."
            WalkPhase.IDLE -> return null
        }
        return PetNotificationEvent(
            kind = PetNotificationKind.WALK,
            title = title,
            text = text,
            fingerprint = "walk:${state.routeId}:${state.phase.name}:${state.outcome.name}",
        )
    }

    private fun humanLabel(raw: String): String = raw.lowercase(Locale.ROOT)
        .replace('_', ' ')
        .replaceFirstChar { if (it.isLowerCase()) it.titlecase(Locale.ROOT) else it.toString() }

    private fun actionLabel(action: CompanionAction): String = when (action) {
        CompanionAction.PET -> "Pet me"
        CompanionAction.FEED -> "Care for me"
        CompanionAction.PLAY -> "Play together"
        CompanionAction.LISTEN -> "Listen together"
        CompanionAction.SLEEP -> "Rest together"
        CompanionAction.WAKE -> "Wake me"
        CompanionAction.MEET -> "Meet up"
        // The wire value stays compatible while owner-facing copy remains neutral.
        CompanionAction.GIFT -> "Spend time together"
    }
}
