package ptl.kitsu.app.media

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect

data class PetStudioPlacement(
    val horizontal: Float = 0.5f,
    val vertical: Float = 0.82f,
    val scale: Float = 4f,
)

data class PetStudioLayout(
    val leftFraction: Float,
    val topFraction: Float,
    val widthFraction: Float,
    val heightFraction: Float,
)

object PetStudioPolicy {
    const val MIN_SCALE = 1f
    const val MAX_SCALE = 12f
    const val DEFAULT_CLIP_FPS = 8
    const val MAX_CLIP_SECONDS = 3
    const val MAX_CLIP_FRAMES = DEFAULT_CLIP_FPS * MAX_CLIP_SECONDS
    const val VIDEO_CANVAS_SIZE = 720
    const val VIDEO_OUTPUT_SIZE = 512

    fun validPlacement(value: PetStudioPlacement): Boolean =
        value.horizontal in 0f..1f && value.vertical in 0f..1f &&
            value.scale.isFinite() && value.scale in MIN_SCALE..MAX_SCALE

    /** One shared coordinate system for the Compose preview and every export format. */
    fun layout(
        petWidth: Int,
        petHeight: Int,
        placement: PetStudioPlacement,
    ): PetStudioLayout {
        require(petWidth > 0 && petHeight > 0) { "invalid_pet_frame_dimensions" }
        require(validPlacement(placement)) { "invalid_pet_studio_placement" }
        val widthFraction = petWidth * placement.scale / VIDEO_CANVAS_SIZE
        val heightFraction = petHeight * placement.scale / VIDEO_CANVAS_SIZE
        return PetStudioLayout(
            leftFraction = placement.horizontal - widthFraction / 2f,
            topFraction = placement.vertical - heightFraction,
            widthFraction = widthFraction,
            heightFraction = heightFraction,
        )
    }

    fun composite(
        background: Bitmap,
        pet: Bitmap,
        placement: PetStudioPlacement,
    ): Bitmap {
        require(validPlacement(placement)) { "invalid_pet_studio_placement" }
        return PetFrameRenderer.composite(
            background = background,
            pet = pet,
            centerX = background.width * placement.horizontal,
            bottomY = background.height * placement.vertical,
            scale = placement.scale,
        )
    }

    fun transparentCanvas(
        pet: Bitmap,
        placement: PetStudioPlacement,
        width: Int = 1024,
        height: Int = 1024,
    ): Bitmap {
        require(validPlacement(placement)) { "invalid_pet_studio_placement" }
        require(width in 64..4096 && height in 64..4096) { "invalid_pet_studio_canvas" }
        val background = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        background.eraseColor(Color.TRANSPARENT)
        return try {
            PetFrameRenderer.composite(
                background = background,
                pet = pet,
                centerX = width * placement.horizontal,
                bottomY = height * placement.vertical,
                scale = placement.scale * width / VIDEO_CANVAS_SIZE,
            )
        } finally {
            background.recycle()
        }
    }

    /** Matches the square, center-cropped Studio preview at a bounded output size. */
    fun squareComposite(
        background: Bitmap,
        pet: Bitmap,
        placement: PetStudioPlacement,
        size: Int = VIDEO_CANVAS_SIZE,
    ): Bitmap {
        require(validPlacement(placement)) { "invalid_pet_studio_placement" }
        require(size in 64..1_024) { "invalid_pet_studio_canvas" }
        val square = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val shortest = minOf(background.width, background.height)
        val left = (background.width - shortest) / 2
        val top = (background.height - shortest) / 2
        Canvas(square).drawBitmap(
            background,
            Rect(left, top, left + shortest, top + shortest),
            Rect(0, 0, size, size),
            Paint(Paint.ANTI_ALIAS_FLAG).apply { isFilterBitmap = true },
        )
        return try {
            PetFrameRenderer.composite(
                background = square,
                pet = pet,
                centerX = size * placement.horizontal,
                bottomY = size * placement.vertical,
                scale = placement.scale * size / VIDEO_CANVAS_SIZE,
            )
        } finally {
            square.recycle()
        }
    }

    fun canAppendFrame(currentCount: Int): Boolean = currentCount in 0 until MAX_CLIP_FRAMES
}
