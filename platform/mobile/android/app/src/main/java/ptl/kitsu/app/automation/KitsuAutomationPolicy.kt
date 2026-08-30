package ptl.kitsu.app.automation

import android.content.Intent

enum class KitsuAutomationAction(val wireName: String, val label: String) {
    CHECK("check", "Check on Kitsu"),
    PET("pet", "Pet Kitsu"),
    FEED("feed", "Feed Kitsu"),
    PLAY("play", "Play with Kitsu"),
    FOCUS_25("focus_25", "Start a 25-minute focus session"),
    FOCUS_50("focus_50", "Start a 50-minute focus session"),
    WALK("walk", "Open Walk with Kitsu"),
    ACCESSIBILITY("accessibility", "Open accessible companion"),
    STUDIO("studio", "Open Pet Studio"),
    ;

    companion object {
        fun fromWireName(value: String?): KitsuAutomationAction? =
            entries.firstOrNull { it.wireName == value }
    }
}

data class KitsuAutomationRequest(
    val action: KitsuAutomationAction,
    val capabilityToken: String,
)

object KitsuAutomationPolicy {
    const val ACTION_AUTOMATE = "ptl.kitsu.app.action.AUTOMATE"
    const val ACTION_CONFIRM = "ptl.kitsu.app.action.CONFIRM_AUTOMATION"
    const val EXTRA_ACTION = "ptl.kitsu.app.extra.AUTOMATION_ACTION"
    const val EXTRA_CAPABILITY = "ptl.kitsu.app.extra.AUTOMATION_CAPABILITY"
    private val tokenPattern = Regex("^[A-Za-z0-9_-]{43}$")

    fun parse(intent: Intent?): KitsuAutomationRequest? {
        if (intent == null) return null
        val extras = intent.extras ?: return null
        return parse(
            action = intent.action,
            hasData = intent.data != null,
            hasClipData = intent.clipData != null,
            mimeType = intent.type,
            categories = intent.categories.orEmpty(),
            extras = extras.keySet().associateWith(extras::getString),
        )
    }

    fun parse(
        action: String?,
        hasData: Boolean,
        hasClipData: Boolean,
        mimeType: String?,
        categories: Set<String>,
        extras: Map<String, Any?>,
    ): KitsuAutomationRequest? {
        if (action != ACTION_AUTOMATE || hasData || hasClipData || mimeType != null ||
            categories.isNotEmpty() || extras.keys != setOf(EXTRA_ACTION, EXTRA_CAPABILITY)
        ) return null
        val parsedAction = KitsuAutomationAction.fromWireName(
            extras[EXTRA_ACTION] as? String,
        ) ?: return null
        val token = extras[EXTRA_CAPABILITY] as? String ?: return null
        if (!tokenPattern.matches(token)) return null
        return KitsuAutomationRequest(parsedAction, token)
    }

    fun confirmationIntent(applicationId: String, action: KitsuAutomationAction): Intent =
        Intent(ACTION_CONFIRM).apply {
            require(applicationId.isNotBlank()) { "automation_application_id_required" }
            setClassName(applicationId, "ptl.kitsu.app.MainActivity")
            putExtra(EXTRA_ACTION, action.wireName)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        }

    fun setupRecipe(applicationId: String, capabilityToken: String): String {
        require(applicationId.isNotBlank()) { "automation_application_id_required" }
        require(tokenPattern.matches(capabilityToken)) { "automation_capability_invalid" }
        return buildString {
            appendLine(
                "Component: $applicationId/ptl.kitsu.app.automation.KitsuAutomationActivity",
            )
            appendLine("Action: $ACTION_AUTOMATE")
            appendLine("String extra $EXTRA_ACTION: <action>")
            appendLine("String extra $EXTRA_CAPABILITY: $capabilityToken")
            append("Allowed <action>: ")
            append(KitsuAutomationAction.entries.joinToString { it.wireName })
        }
    }
}
