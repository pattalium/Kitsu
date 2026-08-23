package ptl.kitsu.app.ui

import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.Peer

internal data class AdvertEvidencePresentation(
    val statusLine: String,
    val badge: String,
    val detail: String,
    val tone: StatusTone,
)

/** Presents local rebroadcast observations without inventing relay identities or delivery proof. */
internal object AdvertEvidencePresentationPolicy {
    fun present(
        evidence: LastFloodAdvert?,
        peers: List<Peer> = emptyList(),
    ): AdvertEvidencePresentation = when {
        evidence == null -> AdvertEvidencePresentation(
            statusLine = "No Mesh-wide advertisement recorded",
            badge = "No record",
            detail = "No volatile Mesh-wide advertisement record is available from this Kitsu. $EVIDENCE_SCOPE",
            tone = StatusTone.NEUTRAL,
        )
        evidence.state == "queued" && evidence.repeatCount == null && !evidence.observationOpen ->
            AdvertEvidencePresentation(
                statusLine = "Queued",
                badge = "Queued",
                detail = "Kitsu queued this Mesh-wide advertisement for radio transmission. $EVIDENCE_SCOPE",
                tone = StatusTone.ACTIVE,
            )
        evidence.state == "sent" && evidence.repeatCount?.let { it in 0..255 } == true -> sent(evidence, peers)
        evidence.state == "tx_failed" && evidence.repeatCount == null && !evidence.observationOpen ->
            AdvertEvidencePresentation(
                statusLine = "Transmission failed",
                badge = "Failed",
                detail = "Kitsu did not complete this Mesh-wide radio transmission. $EVIDENCE_SCOPE",
                tone = StatusTone.NEGATIVE,
            )
        else -> AdvertEvidencePresentation(
            statusLine = "Advertisement evidence unavailable",
            badge = "Unavailable",
            detail = "Kitsu reported an advertisement record that this app cannot present safely. $EVIDENCE_SCOPE",
            tone = StatusTone.NEUTRAL,
        )
    }

    private fun sent(evidence: LastFloodAdvert, peers: List<Peer>): AdvertEvidencePresentation {
        val count = requireNotNull(evidence.repeatCount)
        val statusLine = when {
            count == 0 && evidence.observationOpen -> "Sent · listening for repeats"
            count == 0 -> "Sent · no matching repeat recorded"
            count == 1 -> "Sent · heard 1 repeat"
            count == 255 -> "Sent · heard 255+ repeats"
            else -> "Sent · heard $count repeats"
        }
        val observation = when {
            evidence.observationOpen && count == 0 ->
                "Kitsu sent this Mesh-wide advertisement and is listening during its bounded observation window."
            evidence.observationOpen ->
                if (count == 255) {
                    "Kitsu is still listening during its bounded observation window after hearing at least 255 matching rebroadcast packet copies."
                } else {
                    "Kitsu is still listening during its bounded observation window after hearing $count matching packet ${if (count == 1) "copy" else "copies"}."
                }
            count == 0 ->
                "Kitsu heard no matching returned copy before its bounded observation window closed. " +
                    "A repeater may still have forwarded the advertisement without that copy returning " +
                    "to this Kitsu. Recipient reception remains unconfirmed."
            count == 255 ->
                "Kitsu's bounded observation window closed after hearing at least 255 matching rebroadcast packet copies."
            else ->
                "Kitsu's bounded observation window closed after hearing $count matching packet ${if (count == 1) "copy" else "copies"}."
        }
        val sourceCaveat = if (evidence.repeatSources != null && count > 0) {
            "Path-token matches are unauthenticated and only indicate a likely current full-key peer."
        } else {
            null
        }
        return AdvertEvidencePresentation(
            statusLine = statusLine,
            badge = if (evidence.observationOpen) "Listening" else "Closed",
            detail = buildString {
                append(observation)
                if (sourceCaveat != null) append(" $sourceCaveat")
                append(" $EVIDENCE_SCOPE")
            },
            tone = if (evidence.observationOpen) StatusTone.ACTIVE else StatusTone.POSITIVE,
        )
    }

    private const val EVIDENCE_SCOPE =
        "Repeat evidence counts matching rebroadcast packet copies heard locally. " +
            "It does not identify unique repeaters or confirm recipient delivery."
}
