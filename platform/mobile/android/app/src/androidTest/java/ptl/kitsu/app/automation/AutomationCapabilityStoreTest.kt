package ptl.kitsu.app.automation

import android.content.Context
import android.content.pm.ShortcutManager
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AutomationCapabilityStoreTest {
    @Test
    fun enableIsStableAndDisableRevokesImmediately() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val store = AutomationCapabilityStore(context)
        store.disable()
        try {
            val first = store.enable()
            assertEquals(43, first.length)
            assertEquals(first, store.enable())
            assertTrue(store.accepts(first))
            assertFalse(store.accepts("B".repeat(43)))

            store.disable()
            assertNull(store.enabledToken())
            assertFalse(store.accepts(first))
        } finally {
            store.disable()
        }
    }

    @Test
    fun everyInstalledShortcutTargetsThisExactBuildVariant() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val shortcuts = context.getSystemService(ShortcutManager::class.java).manifestShortcuts

        assertEquals(4, shortcuts.size)
        shortcuts.forEach { shortcut ->
            val intents = shortcut.intents.orEmpty()
            assertTrue(intents.isNotEmpty())
            intents.forEach { intent ->
                assertEquals(context.packageName, intent.component?.packageName)
                assertEquals("ptl.kitsu.app.MainActivity", intent.component?.className)
            }
        }
    }
}
