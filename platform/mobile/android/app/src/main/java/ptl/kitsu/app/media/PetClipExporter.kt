package ptl.kitsu.app.media

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import java.io.FileDescriptor
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlin.coroutines.coroutineContext

data class PetClipExportResult(
    val frameCount: Int,
    val width: Int,
    val height: Int,
    val framesPerSecond: Int,
)

/** Writes a short, local-only MP4 from already composed studio frames. */
object PetClipExporter {
    private const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
    private const val OUTPUT_TIMEOUT_US = 20_000L

    suspend fun writeMp4(
        frames: List<Bitmap>,
        destination: FileDescriptor,
        framesPerSecond: Int = PetStudioPolicy.DEFAULT_CLIP_FPS,
        width: Int = PetStudioPolicy.VIDEO_OUTPUT_SIZE,
        height: Int = PetStudioPolicy.VIDEO_OUTPUT_SIZE,
        backgroundColor: Int = Color.BLACK,
    ): PetClipExportResult {
        require(frames.none { it.isRecycled }) { "recycled_pet_clip_frame" }
        return writeMp4Internal(
            frameCount = frames.size,
            destination = destination,
            framesPerSecond = framesPerSecond,
            width = width,
            height = height,
            backgroundColor = backgroundColor,
            frameAt = frames::get,
            recycleGeneratedFrames = false,
        )
    }

    /** Encodes generated frames one at a time so a clip never retains every ARGB canvas. */
    suspend fun writeGeneratedMp4(
        frameCount: Int,
        destination: FileDescriptor,
        framesPerSecond: Int = PetStudioPolicy.DEFAULT_CLIP_FPS,
        width: Int = PetStudioPolicy.VIDEO_OUTPUT_SIZE,
        height: Int = PetStudioPolicy.VIDEO_OUTPUT_SIZE,
        backgroundColor: Int = Color.BLACK,
        frameAt: (Int) -> Bitmap,
    ): PetClipExportResult = writeMp4Internal(
        frameCount = frameCount,
        destination = destination,
        framesPerSecond = framesPerSecond,
        width = width,
        height = height,
        backgroundColor = backgroundColor,
        frameAt = frameAt,
        recycleGeneratedFrames = true,
    )

    private suspend fun writeMp4Internal(
        frameCount: Int,
        destination: FileDescriptor,
        framesPerSecond: Int,
        width: Int,
        height: Int,
        backgroundColor: Int,
        frameAt: (Int) -> Bitmap,
        recycleGeneratedFrames: Boolean,
    ): PetClipExportResult = withContext(Dispatchers.IO) {
        require(frameCount in 2..PetStudioPolicy.MAX_CLIP_FRAMES) { "invalid_pet_clip_frames" }
        require(framesPerSecond in 1..30) { "invalid_pet_clip_frame_rate" }
        require(width in 64..1920 && height in 64..1920 && width % 2 == 0 && height % 2 == 0) {
            "invalid_pet_clip_dimensions"
        }
        val codec = MediaCodec.createEncoderByType(MIME)
        var muxer: MediaMuxer? = null
        var inputSurface: android.view.Surface? = null
        var codecStarted = false
        var muxerStarted = false
        var trackIndex = -1
        var exportFailure: Throwable? = null
        try {
            val format = MediaFormat.createVideoFormat(MIME, width, height).apply {
                setInteger(
                    MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface,
                )
                setInteger(MediaFormat.KEY_BIT_RATE, (width * height * framesPerSecond * 0.22f).toInt())
                setInteger(MediaFormat.KEY_FRAME_RATE, framesPerSecond)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            }
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val surface = codec.createInputSurface()
            inputSurface = surface
            val outputMuxer = MediaMuxer(destination, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            muxer = outputMuxer
            codec.start()
            codecStarted = true
            val bufferInfo = MediaCodec.BufferInfo()

            fun drain(endOfStream: Boolean): Boolean {
                val deadlineNanos = if (endOfStream) {
                    System.nanoTime() + 10_000_000_000L
                } else {
                    0L
                }
                while (true) {
                    val outputIndex = codec.dequeueOutputBuffer(
                        bufferInfo,
                        if (endOfStream) OUTPUT_TIMEOUT_US else 0L,
                    )
                    when {
                        outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER -> {
                            if (!endOfStream) return false
                            check(System.nanoTime() < deadlineNanos) { "pet_clip_encoder_timeout" }
                        }
                        outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            check(!muxerStarted) { "pet_clip_format_changed_twice" }
                            trackIndex = outputMuxer.addTrack(codec.outputFormat)
                            outputMuxer.start()
                            muxerStarted = true
                        }
                        outputIndex >= 0 -> {
                            val output = codec.getOutputBuffer(outputIndex)
                                ?: error("pet_clip_output_buffer_missing")
                            if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                                bufferInfo.size = 0
                            }
                            if (bufferInfo.size > 0) {
                                check(muxerStarted && trackIndex >= 0) { "pet_clip_muxer_not_ready" }
                                output.position(bufferInfo.offset)
                                output.limit(bufferInfo.offset + bufferInfo.size)
                                outputMuxer.writeSampleData(trackIndex, output, bufferInfo)
                            }
                            val finished = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0
                            codec.releaseOutputBuffer(outputIndex, false)
                            if (finished) return true
                        }
                    }
                }
            }

            val framePeriodNanos = 1_000_000_000L / framesPerSecond
            val startedAt = System.nanoTime()
            repeat(frameCount) { index ->
                coroutineContext.ensureActive()
                val bitmap = frameAt(index)
                try {
                    require(!bitmap.isRecycled) { "recycled_pet_clip_frame" }
                    val canvas = surface.lockCanvas(null)
                    try {
                        drawFrame(canvas, bitmap, width, height, backgroundColor)
                    } finally {
                        surface.unlockCanvasAndPost(canvas)
                    }
                } finally {
                    if (recycleGeneratedFrames && !bitmap.isRecycled) bitmap.recycle()
                }
                drain(endOfStream = false)
                if (index != frameCount - 1) {
                    val deadline = startedAt + (index + 1L) * framePeriodNanos
                    val remaining = deadline - System.nanoTime()
                    if (remaining > 0L) {
                        val milliseconds = remaining / 1_000_000L
                        val nanoseconds = (remaining % 1_000_000L).toInt()
                        Thread.sleep(milliseconds, nanoseconds)
                    }
                }
            }
            codec.signalEndOfInputStream()
            check(drain(endOfStream = true)) { "pet_clip_missing_end_of_stream" }
            check(muxerStarted) { "pet_clip_empty_output" }

            PetClipExportResult(frameCount, width, height, framesPerSecond)
        } catch (failure: Throwable) {
            exportFailure = failure
            throw failure
        } finally {
            if (codecStarted) runCatching { codec.stop() }
            runCatching { codec.release() }
            inputSurface?.let { runCatching { it.release() } }
            val muxerStopFailure = if (muxerStarted) {
                muxer?.let { runCatching { it.stop() }.exceptionOrNull() }
            } else {
                null
            }
            muxer?.let { runCatching { it.release() } }
            muxerStopFailure?.let { failure ->
                val primary = exportFailure
                if (primary == null) throw failure else primary.addSuppressed(failure)
            }
        }
    }

    private fun drawFrame(
        canvas: Canvas,
        bitmap: Bitmap,
        width: Int,
        height: Int,
        backgroundColor: Int,
    ) {
        canvas.drawColor(backgroundColor)
        val sourceAspect = bitmap.width.toFloat() / bitmap.height
        val outputAspect = width.toFloat() / height
        val destination = if (sourceAspect > outputAspect) {
            val scaledHeight = (width / sourceAspect).toInt()
            val top = (height - scaledHeight) / 2
            Rect(0, top, width, top + scaledHeight)
        } else {
            val scaledWidth = (height * sourceAspect).toInt()
            val left = (width - scaledWidth) / 2
            Rect(left, 0, left + scaledWidth, height)
        }
        canvas.drawBitmap(
            bitmap,
            null,
            destination,
            Paint(Paint.ANTI_ALIAS_FLAG).apply { isFilterBitmap = true },
        )
    }
}
