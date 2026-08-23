package ptl.kitsu.app.ui

import ptl.kitsu.app.model.LastNearbyAdvert

/** Presents the physical zero-hop submission lifecycle without implying repeat evidence. */
internal object NearbyAdvertPresentationPolicy {
    fun present(evidence: LastNearbyAdvert?): AdvertEvidencePresentation = when (evidence?.state) {
        null -> AdvertEvidencePresentation(
            statusLine = "No Nearby advertisement recorded",
            badge = "No record",
            detail = "No volatile Nearby advertisement record is available from this Kitsu. $SCOPE",
            tone = StatusTone.NEUTRAL,
        )
        "queued" -> AdvertEvidencePresentation(
            statusLine = "Queued",
            badge = "Queued",
            detail = "Kitsu queued this Nearby advertisement for one zero-hop radio transmission. $SCOPE",
            tone = StatusTone.ACTIVE,
        )
        "sent" -> AdvertEvidencePresentation(
            statusLine = "Sent nearby",
            badge = "Sent",
            detail = "Kitsu completed this one zero-hop Nearby radio transmission. $SCOPE",
            tone = StatusTone.POSITIVE,
        )
        "tx_failed" -> AdvertEvidencePresentation(
            statusLine = "Transmission failed",
            badge = "Failed",
            detail = "Kitsu did not complete this Nearby radio transmission. $SCOPE",
            tone = StatusTone.NEGATIVE,
        )
        else -> AdvertEvidencePresentation(
            statusLine = "Advertisement status unavailable",
            badge = "Unavailable",
            detail = "Kitsu reported a Nearby record that this app cannot present safely. $SCOPE",
            tone = StatusTone.NEUTRAL,
        )
    }

    private const val SCOPE =
        "Nearby is zero-hop and is not repeated, so repeat evidence does not apply. " +
            "This status does not confirm that another Kitsu received it."
}
