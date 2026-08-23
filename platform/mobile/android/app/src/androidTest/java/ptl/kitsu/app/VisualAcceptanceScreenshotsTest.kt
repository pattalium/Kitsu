package ptl.kitsu.app

import android.graphics.Bitmap
import android.content.res.Configuration
import android.os.SystemClock
import android.view.WindowInsets
import android.view.inputmethod.InputMethodManager
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsEnabled
import androidx.compose.ui.test.assertTextContains
import androidx.compose.ui.test.hasTestTag
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollToNode
import androidx.compose.ui.test.performTextInput
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.io.FileOutputStream
import kotlin.math.roundToInt
import kotlinx.coroutines.flow.MutableStateFlow
import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import ptl.kitsu.app.qa.FixtureScenario
import ptl.kitsu.app.ui.KitsuThemePreference
import ptl.kitsu.app.ui.KitsuThemePreferences
import ptl.kitsu.app.ui.ModerationPreferences
import ptl.kitsu.app.update.FirmwareInstallProgress
import ptl.kitsu.app.update.FirmwareInstallStage

/**
 * Renders release-review evidence against the instrumentation-only authenticated BLE fixture.
 * Files are written under the debug target's external files/visual-qa directory for collection.
 */
@RunWith(AndroidJUnit4::class)
class VisualAcceptanceScreenshotsTest {
    @get:Rule
    val compose = createAndroidComposeRule<MainActivity>()

    @Before
    fun forceDeterministicDarkAcceptedState() {
        FixtureScenario.reset()
        KitsuThemePreferences(compose.activity).set(KitsuThemePreference.DARK)
        ModerationPreferences(compose.activity).acceptCurrentPolicy()
        compose.activityRule.scenario.recreate()
        val viewModel = viewModel()
        compose.runOnIdle { viewModel.connect() }
        compose.waitUntil(timeoutMillis = 15_000) {
            val owner = viewModel.owner.value
                owner.connection.connected &&
                owner.messageProtocolVersion == 4 &&
                owner.messageJournalSession == FixtureScenario.JOURNAL_SESSION &&
                FixtureScenario.lastMessageOperation == "messages.get.v4" &&
                owner.messages.count {
                    it.journalSession == FixtureScenario.JOURNAL_SESSION
                } == 10
        }
        assertEquals(FixtureScenario.FIRMWARE_VERSION, viewModel.owner.value.status?.firmwareVersion)
    }

    @Test
    fun renderConnectedHome() {
        capture("home-connected-dark")
    }

    @Test
    fun renderMeshAdvertiseReadinessSuccessAndCooldown() {
        compose.onNodeWithTag("nav-network").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("advertise-card") == 1 }
        capture("mesh-advertise-ready")
        compose.onNodeWithTag("advertise-last-flood").assertDoesNotExist()
        compose.onNodeWithTag("advertise-last-nearby")
            .assertIsDisplayed()
            .assertTextContains("No Nearby advertisement recorded")
        compose.onNodeWithContentDescription(
            "repeat evidence does not apply",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-scope-mesh"))
        compose.onNodeWithTag("advertise-scope-mesh").performClick()
        compose.onNodeWithTag("advertise-last-nearby").assertDoesNotExist()
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-last-flood"))
        compose.onNodeWithTag("advertise-last-flood")
            .assertIsDisplayed()
            .assertTextContains("Sent · heard 1 repeat")
        showLastFloodAdvert()
        compose.onNodeWithText("Sent · heard 1 repeat").assertIsDisplayed()
        compose.onNodeWithText("Returned repeat heard · likely Copper Fox").assertIsDisplayed()
        compose.onNodeWithText("Listening").assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "matching rebroadcast packet copies heard locally",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "unauthenticated local path tokens are not a list of all repeaters or recipients",
            substring = true,
        ).assertIsDisplayed()
        capture("mesh-advertise-last-flood-listening")
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-scope-nearby"))
        compose.onNodeWithTag("advertise-scope-nearby").performClick()
        compose.onNodeWithTag("advertise-last-flood").assertDoesNotExist()
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-last-nearby"))
        compose.onNodeWithTag("advertise-last-nearby")
            .assertIsDisplayed()
            .assertTextContains("No Nearby advertisement recorded")
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-now"))
        compose.onNodeWithTag("advertise-now").performClick()
        compose.waitUntil(timeoutMillis = 5_000) {
            tagCount("advertise-cooldown") == 1 &&
                viewModel().owner.value.status?.mesh?.lastNearbyAdvert?.state == "queued"
        }
        compose.onNodeWithText("Queuing…").assertDoesNotExist()
        compose.onNodeWithTag("advertise-now").assertTextContains("Advertise now")
        // Nearby is zero-hop and must not overwrite the preceding Mesh-wide result.
        assertEquals(1, viewModel().owner.value.status?.mesh?.lastFloodAdvert?.repeatCount)
        compose.onNodeWithTag("advertise-last-flood").assertDoesNotExist()
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-last-nearby"))
        compose.onNodeWithTag("advertise-last-nearby")
            .assertIsDisplayed()
            .assertTextContains("Queued")
        compose.onNodeWithContentDescription(
            "Nearby is zero-hop and is not repeated",
            substring = true,
        ).assertIsDisplayed()
        capture("mesh-advertise-success")
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-scope-mesh"))
        compose.onNodeWithTag("advertise-scope-mesh").performClick()
        showLastFloodAdvert()
        compose.onNodeWithText("Sent · heard 1 repeat").assertIsDisplayed()
        capture("mesh-advertise-prior-mesh-wide-result")
        ViewModelProvider(compose.activity)[MainViewModel::class.java].clearNotice()
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-now"))
        waitForStableLayout()
        capture("mesh-advertise-cooldown")
    }

    @Test
    fun advertisementScopeNoRecordCardsNeverLeakAcrossSelection() {
        FixtureScenario.clearAdvertisementRecords()
        compose.runOnIdle { viewModel().refresh() }
        compose.waitUntil(timeoutMillis = 10_000) {
            viewModel().owner.value.status?.mesh?.let {
                it.lastNearbyAdvert == null && it.lastFloodAdvert == null
            } == true
        }

        compose.onNodeWithTag("nav-network").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("advertise-card") == 1 }
        compose.onNodeWithTag("advertise-last-nearby")
            .assertIsDisplayed()
            .assertTextContains("No Nearby advertisement recorded")
        compose.onNodeWithTag("advertise-last-flood").assertDoesNotExist()

        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-scope-mesh"))
        compose.onNodeWithTag("advertise-scope-mesh").performClick()
        compose.onNodeWithTag("advertise-last-nearby").assertDoesNotExist()
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-last-flood"))
        compose.onNodeWithTag("advertise-last-flood")
            .assertIsDisplayed()
            .assertTextContains("No Mesh-wide advertisement recorded")
    }

    @Test
    fun renderMeshWideAdvertisementQueuedEvidence() {
        compose.onNodeWithTag("nav-network").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("advertise-card") == 1 }
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-scope-mesh"))
        compose.onNodeWithTag("advertise-scope-mesh").performClick()
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-now"))
        compose.onNodeWithTag("advertise-now").performClick()
        compose.waitUntil(timeoutMillis = 10_000) {
            !viewModel().owner.value.meshAdvertisementInFlight &&
                viewModel().owner.value.status?.mesh?.lastFloodAdvert?.state == "queued"
        }
        showLastFloodAdvert()
        compose.onNodeWithText("Queued").assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "Last Mesh-wide advertisement. Queued",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "does not identify unique repeaters or confirm recipient delivery",
            substring = true,
        ).assertIsDisplayed()
        capture("mesh-advertise-last-flood-queued")
    }

    @Test
    fun renderMessagesThreadRoot() {
        openMessagesRoot()
        compose.onNodeWithTag(DIRECT_THREAD_TAG).assertIsDisplayed()
        compose.onNodeWithTag(CHANNEL_THREAD_TAG).assertIsDisplayed()
        capture("messages-thread-root")
    }

    @Test
    fun renderMessagesDirectHistoryAndReadState() {
        openDirectConversation()
        compose.waitUntil(timeoutMillis = 10_000) {
            viewModel().owner.value.messages.firstOrNull { it.id == "101" }?.unreadOnKitsu == false
        }
        compose.onNodeWithTag("conversation-list")
            .performScrollToNode(hasTestTag("message-read-15:101"))
        compose.onNodeWithTag("message-read-15:101")
            .assertTextContains("Read on Kitsu", substring = true)
        compose.onNodeWithTag("message-actions-15:101").assertIsDisplayed()
        capture("messages-direct-history-read")

        compose.onNodeWithTag("conversation-list")
            .performScrollToNode(hasTestTag("message-status-15:110"))
        compose.onNodeWithText("Delivered via 1 repeater", substring = true)
            .assertIsDisplayed()
        capture("messages-direct-latest-delivery")
    }

    @Test
    fun renderMessagesChannelConversation() {
        openMessagesRoot()
        compose.onNodeWithTag(CHANNEL_THREAD_TAG).performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("conversation-detail") == 1 }
        compose.onNodeWithTag("conversation-list")
            .performScrollToNode(hasTestTag("message-read-15:103"))
        compose.onNodeWithTag("conversation-channel-routing")
            .assertTextContains(
                "only repeaters configured to allow #EU can participate",
                substring = true,
            )
        compose.waitUntil(timeoutMillis = 10_000) {
            viewModel().owner.value.messages.firstOrNull { it.id == "103" }?.unreadOnKitsu == false
        }
        compose.onNodeWithTag("message-read-15:103")
            .assertTextContains("Read on Kitsu", substring = true)
        compose.onNodeWithTag("conversation-list")
            .performScrollToNode(hasTestTag("message-status-15:104"))
        compose.onNodeWithText("Sent · listening for repeats", substring = true).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "listening for a matching rebroadcast copy",
            substring = true,
        ).assertIsDisplayed()
        assertEquals(
            1,
            viewModel().owner.value.messages.count {
                it.journalSession == FixtureScenario.JOURNAL_SESSION && it.id == "104"
            },
        )

        FixtureScenario.observeChannelRepeat()
        compose.waitUntil(timeoutMillis = 10_000) {
            viewModel().owner.value.messages.singleOrNull {
                it.journalSession == FixtureScenario.JOURNAL_SESSION && it.id == "104"
            }?.repeatCount == 1
        }
        assertEquals(
            1,
            viewModel().owner.value.messages.count {
                it.journalSession == FixtureScenario.JOURNAL_SESSION && it.id == "104"
            },
        )
        compose.onNodeWithTag("conversation-list")
            .performScrollToNode(hasTestTag("message-status-15:104"))
        compose.onNodeWithText("Sent · heard 1 repeat · listening", substring = true).assertIsDisplayed()
        compose.onNodeWithText("Returned repeat heard · likely Copper Fox").assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "Kitsu locally observed 1 matching rebroadcast packet copy",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "last-hop path token 00",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "not a list of all repeaters or recipients",
            substring = true,
        ).assertIsDisplayed()
        compose.onNodeWithContentDescription(
            "Recipient reception remains unconfirmed",
            substring = true,
        ).assertIsDisplayed()
        capture("messages-channel-history-read")
    }

    @Test
    fun renderMessagesNewConversationManualKey() {
        openMessagesRoot()
        compose.onNodeWithTag("messages-new").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("message-target") == 1 }
        compose.onNodeWithTag("message-recipient-0")
            .assertTextContains(
                "only repeaters configured to allow #EU can participate",
                substring = true,
            )
        compose.onNodeWithTag("message-recipient-1")
            .assertTextContains(
                "depends on each repeater's channel configuration",
                substring = true,
            )
        compose.onNodeWithTag("message-target").performTextInput(FixtureScenario.MANUAL_PEER_KEY)
        compose.onNodeWithTag("start-manual-direct").assertIsEnabled()
        capture("messages-new-conversation-manual-key")
    }

    @Test
    fun renderMessagesDisconnectedDetail() {
        openDirectConversation()
        compose.runOnIdle { viewModel().disconnect() }
        compose.waitUntil(timeoutMillis = 10_000) { !viewModel().owner.value.connection.connected }
        compose.onNodeWithText("Offline · connect this Kitsu to send.").assertIsDisplayed()
        capture("messages-direct-disconnected")
    }

    @Test
    fun renderMessagesRefreshErrorWithRetainedThreads() {
        openMessagesRoot()
        FixtureScenario.failMessagesWith("fixture_messages_unavailable")
        compose.runOnIdle { viewModel().refresh() }
        compose.waitUntil(timeoutMillis = 10_000) { tagCount("messages-error") == 1 }
        compose.onNodeWithTag(DIRECT_THREAD_TAG).assertIsDisplayed()
        capture("messages-thread-refresh-error")
    }

    @Test
    fun renderMessagesComposerWithIme() {
        openDirectConversation()
        compose.onNodeWithTag("conversation-body").performClick()
        compose.onNodeWithTag("conversation-body").performTextInput("Meet near the pines")
        compose.onNodeWithTag("conversation-send").assertIsEnabled()
        compose.runOnIdle {
            val inputMethod = compose.activity.getSystemService(InputMethodManager::class.java)
            inputMethod.showSoftInput(compose.activity.currentFocus, InputMethodManager.SHOW_IMPLICIT)
        }
        compose.waitUntil(timeoutMillis = 5_000) {
            compose.activity.window.decorView.rootWindowInsets
                ?.isVisible(WindowInsets.Type.ime()) == true
        }
        capture("messages-direct-composer-ime")
    }

    @Test
    fun renderMessagesThreadRootUsingSystemTheme() {
        KitsuThemePreferences(compose.activity).set(KitsuThemePreference.SYSTEM)
        compose.activityRule.scenario.recreate()
        compose.waitUntil(timeoutMillis = 10_000) { viewModel().owner.value.connection.connected }
        openMessagesRoot()
        capture("messages-thread-root-system-theme")
    }

    @Test
    fun renderMessagesDirectUsingSystemTheme() {
        KitsuThemePreferences(compose.activity).set(KitsuThemePreference.SYSTEM)
        compose.activityRule.scenario.recreate()
        compose.waitUntil(timeoutMillis = 10_000) { viewModel().owner.value.connection.connected }
        openDirectConversation()
        compose.onNodeWithTag("conversation-list")
            .performScrollToNode(hasTestTag("message-status-15:110"))
        compose.onNodeWithText("Delivered via 1 repeater", substring = true)
            .assertIsDisplayed()
        capture("messages-direct-system-theme")
    }

    @Test
    fun renderMessagesTabletMasterDetail() {
        assumeTrue(compose.activity.resources.configuration.screenWidthDp >= 720)
        openMessagesRoot()
        compose.onNodeWithTag("layout-messages-master-detail").assertIsDisplayed()
        compose.onNodeWithTag(DIRECT_THREAD_TAG).performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("conversation-detail") == 1 }
        compose.onNodeWithTag("conversation-title").assertTextContains("Copper Fox")
        capture("messages-tablet-master-detail")
    }

    @Test
    fun renderSettingsAppearanceAndFirmware() {
        compose.onNodeWithTag("nav-settings").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("screen-settings") == 1 }
        capture("settings-appearance")
        compose.onNodeWithTag("screen-settings").performScrollToNode(hasTestTag("firmware-card"))
        compose.waitForIdle()
        capture("settings-firmware")
    }

    @Test
    fun renderOtaLock() {
        compose.onNodeWithTag("nav-settings").performClick()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("screen-settings") == 1 }
        setOtaFixture()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("firmware-update-lock") == 1 }
        capture("ota-lock")
    }

    @Suppress("UNCHECKED_CAST")
    private fun setOtaFixture() {
        val viewModel = ViewModelProvider(compose.activity)[MainViewModel::class.java]
        val field = MainViewModel::class.java.getDeclaredField("mutableFirmware").apply {
            isAccessible = true
        }
        val state = field.get(viewModel) as MutableStateFlow<FirmwareUpdateUiState>
        compose.runOnIdle {
            state.value = FirmwareUpdateUiState(
                progress = FirmwareInstallProgress(
                    stage = FirmwareInstallStage.TRANSFERRING,
                    firmwareVersion = "0.15.0-fixture",
                    bytesSent = 524_288,
                    imageBytes = 1_048_576,
                ),
                importedReleaseId = "visual-fixture",
                updateId = "visual-fixture-update",
            )
        }
    }

    private fun capture(name: String) {
        waitForStableLayout()
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val configuration = context.resources.configuration
        val width = configuration.screenWidthDp
        val orientation = if (configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            "landscape"
        } else {
            "portrait"
        }
        val systemIsDark = (configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        val actualDark = KitsuThemePreferences(context).current() == KitsuThemePreference.DARK ||
            systemIsDark
        val theme = if (actualDark) "dark" else "light"
        val fontPercent = (configuration.fontScale * 100).roundToInt()
        val destination = File(
            requireNotNull(context.getExternalFilesDir("visual-qa")),
            "${width}dp-$orientation-$theme-font$fontPercent-$name.png",
        )
        FileOutputStream(destination).use { output ->
            check(
                InstrumentationRegistry.getInstrumentation()
                    .uiAutomation
                    .takeScreenshot()
                    .compress(Bitmap.CompressFormat.PNG, 100, output),
            ) { "screenshot_encode_failed" }
        }
        check(destination.isFile && destination.length() > 10_000) { "screenshot_missing_or_empty" }
    }

    private fun waitForStableLayout() {
        compose.waitForIdle()
        SystemClock.sleep(500)
        compose.waitForIdle()
    }

    private fun tagCount(tag: String): Int =
        compose.onAllNodes(hasTestTag(tag)).fetchSemanticsNodes().size

    private fun showLastFloodAdvert() {
        compose.onNodeWithTag("screen-mesh")
            .performScrollToNode(hasTestTag("advertise-last-flood"))
        compose.onNodeWithTag("advertise-last-flood").assertIsDisplayed()
    }

    private fun viewModel(): MainViewModel =
        ViewModelProvider(compose.activity)[MainViewModel::class.java]

    private fun openMessagesRoot() {
        if (tagCount("messages-thread-list") == 1 && tagCount("conversation-detail") == 0) return
        if (tagCount("conversation-back") == 1) {
            compose.onNodeWithTag("conversation-back").performClick()
        } else {
            compose.onNodeWithTag("nav-messages").performClick()
        }
        compose.waitUntil(timeoutMillis = 5_000) { tagCount("messages-thread-list") == 1 }
    }

    private fun openDirectConversation() {
        openMessagesRoot()
        compose.waitUntil(timeoutMillis = 5_000) { tagCount(DIRECT_THREAD_TAG) == 1 }
        compose.onNodeWithTag(DIRECT_THREAD_TAG).performClick()
        compose.waitUntil(timeoutMillis = 5_000) {
            tagCount("conversation-detail") == 1 && tagCount("conversation-composer") == 1
        }
    }

    private companion object {
        const val DIRECT_THREAD_TAG =
            "message-thread-direct:${FixtureScenario.DIRECT_PEER_KEY}"
        const val CHANNEL_THREAD_TAG = "message-thread-channel:0"
    }
}
