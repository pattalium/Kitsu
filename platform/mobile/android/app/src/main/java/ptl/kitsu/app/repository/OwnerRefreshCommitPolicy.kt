package ptl.kitsu.app.repository

import ptl.kitsu.app.connection.ConnectionState
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.NearbyKitsu
import ptl.kitsu.app.model.Peer

internal data class OwnerNonMessageSnapshot(
    val connection: ConnectionState,
    val status: KitsuStatus,
    val history: List<HistoryEntry>,
    val peers: List<Peer>,
    val channels: List<MeshChannel>,
    val nearbyKitsu: List<NearbyKitsu> = emptyList(),
    val nearbyKitsuSupported: Boolean = false,
    val nearbyKitsuErrorCode: String? = null,
    val messageMarkReadSupported: Boolean = false,
)

/** Applies slow endpoint results without overwriting a newer messages-only commit. */
internal object OwnerRefreshCommitPolicy {
    fun apply(current: OwnerState, snapshot: OwnerNonMessageSnapshot): OwnerState = current.copy(
        connection = snapshot.connection,
        status = snapshot.status,
        history = snapshot.history,
        peers = snapshot.peers,
        channels = snapshot.channels,
        nearbyKitsu = snapshot.nearbyKitsu,
        nearbyKitsuSupported = snapshot.nearbyKitsuSupported,
        nearbyKitsuErrorCode = snapshot.nearbyKitsuErrorCode,
        messageMarkReadSupported = snapshot.messageMarkReadSupported,
        loading = false,
        errorCode = null,
    )

    fun applyFailure(
        current: OwnerState,
        connection: ConnectionState,
        errorCode: String,
    ): OwnerState = current.copy(
        connection = connection,
        loading = false,
        errorCode = errorCode,
    )
}
