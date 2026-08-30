package ptl.kitsu.app.navigation

import java.util.Locale
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import ptl.kitsu.app.automation.KitsuAutomationAction
import ptl.kitsu.app.model.MAX_MESSAGE_BYTES
import ptl.kitsu.app.model.MeshPeerKeyPolicy
import ptl.kitsu.app.security.ControllerRole

/** A user-visible destination. External input may select a destination, but never perform an action. */
sealed interface AppRoute {
    data object Home : AppRoute

    data class Companion(
        val destination: CompanionDestination = CompanionDestination.OVERVIEW,
    ) : AppRoute

    data class Messages(
        /** Null means that the owner must choose a conversation before the draft is stored. */
        val threadKey: String? = null,
    ) : AppRoute

    /** Always opens a visible confirmation surface; launch routing never performs the action. */
    data class Automation(val action: KitsuAutomationAction) : AppRoute
}

enum class CompanionDestination {
    OVERVIEW,
    GUIDE,
    ACCESSIBILITY,
    STUDIO,
}

object CompanionDestinationPolicy {
    fun resolve(role: ControllerRole, destination: CompanionDestination): CompanionDestination =
        if (role == ControllerRole.CARETAKER && destination == CompanionDestination.GUIDE) {
            CompanionDestination.OVERVIEW
        } else {
            destination
        }

    fun allows(role: ControllerRole, destination: CompanionDestination): Boolean =
        resolve(role, destination) == destination
}

data class AppLaunchSpec(
    val route: AppRoute,
    /** A draft to show to the owner. Launch routing never sends it. */
    val messageDraft: String? = null,
)

data class AppLaunchRequest(
    val id: Long,
    val route: AppRoute,
    val messageDraft: String? = null,
)

/**
 * A one-slot, consume-once launch mailbox.
 *
 * A stale consumer cannot clear a newer request that arrived through MainActivity.onNewIntent().
 */
class AppLaunchRequestCoordinator {
    private val mutablePending = MutableStateFlow<AppLaunchRequest?>(null)
    val pending: StateFlow<AppLaunchRequest?> = mutablePending.asStateFlow()
    private var nextId = 1L

    @Synchronized
    fun submit(spec: AppLaunchSpec): AppLaunchRequest {
        val request = AppLaunchRequest(
            id = nextId,
            route = spec.route,
            messageDraft = spec.messageDraft,
        )
        nextId = if (nextId == Long.MAX_VALUE) 1L else nextId + 1L
        mutablePending.value = request
        return request
    }

    @Synchronized
    fun consume(id: Long): Boolean {
        if (mutablePending.value?.id != id) return false
        mutablePending.value = null
        return true
    }
}

data class SharedTextDraft(
    val text: String,
    val shortened: Boolean,
)

/** Accepts shared text as an editable local draft and bounds it to the firmware message contract. */
object IncomingTextSharePolicy {
    fun prepare(raw: CharSequence?): SharedTextDraft? {
        val sanitized = raw?.toString()?.filterShareControls() ?: return null
        if (sanitized.isBlank()) return null
        val shortened = sanitized.toByteArray(Charsets.UTF_8).size > MAX_MESSAGE_BYTES
        return SharedTextDraft(
            text = sanitized.utf8Prefix(MAX_MESSAGE_BYTES),
            shortened = shortened,
        )
    }

    private fun String.filterShareControls(): String = buildString(length) {
        this@filterShareControls.codePoints().forEach { codePoint ->
            if (!Character.isISOControl(codePoint) || codePoint == '\n'.code || codePoint == '\t'.code) {
                appendCodePoint(codePoint)
            }
        }
    }

    private fun String.utf8Prefix(maxBytes: Int): String = buildString(length) {
        var used = 0
        val codePoints = this@utf8Prefix.codePoints().iterator()
        while (codePoints.hasNext()) {
            val codePoint = codePoints.nextInt()
            val encoded = String(Character.toChars(codePoint)).toByteArray(Charsets.UTF_8)
            if (used + encoded.size > maxBytes) break
            appendCodePoint(codePoint)
            used += encoded.size
        }
    }
}

sealed interface AppLaunchIntentResult {
    data object Ignored : AppLaunchIntentResult
    data class Rejected(val reason: String) : AppLaunchIntentResult
    data class Accepted(
        val spec: AppLaunchSpec,
        val sharedTextShortened: Boolean = false,
    ) : AppLaunchIntentResult
}

/** Pure intent-field parser so exported Android entry points remain small and testable. */
object AppLaunchIntentPolicy {
    const val ACTION_SEND = "android.intent.action.SEND"
    const val ACTION_OPEN_HOME = "ptl.kitsu.app.action.OPEN_HOME"
    const val ACTION_OPEN_MESSAGES = "ptl.kitsu.app.action.OPEN_MESSAGES"
    const val ACTION_OPEN_COMPANION = "ptl.kitsu.app.action.OPEN_COMPANION"
    const val ACTION_CONFIRM_AUTOMATION = "ptl.kitsu.app.action.CONFIRM_AUTOMATION"
    const val EXTRA_THREAD_KEY = "ptl.kitsu.app.extra.THREAD_KEY"
    const val EXTRA_COMPANION_DESTINATION = "ptl.kitsu.app.extra.COMPANION_DESTINATION"
    const val EXTRA_AUTOMATION_ACTION = "ptl.kitsu.app.extra.AUTOMATION_ACTION"

    fun parse(
        action: String?,
        mimeType: String?,
        sharedText: CharSequence?,
        routeThreadKey: String? = null,
        companionDestination: String? = null,
        automationAction: String? = null,
    ): AppLaunchIntentResult {
        if (action == ACTION_OPEN_HOME) {
            return AppLaunchIntentResult.Accepted(AppLaunchSpec(route = AppRoute.Home))
        }
        if (action == ACTION_OPEN_MESSAGES) {
            val threadKey = routeThreadKey?.takeIf(::validThreadKey)
                ?: return AppLaunchIntentResult.Rejected("That conversation is not available.")
            return AppLaunchIntentResult.Accepted(
                AppLaunchSpec(route = AppRoute.Messages(threadKey = threadKey)),
            )
        }
        if (action == ACTION_OPEN_COMPANION) {
            val destination = companionDestination
                ?.let { value ->
                    CompanionDestination.entries.firstOrNull {
                        it.name.equals(value, ignoreCase = true)
                    }
                }
                ?: CompanionDestination.OVERVIEW
            return AppLaunchIntentResult.Accepted(
                AppLaunchSpec(route = AppRoute.Companion(destination)),
            )
        }
        if (action == ACTION_CONFIRM_AUTOMATION) {
            val parsed = KitsuAutomationAction.fromWireName(automationAction)
                ?: return AppLaunchIntentResult.Rejected("That Kitsu shortcut is not available.")
            return AppLaunchIntentResult.Accepted(
                AppLaunchSpec(route = AppRoute.Automation(parsed)),
            )
        }
        if (action != ACTION_SEND) return AppLaunchIntentResult.Ignored
        val normalizedType = mimeType?.substringBefore(';')?.trim()?.lowercase(Locale.ROOT)
        if (normalizedType != "text/plain") {
            return AppLaunchIntentResult.Rejected("Only plain text can be shared to Kitsu.")
        }
        val draft = IncomingTextSharePolicy.prepare(sharedText)
            ?: return AppLaunchIntentResult.Rejected("There is no message text to share.")
        return AppLaunchIntentResult.Accepted(
            spec = AppLaunchSpec(
                route = AppRoute.Messages(),
                messageDraft = draft.text,
            ),
            sharedTextShortened = draft.shortened,
        )
    }

    private fun validThreadKey(value: String): Boolean = when {
        value.startsWith("direct:") ->
            MeshPeerKeyPolicy.isCanonicalBase64Url(value.removePrefix("direct:"))
        value.startsWith("channel:") -> value.removePrefix("channel:").toIntOrNull() in 0..3
        else -> false
    }
}
