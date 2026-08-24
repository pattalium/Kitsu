package ptl.kitsu.app

import android.content.Context
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assert
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.assertHasClickAction
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertIsFocused
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollToNode
import androidx.compose.ui.test.performTextReplacement
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import ptl.kitsu.app.ui.MeshUserPolicy
import ptl.kitsu.app.ui.ModerationPreferences
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AdaptiveOwnerUiTest {
    @get:Rule
    val compose = createAndroidComposeRule<MainActivity>()

    private val targetContext: Context
        get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Before
    fun resetPersistentUiChoices() {
        targetContext.getSharedPreferences(APPEARANCE_PREFERENCES, Context.MODE_PRIVATE)
            .edit().clear().commit()
        targetContext.getSharedPreferences(MODERATION_PREFERENCES, Context.MODE_PRIVATE)
            .edit().clear().commit()
        compose.activityRule.scenario.recreate()
        compose.waitForIdle()
    }

    @After
    fun restoreOrientationAndPreferences() {
        compose.activityRule.scenario.onActivity {
            it.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
        targetContext.getSharedPreferences(APPEARANCE_PREFERENCES, Context.MODE_PRIVATE)
            .edit().clear().commit()
        targetContext.getSharedPreferences(MODERATION_PREFERENCES, Context.MODE_PRIVATE)
            .edit().clear().commit()
    }

    @Test
    fun darkFourTabShellUsesTheLayoutForTheAvailableWidthAndRespectsInsets() {
        assertEquals(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED, compose.activity.requestedOrientation)
        compose.onNodeWithTag("kitsu-app")
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.StateDescription, "Dark theme"))

        listOf("nav-home", "nav-network", "nav-messages", "nav-settings").forEach { tag ->
            compose.onNodeWithTag(tag).assertHasClickAction()
        }
        compose.onAllNodesWithTag("nav-care").assertCountEquals(0)

        assertAdaptiveLayoutMatchesWidth()

        val rootBounds = compose.onNodeWithTag("kitsu-app").fetchSemanticsNode().boundsInRoot
        val topBarBounds = compose.onNodeWithTag("top-app-bar").fetchSemanticsNode().boundsInRoot
        val homeBounds = compose.onNodeWithTag("screen-home").fetchSemanticsNode().boundsInRoot
        assertTrue("top app bar started outside the app root", topBarBounds.top >= rootBounds.top)
        assertTrue("home content overlapped the top app bar", homeBounds.top >= topBarBounds.bottom - 1f)
        assertTrue("home content extended outside the app root", homeBounds.bottom <= rootBounds.bottom + 1f)

        if (tagCount("bottom-navigation") == 1) {
            val navigationBounds = compose.onNodeWithTag("bottom-navigation")
                .fetchSemanticsNode().boundsInRoot
            assertTrue("bottom navigation extended outside the app root", navigationBounds.bottom <= rootBounds.bottom + 1f)
            assertTrue("home content overlapped bottom navigation", homeBounds.bottom <= navigationBounds.top + 1f)
        } else {
            val railBounds = compose.onNodeWithTag("navigation-rail").fetchSemanticsNode().boundsInRoot
            assertTrue("navigation rail extended outside the app root", railBounds.left >= rootBounds.left)
            assertTrue("home content overlapped navigation rail", homeBounds.left >= railBounds.right - 1f)
        }

        val connectionActions = tagCount("connection-connect") + tagCount("connection-disconnect")
        val setupActions = compose.onAllNodesWithText("Set up a Kitsu").fetchSemanticsNodes().size
        assertTrue("home exposed no real connection or setup action", connectionActions + setupActions > 0)
    }

    @Test
    fun landscapeReflowsUsingTheSameAdaptiveThreshold() {
        try {
            compose.activityRule.scenario.onActivity {
                it.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
            }
            compose.waitUntil(timeoutMillis = 10_000) {
                compose.activity.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
            }
            assertAdaptiveLayoutMatchesWidth()
            compose.onNodeWithTag("top-app-bar").assertIsDisplayed()
            compose.onNodeWithTag("screen-home").assertIsDisplayed()
        } finally {
            compose.activityRule.scenario.onActivity {
                it.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            }
            compose.waitUntil(timeoutMillis = 10_000) {
                compose.activity.resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
            }
            compose.activityRule.scenario.onActivity {
                it.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
            }
        }
    }

    @Test
    fun nonHomeBackReturnsToKitsuInsteadOfFinishingTheActivity() {
        compose.onNodeWithTag("nav-settings").performClick()
        compose.onNodeWithTag("screen-settings").assertIsDisplayed()

        compose.activityRule.scenario.onActivity {
            it.onBackPressedDispatcher.onBackPressed()
        }

        compose.waitUntil(timeoutMillis = 5_000) { tagCount("screen-home") == 1 }
        compose.onNodeWithTag("screen-home").assertIsDisplayed()
    }

    @Test
    fun settingsOffersPolicyBoundVoluntaryKofiSupport() {
        compose.onNodeWithTag("nav-settings").performClick()
        compose.onNodeWithTag("screen-settings")
            .performScrollToNode(hasTestTag("open-support-kofi"))

        compose.onNodeWithTag("settings-support-kofi").assertIsDisplayed()
        compose.onNodeWithTag("open-support-kofi")
            .assertIsDisplayed()
            .assertIsEnabled()
            .assertHasClickAction()
        compose.onNodeWithText("Supporting Kitsu unlocks no app feature", substring = true)
            .assertIsDisplayed()
        compose.onNodeWithText("Kitsu itself keeps no internet permission", substring = true)
            .assertIsDisplayed()
    }

    @Test
    fun freshHomeHasOnePrimarySetupAction() {
        val viewModel = ViewModelProvider(compose.activity)[MainViewModel::class.java]
        compose.waitUntil(timeoutMillis = 10_000) { viewModel.owner.value.savedKitsu.isNotEmpty() }
        val fixtureAddress = requireNotNull(viewModel.owner.value.savedKitsu.singleOrNull()?.deviceAddress)
        try {
            compose.waitUntil(timeoutMillis = 10_000) { viewModel.owner.value.connection.connected }
            viewModel.forgetController(fixtureAddress)
            compose.waitUntil(timeoutMillis = 10_000) { viewModel.owner.value.savedKitsu.isEmpty() }

            compose.onNodeWithTag("nav-home").performClick()
            compose.waitUntil(timeoutMillis = 5_000) { tagCount("screen-home") == 1 }
            compose.onAllNodesWithText("Set up a Kitsu").assertCountEquals(1)
            compose.onNodeWithText("Set up a Kitsu").assertHasClickAction().assertIsEnabled()
            compose.onAllNodesWithText("Open device setup").assertCountEquals(0)
        } finally {
            viewModel.pairController("Pocket Kitsu")
            compose.waitUntil(timeoutMillis = 10_000) { viewModel.owner.value.savedKitsu.isNotEmpty() }
        }
    }

    @Test
    fun meshPolicyGatesCompositionAndAcceptancePersistsAcrossRecreation() {
        openMessages()
        openFixtureDirectThread()
        compose.onNodeWithTag("message-policy-gate").assertIsDisplayed()
        compose.onNodeWithText("Review").performClick()

        compose.onNodeWithText("Mesh messaging terms & user policy").assertIsDisplayed()
        compose.onNodeWithTag("message-policy-content").assertIsDisplayed()
        compose.onNodeWithText("Kitsu Companion has no internet permission", substring = true)
            .assertExists()
        compose.onNodeWithTag("accept-message-policy").assertIsEnabled().performClick()

        compose.onNodeWithTag("conversation-body").assertIsEnabled()

        compose.activityRule.scenario.recreate()
        compose.waitForIdle()
        compose.onNodeWithTag("conversation-composer").assertExists()
        assertEquals(
            MeshUserPolicy.VERSION,
            ModerationPreferences(targetContext).acceptedPolicyVersion(),
        )
        compose.onNodeWithTag("conversation-body").assertIsEnabled()
    }

    @Test
    fun messageDraftAndSelectedTabSurviveRecreationAndImeControlsRemainReachable() {
        openMessages()
        openFixtureDirectThread()
        acceptPolicyIfRequired()
        val draft = "Adaptive IME and saved-state acceptance"

        compose.onNodeWithTag("conversation-body").performTextReplacement("kept")
        compose.onNodeWithTag("conversation-body").performTextReplacement("🦊".repeat(33))
        compose.onNodeWithTag("conversation-body").assertTextContains("kept")
        compose.onNodeWithTag("conversation-body").performTextReplacement(draft)
        compose.onNodeWithTag("conversation-body").assertTextContains(draft)
        compose.onNodeWithTag("conversation-body").performClick()
        compose.onNodeWithTag("conversation-body").assertIsFocused()
        compose.onNodeWithTag("conversation-send").assertIsDisplayed().assertHasClickAction()
        compose.waitForIdle()
        compose.onNodeWithTag("conversation-body").assertTextContains(draft)

        compose.activityRule.scenario.recreate()
        compose.waitForIdle()
        compose.onNodeWithTag("conversation-composer").assertExists()

        compose.onNodeWithTag("conversation-body").assertTextContains(draft).assertIsDisplayed()
        compose.onNodeWithTag("conversation-send").assertIsDisplayed().assertHasClickAction()
        compose.onNodeWithTag("conversation-composer").assertExists()
    }

    @Test
    fun inboundChannelSenderIsAlwaysVisiblyAndAccessiblyMarkedUnverified() {
        openMessages()
        compose.onNodeWithTag("message-thread-channel:0").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("conversation-composer") == 1 }
        compose.onNodeWithTag("conversation-list")
            .performScrollToNode(hasTestTag("message-bubble-15:103"))

        compose.onAllNodesWithText("Shade · unverified")[0].assertIsDisplayed()
        compose.onNodeWithTag("message-bubble-15:103").assert(
            SemanticsMatcher("channel sender trust label is spoken") { node ->
                node.config[SemanticsProperties.ContentDescription]
                    .any { description -> "Shade · unverified" in description }
            },
        )
    }

    @Test
    fun incomingDirectMessageKeepsAuthenticatedKeyAndModerationActionVisible() {
        openMessages()
        openFixtureDirectThread()
        compose.onNodeWithTag("conversation-list")
            .performScrollToNode(hasTestTag("message-bubble-15:101"))

        compose.onNodeWithTag("message-peer-key-15:101")
            .assertIsDisplayed()
            .assertTextContains("Key AAAAAAA…AAAAA")
        compose.onNodeWithTag("message-actions-15:101").assertIsDisplayed()
        compose.onNodeWithTag("message-bubble-15:101").assert(
            SemanticsMatcher("authenticated direct key is spoken") { node ->
                node.config[SemanticsProperties.ContentDescription]
                    .any { description -> "key AAAAAAA…AAAAA" in description }
            },
        )
    }

    private fun openMessages() {
        compose.onNodeWithTag("nav-messages").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("messages-thread-list") == 1 }
    }

    private fun openFixtureDirectThread() {
        compose.waitUntil(timeoutMillis = 5_000) {
            tagCount("message-thread-direct:${"A".repeat(43)}") == 1
        }
        compose.onNodeWithTag("message-thread-direct:${"A".repeat(43)}").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("conversation-composer") == 1 }
    }

    private fun acceptPolicyIfRequired() {
        if (tagCount("message-policy-gate") == 0) return
        compose.onNodeWithText("Review").performClick()
        compose.onNodeWithTag("accept-message-policy").performClick()
        compose.waitForIdle()
    }

    private fun assertAdaptiveLayoutMatchesWidth() {
        val expanded = compose.activity.resources.configuration.screenWidthDp >= NAVIGATION_RAIL_MIN_WIDTH_DP
        val expectedLayout = if (expanded) "layout-expanded" else "layout-compact"
        compose.waitUntil(timeoutMillis = 5_000) { tagCount(expectedLayout) == 1 }
        assertEquals(if (expanded) 1 else 0, tagCount("navigation-rail"))
        assertEquals(if (expanded) 0 else 1, tagCount("bottom-navigation"))
        assertEquals(if (expanded) 1 else 0, tagCount("layout-expanded"))
        assertEquals(if (expanded) 0 else 1, tagCount("layout-compact"))
    }

    private fun tagCount(tag: String): Int =
        compose.onAllNodesWithTag(tag).fetchSemanticsNodes().size

    private companion object {
        const val APPEARANCE_PREFERENCES = "kitsu_appearance"
        const val MODERATION_PREFERENCES = "kitsu_mesh_moderation"
        const val NAVIGATION_RAIL_MIN_WIDTH_DP = 720
    }
}
