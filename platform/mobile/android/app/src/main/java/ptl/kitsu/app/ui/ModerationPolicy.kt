package ptl.kitsu.app.ui

import android.content.Context
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

object MeshUserPolicy {
    const val VERSION = 1
    const val VERSION_LABEL = "2026-08-22.1"

    const val TERMS =
        "Mesh messaging is for lawful, respectful communication. Do not send threats, harassment, hate, spam, scams, illegal content, sexual exploitation, or content that exposes another person's private information. You are responsible for messages you deliberately transmit. Kitsu may update this policy and require acceptance again before sending."

    const val PROHIBITED_CONTENT =
        "Do not send threats, harassment, hate, spam, scams, illegal content, sexual exploitation, or another person's private information."

    const val PRIVACY =
        "Kitsu Companion has no internet permission. Device authorization, cached mesh activity, policy acceptance, and blocked peer identifiers stay on this phone. Bluetooth messages go directly through your selected Kitsu. A moderation report is created only when you choose Report and is written only to the document location you select; the app does not submit it automatically."
}

class ModerationPreferences(context: Context) {
    private val preferences = context.applicationContext.getSharedPreferences(
        PREFERENCES_NAME,
        Context.MODE_PRIVATE,
    )

    fun acceptedPolicyVersion(): Int = preferences.getInt(KEY_ACCEPTED_POLICY_VERSION, 0)

    fun acceptCurrentPolicy() {
        preferences.edit().putInt(KEY_ACCEPTED_POLICY_VERSION, MeshUserPolicy.VERSION).apply()
    }

    fun blockedPeerIds(): Set<String> =
        preferences.getStringSet(KEY_BLOCKED_PEERS, emptySet()).orEmpty().toSet()

    fun blockPeer(peerId: String) {
        val canonical = peerId.trim()
        require(canonical.isNotEmpty()) { "peer_id_required" }
        preferences.edit().putStringSet(KEY_BLOCKED_PEERS, blockedPeerIds() + canonical).apply()
    }

    fun unblockPeer(peerId: String) {
        preferences.edit().putStringSet(KEY_BLOCKED_PEERS, blockedPeerIds() - peerId).apply()
    }

    private companion object {
        const val PREFERENCES_NAME = "kitsu_mesh_moderation"
        const val KEY_ACCEPTED_POLICY_VERSION = "accepted_policy_version"
        const val KEY_BLOCKED_PEERS = "blocked_peer_ids"
    }
}

@Serializable
enum class ReportReason {
    @SerialName("spam_or_scam") SPAM_OR_SCAM,
    @SerialName("harassment_or_hate") HARASSMENT_OR_HATE,
    @SerialName("illegal_or_exploitative") ILLEGAL_OR_EXPLOITATIVE,
    @SerialName("privacy_violation") PRIVACY_VIOLATION,
    @SerialName("other") OTHER,
}

@Serializable
enum class ReportType {
    @SerialName("message") MESSAGE,
    @SerialName("sender") SENDER,
}

@Serializable
data class ModerationReport(
    val schema: String = SCHEMA,
    @SerialName("policy_version") val policyVersion: Int = MeshUserPolicy.VERSION,
    @SerialName("policy_version_label") val policyVersionLabel: String = MeshUserPolicy.VERSION_LABEL,
    @SerialName("app_id") val appId: String,
    @SerialName("app_version") val appVersion: String,
    @SerialName("created_at_epoch") val createdAtEpoch: Long,
    @SerialName("device_id") val deviceId: String?,
    @SerialName("report_type") val reportType: ReportType,
    val reason: ReportReason,
    val note: String? = null,
    @SerialName("message_id") val messageId: String,
    val cursor: String,
    val direction: String,
    @SerialName("peer_id") val peerId: String?,
    val channel: String?,
    val text: String,
    val state: String,
    @SerialName("occurred_at") val occurredAt: Long,
) {
    init {
        require(schema == SCHEMA)
        require(policyVersion == MeshUserPolicy.VERSION)
        require(messageId.isNotBlank())
        require(reportType != ReportType.SENDER || !peerId.isNullOrBlank())
        require(note == null || note.toByteArray(Charsets.UTF_8).size <= MAX_NOTE_BYTES)
    }

    fun suggestedFileName(): String =
        "kitsu-mesh-report-${messageId.filter { it.isLetterOrDigit() }.take(24).ifBlank { "message" }}.json"

    companion object {
        const val SCHEMA = "kitsu.mesh-moderation-report.v1"
        const val MAX_NOTE_BYTES = 512
    }
}

object ModerationReportCodec {
    private val json = Json {
        encodeDefaults = true
        explicitNulls = true
        prettyPrint = true
    }

    fun encode(report: ModerationReport): String = json.encodeToString(report) + "\n"
    fun decode(value: String): ModerationReport = json.decodeFromString(value)
}
