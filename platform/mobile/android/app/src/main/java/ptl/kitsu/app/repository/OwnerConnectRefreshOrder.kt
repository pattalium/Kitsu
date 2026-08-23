package ptl.kitsu.app.repository

/** Prevents replay=0 companion events from falling into the initial-refresh gap. */
internal object OwnerConnectRefreshOrder {
    suspend fun run(
        subscribe: suspend () -> Unit,
        initialRefresh: suspend () -> Unit,
    ) {
        subscribe()
        initialRefresh()
    }
}
