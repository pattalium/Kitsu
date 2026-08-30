package ptl.kitsu.app.media

import android.graphics.Bitmap
import android.graphics.Color
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PetStudioPolicyTest {
    @Test
    fun transparentCanvasKeepsBackgroundTransparentAndPlacesPet() {
        val pet = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888).apply {
            eraseColor(Color.WHITE)
        }
        val output = PetStudioPolicy.transparentCanvas(
            pet = pet,
            placement = PetStudioPlacement(horizontal = 0.5f, vertical = 0.75f, scale = 12f),
            width = 64,
            height = 64,
        )

        assertEquals(Color.TRANSPARENT, output.getPixel(0, 0))
        assertEquals(Color.WHITE, output.getPixel(32, 47))
        output.recycle()
        pet.recycle()
    }

    @Test
    fun squareExportUsesTheSameNormalizedPlacementAndCenterCropAsPreview() {
        val background = Bitmap.createBitmap(100, 50, Bitmap.Config.ARGB_8888).apply {
            for (x in 0 until width) {
                for (y in 0 until height) {
                    setPixel(x, y, if (x < 50) Color.RED else Color.BLUE)
                }
            }
        }
        val pet = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888).apply {
            eraseColor(Color.WHITE)
        }
        val placement = PetStudioPlacement(horizontal = 0.5f, vertical = 0.75f, scale = 12f)
        val layout = PetStudioPolicy.layout(pet.width, pet.height, placement)
        val output = PetStudioPolicy.squareComposite(background, pet, placement, size = 64)

        assertEquals(placement.horizontal - layout.widthFraction / 2f, layout.leftFraction)
        assertEquals(placement.vertical - layout.heightFraction, layout.topFraction)
        assertEquals(Color.RED, output.getPixel(0, 0))
        assertEquals(Color.BLUE, output.getPixel(63, 0))
        assertEquals(Color.WHITE, output.getPixel(32, 47))
        output.recycle()
        pet.recycle()
        background.recycle()
    }

    @Test
    fun clipAndPlacementBoundsAreStrict() {
        assertTrue(PetStudioPolicy.validPlacement(PetStudioPlacement()))
        assertFalse(PetStudioPolicy.validPlacement(PetStudioPlacement(scale = Float.NaN)))
        assertTrue(PetStudioPolicy.canAppendFrame(0))
        assertEquals(24, PetStudioPolicy.MAX_CLIP_FRAMES)
        assertFalse(PetStudioPolicy.canAppendFrame(PetStudioPolicy.MAX_CLIP_FRAMES))
    }
}
