package ptl.kitsu.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BluetoothDisabled
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.HourglassTop
import androidx.compose.material.icons.filled.Pets
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.R
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.transport.ConnectionMode

@Composable
internal fun KitsuCard(
    modifier: Modifier = Modifier,
    title: String? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            title?.let {
                Text(it, style = MaterialTheme.typography.titleLarge)
            }
            content()
        }
    }
}

@Composable
internal fun SectionHeading(
    title: String,
    supporting: String? = null,
    modifier: Modifier = Modifier,
) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(title, style = MaterialTheme.typography.titleLarge)
        supporting?.let {
            Text(
                it,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
internal fun StatusPill(
    label: String,
    tone: StatusTone,
    modifier: Modifier = Modifier,
) {
    val colors = when (tone) {
        StatusTone.POSITIVE -> MaterialTheme.colorScheme.tertiaryContainer to MaterialTheme.colorScheme.onTertiaryContainer
        StatusTone.ACTIVE -> MaterialTheme.colorScheme.primaryContainer to MaterialTheme.colorScheme.onPrimaryContainer
        StatusTone.NEGATIVE -> MaterialTheme.colorScheme.errorContainer to MaterialTheme.colorScheme.onErrorContainer
        StatusTone.NEUTRAL -> MaterialTheme.colorScheme.surfaceVariant to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Surface(
        modifier = modifier,
        color = colors.first,
        contentColor = colors.second,
        shape = CircleShape,
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 11.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.spacedBy(7.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(
                Modifier.size(7.dp).clip(CircleShape).background(colors.second),
            )
            Text(label, style = MaterialTheme.typography.labelLarge)
        }
    }
}

internal enum class StatusTone { POSITIVE, ACTIVE, NEGATIVE, NEUTRAL }

internal data class ConnectionPresentation(
    val label: String,
    val detail: String,
    val tone: StatusTone,
    val icon: ImageVector,
)

private val TRANSIENT_BLE_DISCONNECT_CODES = setOf(
    "gatt_peer_terminated",
    "gatt_peer_terminated_before_auth",
    // Keep an old raw status value neutral if it survives in process state
    // across an app upgrade.
    "gatt_status_19",
)

internal fun connectionPresentation(owner: OwnerState): ConnectionPresentation = when {
    owner.connection.connected && owner.connection.warning != null -> ConnectionPresentation(
        label = "Connected",
        detail = "${owner.connection.warning.humanized()}. Bluetooth stays connected; synchronize time and retry.",
        tone = StatusTone.ACTIVE,
        icon = Icons.Default.ErrorOutline,
    )
    owner.connection.connected -> ConnectionPresentation(
        label = "Connected",
        detail = "Private, authenticated Bluetooth link",
        tone = StatusTone.POSITIVE,
        icon = Icons.Default.CheckCircle,
    )
    owner.loading || owner.connection.mode == ConnectionMode.CONNECTING -> ConnectionPresentation(
        label = "Connecting",
        detail = "Looking for your selected Kitsu nearby",
        tone = StatusTone.ACTIVE,
        icon = Icons.Default.HourglassTop,
    )
    owner.errorCode != null && owner.errorCode !in TRANSIENT_BLE_DISCONNECT_CODES -> ConnectionPresentation(
        label = "Needs attention",
        detail = owner.errorCode.humanized(),
        tone = StatusTone.NEGATIVE,
        icon = Icons.Default.ErrorOutline,
    )
    owner.activeDeviceAddress == null -> ConnectionPresentation(
        label = "No Kitsu yet",
        detail = "Pair a nearby Kitsu to begin",
        tone = StatusTone.NEUTRAL,
        icon = Icons.Default.Pets,
    )
    else -> ConnectionPresentation(
        label = "Disconnected",
        detail = when {
            owner.connection.mode == ConnectionMode.PERMISSION_REQUIRED -> "Bluetooth permission is required"
            owner.connection.detail == "bluetooth_disabled" -> "Bluetooth is turned off"
            owner.connection.detail == "user_disconnected" -> "Disconnected by you"
            else -> owner.connection.detail.humanized()
        },
        tone = StatusTone.NEUTRAL,
        icon = Icons.Default.BluetoothDisabled,
    )
}

@Composable
internal fun StatePanel(
    title: String,
    message: String,
    modifier: Modifier = Modifier,
    kind: StatePanelKind = StatePanelKind.EMPTY,
    actionLabel: String? = null,
    onAction: (() -> Unit)? = null,
    testTag: String? = null,
) {
    val icon = when (kind) {
        StatePanelKind.LOADING -> Icons.Default.HourglassTop
        StatePanelKind.ERROR -> Icons.Default.ErrorOutline
        StatePanelKind.EMPTY -> Icons.Default.Pets
    }
    val tint = when (kind) {
        StatePanelKind.ERROR -> MaterialTheme.colorScheme.error
        StatePanelKind.LOADING -> MaterialTheme.colorScheme.primary
        StatePanelKind.EMPTY -> MaterialTheme.colorScheme.secondary
    }
    val tagged = if (testTag == null) modifier else modifier.testTag(testTag)
    Surface(
        modifier = tagged.fillMaxWidth(),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.62f),
        shape = MaterialTheme.shapes.large,
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (kind == StatePanelKind.LOADING) {
                CircularProgressIndicator(modifier = Modifier.size(28.dp), strokeWidth = 3.dp)
            } else if (kind == StatePanelKind.EMPTY) {
                Icon(
                    painter = painterResource(R.drawable.kitsu_app_icon_monochrome),
                    contentDescription = null,
                    tint = tint,
                    modifier = Modifier.size(54.dp),
                )
            } else {
                Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.size(32.dp))
            }
            Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Text(
                message,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (actionLabel != null && onAction != null) {
                Spacer(Modifier.height(2.dp))
                Button(onClick = onAction) { Text(actionLabel) }
            }
        }
    }
}

internal enum class StatePanelKind { LOADING, ERROR, EMPTY }

@Composable
internal fun NeedMeter(
    label: String,
    value: Int,
    color: Color,
    modifier: Modifier = Modifier,
) {
    val safeValue = value.coerceIn(0, 100)
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(label, style = MaterialTheme.typography.labelLarge)
            Text(
                "$safeValue%",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        LinearProgressIndicator(
            progress = { safeValue / 100f },
            modifier = Modifier.fillMaxWidth().height(7.dp).clip(CircleShape),
            color = color,
            trackColor = MaterialTheme.colorScheme.surfaceVariant,
        )
    }
}

internal fun String.humanized(): String = when (this) {
    "controller_already_saved_use_repair_or_forget" ->
        "This Kitsu already has a saved controller on this phone. Use Repair Bluetooth pairing, or explicitly forget the old controller before issuing a new one."
    "controller_full" ->
        "Kitsu already has four controllers. On Kitsu: CONNECT > CONTROLLERS; remove a slot; reopen Pair Phone; retry."
    "bluetooth_pairing_repair_required", "bond_missing_repair_required" ->
        "Bluetooth security pairing needs repair"
    "gatt_status_19", "gatt_peer_terminated" -> "Kitsu ended the Bluetooth connection"
    "gatt_peer_terminated_before_auth" ->
        "Kitsu ended the Bluetooth connection before authentication completed"
    "android_bluetooth_forget_required" ->
        "Forget the old Kitsu bond in Android Bluetooth settings to continue"
    "repair_bluetooth_permission_required" ->
        "Bluetooth permission is required to repair this pairing"
    "saved_controller_authorization_missing" ->
        "The saved controller authorization is no longer present on Kitsu"
    "checking_saved_controller" -> "Checking the saved controller authorization"
    "scanning_saved_kitsu_for_repair" -> "Looking for the saved Kitsu"
    "accept_android_pairing_code_then_confirm_on_kitsu" ->
        "Accept Android's pairing code, then confirm it on Kitsu"
    "bluetooth_bond_repaired_controller_kept" ->
        "New Bluetooth bond complete; saved controller kept"
    "one_fresh_gatt_retry" -> "Verifying the new bond once"
    "saved_controller_authenticated" -> "Saved controller authenticated"
    "idempotency_busy" ->
        "Too many recent actions are still protected. Wait a moment and retry."
    "idempotency_unavailable" -> "Durable action storage is unavailable."
    "clock_sync_failed" -> "Clock sync failed"
    "system_clock_failed" -> "Kitsu rejected clock synchronization"
    "firmware_reinstall_confirmation_stale" ->
        "Kitsu's firmware state changed. Review the selected Kitsu, then confirm Install again once more."
    else -> replace('_', ' ').replaceFirstChar { it.uppercase() }
}

internal data class LocationSettingsActionState(
    val visible: Boolean,
    val enabled: Boolean,
)

internal fun locationSettingsActionState(
    connectionDetail: String,
    errorCode: String?,
    updateBusy: Boolean,
): LocationSettingsActionState {
    val visible = connectionDetail in LOCATION_SETTINGS_ERRORS ||
        (errorCode != null && errorCode in LOCATION_SETTINGS_ERRORS)
    return LocationSettingsActionState(visible = visible, enabled = visible && !updateBusy)
}

private val LOCATION_SETTINGS_ERRORS = setOf(
    "location_services_disabled",
    "location_services_unavailable",
)

internal const val UPDATE_LOCKED_COPY =
    "Firmware update in progress. Other controls are locked until it finishes or is safely cancelled."

internal const val BLE_PERMISSION_COPY =
    "Android requests Nearby devices access, or location permission on Android 11 and older, only so Kitsu can scan for the selected Bluetooth device. The app does not collect phone location."
