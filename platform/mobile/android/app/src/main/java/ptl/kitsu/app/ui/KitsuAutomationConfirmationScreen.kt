package ptl.kitsu.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.automation.KitsuAutomationAction

/** External shortcuts can select this page, but a local controller must confirm the action. */
@Composable
internal fun KitsuAutomationConfirmationScreen(
    action: KitsuAutomationAction,
    targetName: String?,
    targetDeviceId: String?,
    targetAddress: String?,
    enabled: Boolean,
    unavailableReason: String? = null,
    onConfirm: () -> Unit,
    onCancel: () -> Unit,
    modifier: Modifier = Modifier,
) {
    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("screen-automation-confirmation"),
        contentPadding = PaddingValues(18.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SectionHeading(
                title = action.label,
                supporting = "A shortcut opened this page. Nothing has been sent to Kitsu yet.",
            )
        }
        item {
            KitsuCard(title = "Confirm on this phone") {
                Text(
                    listOfNotNull(targetName, targetDeviceId)
                        .takeIf { it.isNotEmpty() }
                        ?.joinToString(" · ")
                        ?: "No Kitsu is currently selected",
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.testTag("automation-target"),
                )
                targetAddress?.let { address ->
                    Text(
                        "Bluetooth $address",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text(
                    confirmationDetail(action),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                unavailableReason?.let { reason ->
                    Text(
                        reason,
                        color = MaterialTheme.colorScheme.error,
                        modifier = Modifier.testTag("automation-unavailable-reason"),
                    )
                }
                Column(
                    modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Button(
                        onClick = onConfirm,
                        enabled = enabled,
                        modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                    ) { Text(confirmLabel(action)) }
                    OutlinedButton(
                        onClick = onCancel,
                        modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                    ) { Text("Cancel") }
                }
            }
        }
    }
}

private fun confirmationDetail(action: KitsuAutomationAction): String = when (action) {
    KitsuAutomationAction.CHECK -> "Refresh the selected Kitsu's current state."
    KitsuAutomationAction.PET -> "Send one Pet action to the selected Kitsu."
    KitsuAutomationAction.FEED -> "Send one Feed action to the selected Kitsu."
    KitsuAutomationAction.PLAY -> "Send one Play action to the selected Kitsu."
    KitsuAutomationAction.FOCUS_25 -> "Start a 25-minute focus session with Kitsu."
    KitsuAutomationAction.FOCUS_50 -> "Start a 50-minute focus session with Kitsu."
    KitsuAutomationAction.WALK -> "Open Walk with Kitsu so you can choose the route."
    KitsuAutomationAction.ACCESSIBILITY -> "Open Kitsu's large accessible companion view."
    KitsuAutomationAction.STUDIO -> "Open Pet Studio using Kitsu's authenticated live frame."
}

private fun confirmLabel(action: KitsuAutomationAction): String = when (action) {
    KitsuAutomationAction.WALK,
    KitsuAutomationAction.ACCESSIBILITY,
    KitsuAutomationAction.STUDIO,
    KitsuAutomationAction.CHECK,
    -> "Continue"
    else -> "Confirm ${action.label}"
}
