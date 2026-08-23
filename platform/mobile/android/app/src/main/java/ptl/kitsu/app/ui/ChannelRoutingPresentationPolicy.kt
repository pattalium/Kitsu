package ptl.kitsu.app.ui

import ptl.kitsu.app.model.ChannelRegionScope

internal data class ChannelRoutingPresentation(
    val label: String,
    val detail: String,
)

/** Describes the configured channel transport without claiming universal repeater support. */
internal object ChannelRoutingPresentationPolicy {
    fun present(regionScope: ChannelRegionScope?): ChannelRoutingPresentation = when (regionScope) {
        null -> ChannelRoutingPresentation(
            label = "Legacy",
            detail = "Legacy flood routing; repeater participation still depends on each repeater's " +
                "channel configuration.",
        )
        ChannelRegionScope.EU -> ChannelRoutingPresentation(
            label = "Scoped #EU",
            detail = "Scoped #EU routing; only repeaters configured to allow #EU can participate.",
        )
    }
}
