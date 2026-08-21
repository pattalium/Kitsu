package app.kitsu.mobile.transport

/**
 * Keeps the security-sensitive direct provisioning order explicit and
 * independently testable. A rejected clock synchronization must prevent the
 * following secret-bearing write from ever being attempted.
 */
internal object DirectProvisioningPlan {
    private val allowedWrites = setOf(
        "wifi.configure",
        "gateway.configure",
        "gateway.enroll.begin",
    )

    suspend fun <T> execute(
        writeOperation: String,
        synchronizeClock: suspend () -> Unit,
        write: suspend (String) -> T,
    ): T {
        require(writeOperation in allowedWrites) { "invalid_provisioning_operation" }
        synchronizeClock()
        return write(writeOperation)
    }
}
