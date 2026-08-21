package app.kitsu.mobile.ui

enum class OwnerAccountStatus {
    CHECKING,
    SIGNED_OUT,
    SIGNED_IN,
}

data class OwnerAccountPresentation(
    val statusLabel: String,
    val statusDetail: String,
    val signInEnabled: Boolean,
    val signOutEnabled: Boolean,
)

/** Human-facing owner-account language and state live here so the settings
 * page cannot drift back into protocol terminology. */
object OwnerAccountUiPolicy {
    const val PURPOSE =
        "An owner account lets this phone reach your companion through its Wi-Fi gateway when " +
            "you are away. It also loads the companions and gateways assigned to that account."

    const val BLUETOOTH_BOUNDARY =
        "Nearby Bluetooth works without an account. Pairing this phone and using a Kitsu beside " +
            "you do not require sign-in."

    const val INITIAL_ACCESS =
        "During service setup, the service operator creates a one-time owner username and " +
            "temporary password, then delivers that handoff to you privately. Use it here, then " +
            "choose your own password when prompted on the first sign-in."

    const val RECOVERY =
        "There is no public sign-up or email password reset. If the private handoff or password " +
            "is lost, follow the owner recovery guide on a trusted computer."

    fun presentation(status: OwnerAccountStatus): OwnerAccountPresentation = when (status) {
        OwnerAccountStatus.CHECKING -> OwnerAccountPresentation(
            statusLabel = "Checking account",
            statusDetail = "Looking for an owner session saved securely on this phone.",
            signInEnabled = false,
            signOutEnabled = false,
        )
        OwnerAccountStatus.SIGNED_OUT -> OwnerAccountPresentation(
            statusLabel = "Not signed in",
            statusDetail = "Bluetooth remains available. Sign in only to use remote access.",
            signInEnabled = true,
            signOutEnabled = false,
        )
        OwnerAccountStatus.SIGNED_IN -> OwnerAccountPresentation(
            statusLabel = "Signed in for remote access",
            statusDetail = "This phone can load companions assigned to your owner account.",
            signInEnabled = false,
            signOutEnabled = true,
        )
    }
}
