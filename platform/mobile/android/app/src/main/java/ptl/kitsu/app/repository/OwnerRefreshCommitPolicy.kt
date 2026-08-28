package ptl.kitsu.app.repository

import ptl.kitsu.app.connection.ConnectionState
import ptl.kitsu.app.model.HistoryEntry
import ptl.kitsu.app.model.EncounterCatalogCreature
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.MeshChannel
import ptl.kitsu.app.model.NearbyKitsu
import ptl.kitsu.app.model.NeighborInteractionKind
import ptl.kitsu.app.model.Peer

internal data class OwnerNonMessageSnapshot(
    val connection: ConnectionState,
    val status: KitsuStatus,
    val history: List<HistoryEntry>,
    val peers: List<Peer>,
    val channels: List<MeshChannel>,
    val encounterCatalog: List<EncounterCatalogCreature> = emptyList(),
    val encounterCatalogSupported: Boolean = false,
    val encounterCatalogErrorCode: String? = null,
    val nearbyKitsu: List<NearbyKitsu> = emptyList(),
    val nearbyKitsuSupported: Boolean = false,
    val nearbyInteractionKinds: Set<NeighborInteractionKind> = emptySet(),
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
        encounterCatalog = snapshot.encounterCatalog,
        encounterCatalogSupported = snapshot.encounterCatalogSupported,
        encounterCatalogErrorCode = snapshot.encounterCatalogErrorCode,
        nearbyKitsu = snapshot.nearbyKitsu,
        nearbyKitsuSupported = snapshot.nearbyKitsuSupported,
        nearbyInteractionKinds = snapshot.nearbyInteractionKinds,
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
