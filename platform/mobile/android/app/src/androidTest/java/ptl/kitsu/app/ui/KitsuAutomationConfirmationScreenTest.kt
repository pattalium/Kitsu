package ptl.kitsu.app.ui

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.assertTextEquals
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import ptl.kitsu.app.automation.KitsuAutomationAction

@RunWith(AndroidJUnit4::class)
class KitsuAutomationConfirmationScreenTest {
    @get:Rule
    val compose = createComposeRule()

    @Test
    fun unavailableStudioTargetExplainsWhyConfirmationIsDisabled() {
        val reason = "Update this Kitsu's firmware before opening Pet Studio."
        compose.setContent {
            KitsuTheme(KitsuThemePreference.DARK) {
                KitsuAutomationConfirmationScreen(
                    action = KitsuAutomationAction.STUDIO,
                    targetName = "Shade",
                    targetDeviceId = "kitsu-1234",
                    targetAddress = "AA:BB:CC:DD:EE:FF",
                    enabled = false,
                    unavailableReason = reason,
                    onConfirm = {},
                    onCancel = {},
                )
            }
        }

        compose.onNodeWithTag("automation-unavailable-reason")
            .assertIsDisplayed()
            .assertTextEquals(reason)
        compose.onNodeWithText("Continue").assertIsNotEnabled()
    }
}
