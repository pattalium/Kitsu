package ptl.kitsu.app.media

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import java.io.OutputStream

/** Renders the exact one-bit XBM frame supplied by the authenticated Kitsu API. */
object PetFrameRenderer {
    fun decodeXbm(
        frame: ByteArray,
        width: Int,
        height: Int,
        foreground: Int = Color.WHITE,
    ): Bitmap {
        require(width in 1..256 && height in 1..256) { "invalid_pet_frame_dimensions" }
        val rowBytes = (width + 7) / 8
        require(frame.size == rowBytes * height) { "invalid_pet_frame_bytes" }
        val pixels = IntArray(width * height)
        for (y in 0 until height) {
            for (x in 0 until width) {
                val value = frame[y * rowBytes + x / 8].toInt() and 0xff
                pixels[y * width + x] = if ((value and (1 shl (x and 7))) != 0) {
                    foreground
                } else {
                    Color.TRANSPARENT
                }
            }
        }
        return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
    }

    fun composite(
        background: Bitmap,
        pet: Bitmap,
        centerX: Float = background.width / 2f,
        bottomY: Float = background.height.toFloat(),
        scale: Float = 1f,
    ): Bitmap {
        require(scale > 0f && scale.isFinite()) { "invalid_pet_scale" }
        val output = background.copy(Bitmap.Config.ARGB_8888, true)
        val targetWidth = (pet.width * scale).toInt().coerceAtLeast(1)
        val targetHeight = (pet.height * scale).toInt().coerceAtLeast(1)
        val left = (centerX - targetWidth / 2f).toInt()
        val top = (bottomY - targetHeight).toInt()
        Canvas(output).drawBitmap(
            pet,
            null,
            Rect(left, top, left + targetWidth, top + targetHeight),
            Paint(Paint.ANTI_ALIAS_FLAG).apply { isFilterBitmap = false },
        )
        return output
    }

    fun writePng(bitmap: Bitmap, destination: OutputStream) {
        check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, destination)) {
            "pet_png_export_failed"
        }
        destination.flush()
    }
}
