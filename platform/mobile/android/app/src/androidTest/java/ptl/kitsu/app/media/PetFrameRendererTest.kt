package ptl.kitsu.app.media

import android.graphics.BitmapFactory
import android.graphics.Color
import androidx.test.ext.junit.runners.AndroidJUnit4
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PetFrameRendererTest {
    @Test
    fun xbmUsesRowMajorLeastSignificantBitOrder() {
        val frame = byteArrayOf(0b0000_0001, 0b1000_0000.toByte())

        val bitmap = PetFrameRenderer.decodeXbm(frame, width = 8, height = 2)

        assertEquals(Color.WHITE, bitmap.getPixel(0, 0))
        assertEquals(Color.TRANSPARENT, bitmap.getPixel(7, 0))
        assertEquals(Color.TRANSPARENT, bitmap.getPixel(0, 1))
        assertEquals(Color.WHITE, bitmap.getPixel(7, 1))
    }

    @Test
    fun frameByteCountIsExact() {
        assertThrows(IllegalArgumentException::class.java) {
            PetFrameRenderer.decodeXbm(ByteArray(511), 64, 64)
        }
    }

    @Test
    fun transparentFrameExportsAsReadablePng() {
        val source = PetFrameRenderer.decodeXbm(ByteArray(512), 64, 64)
        val output = ByteArrayOutputStream()

        PetFrameRenderer.writePng(source, output)

        val bytes = output.toByteArray()
        assertTrue(bytes.copyOfRange(0, 8).contentEquals(PNG_SIGNATURE))
        val decoded = BitmapFactory.decodeStream(ByteArrayInputStream(bytes))
        assertEquals(64, decoded.width)
        assertEquals(64, decoded.height)
    }

    private companion object {
        val PNG_SIGNATURE = byteArrayOf(
            0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        )
    }
}
