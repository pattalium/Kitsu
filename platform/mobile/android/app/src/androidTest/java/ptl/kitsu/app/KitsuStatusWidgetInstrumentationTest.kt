package ptl.kitsu.app

import android.appwidget.AppWidgetManager
import android.content.ComponentName
import android.content.pm.PackageManager
import android.graphics.drawable.BitmapDrawable
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.ProgressBar
import android.widget.TextView
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import ptl.kitsu.app.cache.CacheSnapshot
import ptl.kitsu.app.cache.EncryptedBoundedCache
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.NeedLevels
import ptl.kitsu.app.navigation.AppLaunchIntentPolicy
import ptl.kitsu.app.navigation.AppLaunchIntentResult
import ptl.kitsu.app.navigation.AppRoute
import ptl.kitsu.app.security.AndroidKeystoreCredentialStore
import ptl.kitsu.app.security.BondedCompanion
import ptl.kitsu.app.widget.KitsuStatusWidgetIntents
import ptl.kitsu.app.widget.KitsuStatusWidgetPresentationPolicy
import ptl.kitsu.app.widget.KitsuStatusWidgetProvider
import ptl.kitsu.app.widget.KitsuStatusWidgetRenderer
import ptl.kitsu.app.widget.KitsuStatusWidgetSnapshotLoader
import ptl.kitsu.app.widget.KitsuStatusWidgetSource

@RunWith(AndroidJUnit4::class)
class KitsuStatusWidgetInstrumentationTest {
    private val context
        get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test fun remoteViewsRendersExactStatusKnownPortraitAndHiddenOptionalRows() {
        val presentation = KitsuStatusWidgetPresentationPolicy.present(
            source = KitsuStatusWidgetSource(
                status = status(),
                connected = true,
                snapshotAtEpochSeconds = 9_990,
            ),
            nowEpochSeconds = 10_000,
        )
        val root = KitsuStatusWidgetRenderer.render(context, presentation)
            .apply(context, FrameLayout(context))

        assertEquals("Shade", root.text(R.id.kitsu_widget_name))
        assertEquals("Mood · Content", root.text(R.id.kitsu_widget_mood))
        assertEquals("Energy 73%", root.text(R.id.kitsu_widget_energy))
        assertEquals("Battery 82%", root.text(R.id.kitsu_widget_battery))
        assertEquals("Connected · updated just now", root.text(R.id.kitsu_widget_freshness))
        assertEquals(73, root.findViewById<ProgressBar>(R.id.kitsu_widget_energy_progress).progress)
        assertEquals(View.GONE, root.findViewById<View>(R.id.kitsu_widget_request).visibility)
        assertEquals(View.GONE, root.findViewById<View>(R.id.kitsu_widget_focus).visibility)
        assertEquals(View.GONE, root.findViewById<View>(R.id.kitsu_widget_walk).visibility)

        val portrait = root.findViewById<ImageView>(R.id.kitsu_widget_portrait)
        assertEquals("Shade portrait", portrait.contentDescription.toString())
        val bitmap = (portrait.drawable as BitmapDrawable).bitmap
        assertEquals(64, bitmap.width)
        assertEquals(64, bitmap.height)
        assertTrue(root.contentDescription.toString().contains("Shade"))
    }

    @Test fun coldLoaderReadsOnlyTheActiveDevicesEncryptedSnapshotAsStale() = runBlocking {
        val cache = EncryptedBoundedCache(context)
        val credentials = AndroidKeystoreCredentialStore(context)
        runCatching(cache::clear)
        credentials.saveBondedCompanion(null)
        try {
            credentials.saveBondedCompanion(
                BondedCompanion(
                    deviceAddress = "AA:BB:CC:DD:EE:FF",
                    displayName = "Shade",
                    controllerIdB64 = "AAAAAAAAAAAAAAAAAAAAAA",
                    controllerRootB64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                ),
            )
            cache.write(
                CacheSnapshot(
                    status = status(),
                    writtenAt = 9_900,
                    deviceAddress = "AA:BB:CC:DD:EE:FF",
                ),
            )

            val source = KitsuStatusWidgetSnapshotLoader.load(context)
            assertEquals(status(), source.status)
            assertEquals(9_900L, source.snapshotAtEpochSeconds)
            assertFalse(source.connected)
            assertEquals(
                "Last synced 1m ago",
                KitsuStatusWidgetPresentationPolicy.present(source, nowEpochSeconds = 10_000)
                    .freshnessText,
            )

            credentials.saveBondedCompanion(
                BondedCompanion(
                    deviceAddress = "11:22:33:44:55:66",
                    displayName = "Other Kitsu",
                    controllerIdB64 = "AQEBAQEBAQEBAQEBAQEBAQ",
                    controllerRootB64 = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE",
                ),
            )
            assertNull(KitsuStatusWidgetSnapshotLoader.load(context).status)
        } finally {
            runCatching(cache::clear)
            credentials.saveBondedCompanion(null)
        }
    }

    @Test fun providerIsPrivateAndWidgetTapIntentOnlyRoutesToVisibleHome() {
        @Suppress("DEPRECATION")
        val receiver = context.packageManager.getReceiverInfo(
            ComponentName(context, KitsuStatusWidgetProvider::class.java),
            PackageManager.GET_META_DATA,
        )
        assertFalse(receiver.exported)
        assertEquals(
            R.xml.kitsu_status_widget_info,
            receiver.metaData.getInt(AppWidgetManager.META_DATA_APPWIDGET_PROVIDER),
        )

        val intent = KitsuStatusWidgetIntents.openHome(context)
        assertEquals(ComponentName(context, MainActivity::class.java), intent.component)
        assertEquals(AppLaunchIntentPolicy.ACTION_OPEN_HOME, intent.action)
        assertTrue(intent.extras == null || intent.extras!!.isEmpty)
        assertNull(intent.data)

        val route = AppLaunchIntentPolicy.parse(intent.action, intent.type, null)
            as AppLaunchIntentResult.Accepted
        assertEquals(AppRoute.Home, route.spec.route)
        assertNull(route.spec.messageDraft)
    }

    private fun View.text(id: Int): String = findViewById<TextView>(id).text.toString()

    private fun status() = KitsuStatus(
        deviceId = "KT0001",
        companionName = "Shade",
        mood = "CONTENT",
        batteryPercent = 82,
        batteryMillivolts = 3_990,
        packReady = true,
        packId = 0x6C393E21L.toString(),
        needs = NeedLevels(energy = 73, curiosity = 61, affection = 88),
        updatedAt = 9_970,
    )
}
