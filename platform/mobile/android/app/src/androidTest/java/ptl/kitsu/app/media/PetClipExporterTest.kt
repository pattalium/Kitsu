package ptl.kitsu.app.media

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.media.MediaMetadataRetriever
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import java.io.FileOutputStream
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PetClipExporterTest {
    @Test
    fun twoFramesProduceDecodableLocalMp4() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val destination = context.cacheDir.resolve("pet-clip-${System.nanoTime()}.mp4")
        val frames = listOf(Color.MAGENTA, Color.CYAN).map { color ->
            Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888).apply { eraseColor(color) }
        }
        try {
            FileOutputStream(destination).use { stream ->
                val result = PetClipExporter.writeMp4(
                    frames = frames,
                    destination = stream.fd,
                    framesPerSecond = 4,
                    width = 64,
                    height = 64,
                )
                assertEquals(2, result.frameCount)
            }
            assertTrue(destination.length() > 0L)
            val metadata = MediaMetadataRetriever()
            try {
                metadata.setDataSource(destination.absolutePath)
                assertEquals("64", metadata.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH))
                assertEquals("64", metadata.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT))
                assertTrue(
                    (metadata.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)
                        ?.toLongOrNull() ?: 0L) > 0L,
                )
            } finally {
                metadata.release()
            }
        } finally {
            frames.forEach(Bitmap::recycle)
            destination.delete()
        }
    }

    @Test
    fun generatedFramesAreRecycledBeforeTheNextFrameIsAllocated() = runBlocking {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val destination = context.cacheDir.resolve("pet-stream-${System.nanoTime()}.mp4")
        var previous: Bitmap? = null
        try {
            FileOutputStream(destination).use { stream ->
                val result = PetClipExporter.writeGeneratedMp4(
                    frameCount = 3,
                    destination = stream.fd,
                    framesPerSecond = 4,
                    width = 64,
                    height = 64,
                ) { index ->
                    assertTrue(previous == null || previous!!.isRecycled)
                    Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888).apply {
                        eraseColor(if (index % 2 == 0) Color.YELLOW else Color.GREEN)
                        previous = this
                    }
                }
                assertEquals(3, result.frameCount)
            }
            assertTrue(previous?.isRecycled == true)
            assertTrue(destination.length() > 0L)
        } finally {
            previous?.takeUnless(Bitmap::isRecycled)?.recycle()
            destination.delete()
        }
    }
}
