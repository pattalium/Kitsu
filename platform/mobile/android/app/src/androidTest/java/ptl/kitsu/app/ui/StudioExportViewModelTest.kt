package ptl.kitsu.app.ui

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.ViewModelStoreOwner
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.awaitCancellation
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import ptl.kitsu.app.media.PetStudioPlacement

@RunWith(AndroidJUnit4::class)
class StudioExportViewModelTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context = ApplicationProvider.getApplicationContext<Context>()

    @Test
    fun viewModelStoreRetainsPendingRequestAcrossOwnerRecreation() {
        val bitmap = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888)

        instrumentation.runOnMainSync {
            val store = ViewModelStore()
            val storeOwner = object : ViewModelStoreOwner {
                override val viewModelStore: ViewModelStore = store
            }
            val first = ViewModelProvider(storeOwner)[StudioExportViewModel::class.java]
            val preparation = requireNotNull(first.beginPreparation(StudioExportKind.PNG))
            assertTrue(first.stagePng(preparation, bitmap))

            // A recreated Activity receives the same retained ViewModelStore.
            val recreated = ViewModelProvider(storeOwner)[StudioExportViewModel::class.java]
            assertSame(first, recreated)
            assertEquals(StudioExportKind.PNG, recreated.pendingKind)
            assertFalse(bitmap.isRecycled)

            store.clear()
            assertTrue(bitmap.isRecycled)
        }
    }

    @Test
    fun mismatchedPickerResultDeletesOrphanWithoutConsumingPng() {
        val cleaned = mutableListOf<Uri>()
        val bitmap = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888)
        lateinit var owner: StudioExportViewModel

        instrumentation.runOnMainSync {
            owner = StudioExportViewModel(
                orphanCleaner = { _, uri -> cleaned += uri },
            )
            val preparation = requireNotNull(owner.beginPreparation(StudioExportKind.PNG))
            assertTrue(owner.stagePng(preparation, bitmap))

            val orphan = Uri.parse("content://kitsu.test/orphan-video")
            owner.handleVideoResult(context, orphan) {}

            assertEquals(listOf(orphan), cleaned)
            assertEquals(StudioExportKind.PNG, owner.pendingKind)
            assertTrue(owner.isBusy)
            assertFalse(bitmap.isRecycled)

            owner.handlePngResult(context, null) {}
            assertNull(owner.pendingKind)
            assertFalse(owner.isBusy)
            assertTrue(bitmap.isRecycled)
        }
    }

    @Test
    fun matchingPngResultWritesOnceAndRecyclesOwnedBitmap() {
        val writes = AtomicInteger()
        val bitmap = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888)
        val notices = mutableListOf<String>()

        instrumentation.runOnMainSync {
            val owner = StudioExportViewModel(
                pngWriter = { _, _, owned ->
                    assertTrue(owned === bitmap)
                    assertFalse(owned.isRecycled)
                    writes.incrementAndGet()
                    true
                },
            )
            val preparation = requireNotNull(owner.beginPreparation(StudioExportKind.PNG))
            assertTrue(owner.stagePng(preparation, bitmap))

            owner.handlePngResult(
                context,
                Uri.parse("content://kitsu.test/studio.png"),
                notices::add,
            )

            assertEquals(1, writes.get())
            assertEquals(listOf("Studio image saved."), notices)
            assertFalse(owner.isBusy)
            assertNull(owner.pendingKind)
            assertTrue(bitmap.isRecycled)
        }
    }

    @Test
    fun pendingVideoRejectsOverlapAndRouteExitRecyclesItExactlyOnce() {
        val background = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888)

        instrumentation.runOnMainSync {
            val owner = StudioExportViewModel()
            val preparation = requireNotNull(owner.beginPreparation(StudioExportKind.VIDEO))
            assertTrue(
                owner.stageVideo(
                    preparation,
                    StudioVideoRequest(
                        background = background,
                        frames = emptyList(),
                        placement = PetStudioPlacement(),
                    ),
                ),
            )

            assertNull(owner.beginPreparation(StudioExportKind.PNG))
            assertEquals(StudioExportKind.VIDEO, owner.pendingKind)
            owner.clearForRouteExit()
            owner.clearForRouteExit()

            assertTrue(background.isRecycled)
            assertFalse(owner.isBusy)
            assertNull(owner.pendingKind)
        }
    }

    @Test
    fun routeExitCancelsActiveExportAndRecyclesItsBitmap() {
        val writerStarted = CountDownLatch(1)
        val writerCancelled = CountDownLatch(1)
        val bitmap = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888)
        lateinit var owner: StudioExportViewModel

        instrumentation.runOnMainSync {
            owner = StudioExportViewModel(
                pngWriter = { _, _, _ ->
                    writerStarted.countDown()
                    try {
                        awaitCancellation()
                    } finally {
                        writerCancelled.countDown()
                    }
                },
            )
            val preparation = requireNotNull(owner.beginPreparation(StudioExportKind.PNG))
            assertTrue(owner.stagePng(preparation, bitmap))
            owner.handlePngResult(context, Uri.parse("content://kitsu.test/pending.png")) {}
        }

        assertTrue(writerStarted.await(2, TimeUnit.SECONDS))
        instrumentation.runOnMainSync { owner.clearForRouteExit() }
        assertTrue(writerCancelled.await(2, TimeUnit.SECONDS))
        instrumentation.waitForIdleSync()
        assertTrue(bitmap.isRecycled)
        assertFalse(owner.isBusy)
    }
}
