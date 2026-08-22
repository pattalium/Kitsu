package app.kitsu.mobile

import android.content.pm.ActivityInfo
import android.content.res.Configuration
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.assertHasClickAction
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextInput
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PortraitOwnerUiTest {
    @get:Rule
    val compose = createAndroidComposeRule<MainActivity>()

    @Test
    fun portraitShellKeepsPrimaryNavigationAndConnectionControlsReachable() {
        compose.waitForIdle()
        assertEquals(
            Configuration.ORIENTATION_PORTRAIT,
            compose.activity.resources.configuration.orientation,
        )
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT, compose.activity.requestedOrientation)
        val connectControls = compose.onAllNodesWithTag("connection-connect").fetchSemanticsNodes()
        val disconnectControls = compose.onAllNodesWithTag("connection-disconnect").fetchSemanticsNodes()
        assertTrue(connectControls.isNotEmpty() || disconnectControls.isNotEmpty())

        compose.onNodeWithTag("nav-messages").performClick()
        compose.onNodeWithText("Messages", substring = true).assertExists()
        compose.onNodeWithTag("nav-settings").performClick()
        compose.onNodeWithText("Kitsu companion").assertExists()

        val screenshot = InstrumentationRegistry.getInstrumentation().uiAutomation.takeScreenshot()
        assertTrue("portrait screenshot was ${screenshot.width}x${screenshot.height}", screenshot.height > screenshot.width)
    }

    @Test
    fun messageComposerKeepsBodyAndSendControlVisibleWithTheImeOpen() {
        compose.onNodeWithTag("nav-messages").performClick()
        compose.onNodeWithTag("message-target")
            .performScrollTo()
            .performClick()
            .performTextInput("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")
        compose.onNodeWithTag("message-body")
            .performScrollTo()
            .performClick()
            .performTextInput("Portrait IME acceptance")
        compose.waitForIdle()

        compose.onNodeWithTag("message-composer").assertIsDisplayed()
        compose.onNodeWithTag("message-body").assertIsDisplayed()
        compose.onNodeWithTag("message-send").assertIsDisplayed().assertHasClickAction()
        compose.waitUntil(timeoutMillis = 5_000) {
            compose.onAllNodesWithTag("message-compact-target")
                .fetchSemanticsNodes().isNotEmpty()
        }
        compose.onNodeWithTag("message-compact-target").assertIsDisplayed()
    }

    @Test
    fun primaryControlsExposeReachableAccessibilitySemantics() {
        compose.onNodeWithTag("nav-home").assertHasClickAction()
        compose.onNodeWithTag("nav-messages").assertHasClickAction()
        compose.onNodeWithTag("nav-network").assertHasClickAction()
        compose.onNodeWithTag("nav-care").assertHasClickAction()
        compose.onNodeWithTag("nav-settings").assertHasClickAction()
        val connect = compose.onAllNodesWithTag("connection-connect").fetchSemanticsNodes()
        val disconnect = compose.onAllNodesWithTag("connection-disconnect").fetchSemanticsNodes()
        assertTrue(connect.isNotEmpty() || disconnect.isNotEmpty())
    }

    @Test
    fun emulatorProfileMatchesTheDeclaredPortraitAndFontScaleContract() {
        val arguments = InstrumentationRegistry.getArguments()
        val profile = arguments.getString("kitsu_profile") ?: "unspecified"
        val configuration = compose.activity.resources.configuration
        when (profile) {
            "small" -> {
                assertTrue("small width was ${configuration.screenWidthDp}dp", configuration.screenWidthDp <= 360)
                assertTrue("small height was ${configuration.screenHeightDp}dp", configuration.screenHeightDp <= 720)
                assertTrue("small font scale was ${configuration.fontScale}", configuration.fontScale >= 1.25f)
            }
            "large" -> {
                assertTrue("large width was ${configuration.screenWidthDp}dp", configuration.screenWidthDp >= 390)
                assertTrue("large height was ${configuration.screenHeightDp}dp", configuration.screenHeightDp >= 800)
                assertTrue("large font scale was ${configuration.fontScale}", configuration.fontScale >= 1.10f)
            }
            else -> assertTrue(configuration.screenHeightDp > configuration.screenWidthDp)
        }
    }
}
