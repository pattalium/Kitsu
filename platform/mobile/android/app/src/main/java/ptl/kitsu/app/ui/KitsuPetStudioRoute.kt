package ptl.kitsu.app.ui

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.graphics.Matrix
import android.media.ExifInterface
import android.net.Uri
import android.os.Build
import android.provider.DocumentsContract
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.compose.viewModel
import java.util.Locale
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ptl.kitsu.app.media.PetClipExporter
import ptl.kitsu.app.media.PetFrameRenderer
import ptl.kitsu.app.media.PetStudioPlacement
import ptl.kitsu.app.media.PetStudioPolicy
import ptl.kitsu.app.model.PetPresentationState

internal data class StudioFrame(
    val bytes: ByteArray,
    val width: Int,
    val height: Int,
    val sha256: String,
)

internal data class StudioVideoRequest(
    val background: Bitmap,
    val frames: List<StudioFrame>,
    val placement: PetStudioPlacement,
)

internal enum class StudioExportKind {
    PNG,
    VIDEO,
}

internal data class StudioExportPreparation(
    val id: Long,
    val kind: StudioExportKind,
)

/** Owns prepared Studio exports while Android recreates the route around a SAF picker. */
internal class StudioExportViewModel(
    private val pngWriter: suspend (Context, Uri, Bitmap) -> Boolean = ::writeStudioPng,
    private val videoWriter: suspend (Context, Uri, StudioVideoRequest) -> Boolean = ::writeStudioVideo,
    private val orphanCleaner: suspend (Context, Uri) -> Unit = ::deleteStudioUri,
) : ViewModel() {
    private sealed interface ExportState {
        data object Idle : ExportState

        data class Preparing(val preparation: StudioExportPreparation) : ExportState

        data class AwaitingPng(
            val preparation: StudioExportPreparation,
            val bitmap: Bitmap,
        ) : ExportState

        data class AwaitingVideo(
            val preparation: StudioExportPreparation,
            val request: StudioVideoRequest,
        ) : ExportState

        data class Exporting(
            val id: Long,
            val kind: StudioExportKind,
        ) : ExportState
    }

    private data class ActiveExport(val id: Long, val job: Job)

    private var exportState by mutableStateOf<ExportState>(ExportState.Idle)
    private var activeExport: ActiveExport? = null
    private var nextId = 0L

    val isBusy: Boolean
        get() = exportState !is ExportState.Idle

    internal val pendingKind: StudioExportKind?
        get() = when (exportState) {
            is ExportState.AwaitingPng -> StudioExportKind.PNG
            is ExportState.AwaitingVideo -> StudioExportKind.VIDEO
            else -> null
        }

    fun beginPreparation(kind: StudioExportKind): StudioExportPreparation? {
        if (exportState !is ExportState.Idle || activeExport?.job?.isActive == true) return null
        val preparation = StudioExportPreparation(++nextId, kind)
        exportState = ExportState.Preparing(preparation)
        return preparation
    }

    fun stagePng(preparation: StudioExportPreparation, bitmap: Bitmap): Boolean {
        if (!matchesPreparing(preparation, StudioExportKind.PNG)) return false
        exportState = ExportState.AwaitingPng(preparation, bitmap)
        return true
    }

    fun stageVideo(preparation: StudioExportPreparation, request: StudioVideoRequest): Boolean {
        if (!matchesPreparing(preparation, StudioExportKind.VIDEO)) return false
        exportState = ExportState.AwaitingVideo(preparation, request)
        return true
    }

    /** Cancels only this exact preparation, including a request already staged for the picker. */
    fun discard(preparation: StudioExportPreparation) {
        when (val current = exportState) {
            is ExportState.Preparing -> if (current.preparation == preparation) {
                exportState = ExportState.Idle
            }
            is ExportState.AwaitingPng -> if (current.preparation == preparation) {
                exportState = ExportState.Idle
                current.bitmap.recycleStudioBitmap()
            }
            is ExportState.AwaitingVideo -> if (current.preparation == preparation) {
                exportState = ExportState.Idle
                current.request.background.recycleStudioBitmap()
            }
            else -> Unit
        }
    }

    fun handlePngResult(context: Context, uri: Uri?, onNotice: (String) -> Unit) {
        val current = exportState
        if (current !is ExportState.AwaitingPng) {
            if (uri != null) cleanupOrphan(context, uri, onNotice)
            return
        }
        if (uri == null) {
            exportState = ExportState.Idle
            current.bitmap.recycleStudioBitmap()
            return
        }
        startExport(
            id = current.preparation.id,
            kind = StudioExportKind.PNG,
            context = context,
            uri = uri,
            resourceCleanup = current.bitmap::recycleStudioBitmap,
            onNotice = onNotice,
            successNotice = "Studio image saved.",
            failureNotice = "Studio image export failed.",
        ) {
            pngWriter(context, uri, current.bitmap)
        }
    }

    fun handleVideoResult(context: Context, uri: Uri?, onNotice: (String) -> Unit) {
        val current = exportState
        if (current !is ExportState.AwaitingVideo) {
            if (uri != null) cleanupOrphan(context, uri, onNotice)
            return
        }
        if (uri == null) {
            exportState = ExportState.Idle
            current.request.background.recycleStudioBitmap()
            return
        }
        startExport(
            id = current.preparation.id,
            kind = StudioExportKind.VIDEO,
            context = context,
            uri = uri,
            resourceCleanup = current.request.background::recycleStudioBitmap,
            onNotice = onNotice,
            successNotice = "Studio video saved.",
            failureNotice = "Studio video export failed.",
        ) {
            videoWriter(context, uri, current.request)
        }
    }

    /** Called when the user really leaves Studio, but not for Activity configuration recreation. */
    fun clearForRouteExit() {
        val current = exportState
        when (current) {
            is ExportState.Exporting -> {
                // Keep the exporting state until its finally block releases the resource;
                // a quick route re-entry must not overlap the cancelling codec/writer.
                activeExport?.job?.cancel()
            }
            is ExportState.AwaitingPng -> {
                exportState = ExportState.Idle
                current.bitmap.recycleStudioBitmap()
            }
            is ExportState.AwaitingVideo -> {
                exportState = ExportState.Idle
                current.request.background.recycleStudioBitmap()
            }
            else -> exportState = ExportState.Idle
        }
    }

    override fun onCleared() {
        clearForRouteExit()
        super.onCleared()
    }

    private fun matchesPreparing(
        preparation: StudioExportPreparation,
        kind: StudioExportKind,
    ): Boolean {
        val current = exportState as? ExportState.Preparing ?: return false
        return preparation.kind == kind && current.preparation == preparation
    }

    private fun startExport(
        id: Long,
        kind: StudioExportKind,
        context: Context,
        uri: Uri,
        resourceCleanup: () -> Unit,
        onNotice: (String) -> Unit,
        successNotice: String,
        failureNotice: String,
        writer: suspend () -> Boolean,
    ) {
        exportState = ExportState.Exporting(id, kind)
        val job = viewModelScope.launch(start = CoroutineStart.LAZY) {
            try {
                val saved = try {
                    writer()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    // Production writers already delete a failed document. This is also
                    // the defensive boundary for an unexpected provider/exporter exception.
                    deleteStudioUri(context, uri)
                    false
                }
                onNotice(if (saved) successNotice else failureNotice)
            } finally {
                resourceCleanup()
                if (activeExport?.id == id) activeExport = null
                val state = exportState
                if (state is ExportState.Exporting && state.id == id) {
                    exportState = ExportState.Idle
                }
            }
        }
        activeExport = ActiveExport(id, job)
        if (!job.start()) {
            activeExport = null
            exportState = ExportState.Idle
            resourceCleanup()
            cleanupOrphan(context, uri, onNotice)
        }
    }

    private fun cleanupOrphan(context: Context, uri: Uri, onNotice: (String) -> Unit) {
        viewModelScope.launch {
            runCatching { orphanCleaner(context, uri) }
            onNotice("Studio export was cancelled.")
        }
    }
}

@Composable
internal fun KitsuPetStudioRoute(
    presentation: PetPresentationState?,
    frame: ByteArray?,
    enabled: Boolean,
    onRefreshFrame: () -> Unit,
    onNotice: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val exportOwner = viewModel<StudioExportViewModel>()
    val activity = remember(context) { context.findActivity() }
    var background by remember { mutableStateOf<Bitmap?>(null) }
    val clip = remember { mutableStateListOf<StudioFrame>() }

    DisposableEffect(exportOwner, activity) {
        onDispose {
            background?.recycleStudioBitmap()
            if (activity?.isChangingConfigurations != true) {
                exportOwner.clearForRouteExit()
            }
        }
    }

    val choosePhoto = rememberLauncherForActivityResult(
        ActivityResultContracts.PickVisualMedia(),
    ) { uri ->
        if (uri != null) {
            scope.launch {
                var ownedDecoded: Bitmap? = null
                try {
                    withContext(Dispatchers.IO) {
                        ownedDecoded = decodeBoundedBitmap(context, uri)
                    }
                    val decoded = ownedDecoded ?: error("studio_photo_decode_failed")
                    ownedDecoded = null
                    background?.takeUnless(Bitmap::isRecycled)?.recycle()
                    background = decoded
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    onNotice("That photo could not be opened.")
                } finally {
                    ownedDecoded?.takeUnless(Bitmap::isRecycled)?.recycle()
                }
            }
        }
    }
    val takePhoto = rememberLauncherForActivityResult(
        ActivityResultContracts.TakePicturePreview(),
    ) { bitmap ->
        if (bitmap != null) {
            background?.takeUnless(Bitmap::isRecycled)?.recycle()
            background = bitmap.copy(Bitmap.Config.ARGB_8888, false)
            bitmap.recycle()
        }
    }
    val savePng = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("image/png"),
    ) { uri ->
        exportOwner.handlePngResult(context.applicationContext, uri, onNotice)
    }
    val saveVideo = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("video/mp4"),
    ) { uri ->
        exportOwner.handleVideoResult(context.applicationContext, uri, onNotice)
    }

    fun currentPetBitmap(): Bitmap? {
        val pack = presentation?.pack ?: return null
        val current = frame ?: return null
        if (!presentation.frame.available) return null
        return runCatching {
            PetFrameRenderer.decodeXbm(current, pack.width, pack.height)
        }.getOrNull()
    }

    KitsuPetStudioScreen(
        presentation = presentation,
        frame = frame,
        background = background,
        clipFrameCount = clip.size,
        enabled = enabled && !exportOwner.isBusy,
        onRefreshFrame = onRefreshFrame,
        onChoosePhoto = {
            choosePhoto.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
        },
        onTakePhoto = { takePhoto.launch(null) },
        onSaveTransparentPng = { placement ->
            val pet = currentPetBitmap()
            if (pet == null) {
                onNotice("Capture Kitsu's current frame first.")
            } else {
                val preparation = exportOwner.beginPreparation(StudioExportKind.PNG)
                if (preparation == null) {
                    pet.recycleStudioBitmap()
                    onNotice("A Studio export is already in progress.")
                    return@KitsuPetStudioScreen
                }
                scope.launch {
                    var prepared: Bitmap? = null
                    var pickerLaunched = false
                    try {
                        withContext(Dispatchers.Default) {
                            prepared = PetStudioPolicy.transparentCanvas(pet, placement)
                        }
                        val output = requireNotNull(prepared)
                        check(exportOwner.stagePng(preparation, output)) {
                            "studio_export_preparation_expired"
                        }
                        prepared = null // Ownership moved to exportOwner.
                        try {
                            savePng.launch(studioFileName("kitsu-pet", "png"))
                            pickerLaunched = true
                        } catch (error: Exception) {
                            exportOwner.discard(preparation)
                            throw error
                        }
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (_: Exception) {
                        onNotice("Studio image could not be prepared.")
                    } finally {
                        if (!pickerLaunched) exportOwner.discard(preparation)
                        prepared?.recycleStudioBitmap()
                        pet.recycleStudioBitmap()
                    }
                }
            }
        },
        onSaveCompositedPng = { placement ->
            val pet = currentPetBitmap()
            val photo = background
            if (pet == null || photo == null) {
                pet?.recycle()
                onNotice("Choose a photo and capture Kitsu's frame first.")
            } else {
                val photoSnapshot = runCatching {
                    photo.copy(Bitmap.Config.ARGB_8888, false)
                }.getOrNull()
                if (photoSnapshot == null) {
                    pet.recycle()
                    onNotice("Studio image could not be prepared.")
                } else {
                    val preparation = exportOwner.beginPreparation(StudioExportKind.PNG)
                    if (preparation == null) {
                        photoSnapshot.recycleStudioBitmap()
                        pet.recycleStudioBitmap()
                        onNotice("A Studio export is already in progress.")
                        return@KitsuPetStudioScreen
                    }
                    scope.launch {
                        var prepared: Bitmap? = null
                        var pickerLaunched = false
                        try {
                            withContext(Dispatchers.Default) {
                                prepared = PetStudioPolicy.squareComposite(
                                    photoSnapshot,
                                    pet,
                                    placement,
                                    size = 1_024,
                                )
                            }
                            val output = requireNotNull(prepared)
                            check(exportOwner.stagePng(preparation, output)) {
                                "studio_export_preparation_expired"
                            }
                            prepared = null // Ownership moved to exportOwner.
                            try {
                                savePng.launch(studioFileName("kitsu-photo", "png"))
                                pickerLaunched = true
                            } catch (error: Exception) {
                                exportOwner.discard(preparation)
                                throw error
                            }
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (_: Exception) {
                            onNotice("Studio image could not be prepared.")
                        } finally {
                            if (!pickerLaunched) exportOwner.discard(preparation)
                            prepared?.recycleStudioBitmap()
                            photoSnapshot.recycleStudioBitmap()
                            pet.recycleStudioBitmap()
                        }
                    }
                }
            }
        },
        onAppendClipFrame = {
            val state = presentation
            val bytes = frame
            val pack = state?.pack
            if (bytes == null || pack == null || !state.frame.available) {
                onNotice("Capture Kitsu's current frame first.")
            } else if (!PetStudioPolicy.canAppendFrame(clip.size)) {
                onNotice("This short clip is full.")
            } else if (clip.isNotEmpty() &&
                (clip.first().width != pack.width || clip.first().height != pack.height)
            ) {
                onNotice("Clear the clip before adding frames from a different pet format.")
            } else {
                clip += StudioFrame(bytes.copyOf(), pack.width, pack.height, state.frame.sha256)
                onNotice("Frame ${clip.size} added.")
            }
        },
        onClearClip = { clip.clear() },
        onExportClip = { placement ->
            val photo = background
            if (photo == null || clip.size < 2) {
                onNotice("Choose a photo and capture at least two frames first.")
            } else {
                val photoSnapshot = runCatching {
                    photo.copy(Bitmap.Config.ARGB_8888, false)
                }.getOrNull()
                if (photoSnapshot == null) {
                    onNotice("Studio video could not be prepared.")
                } else {
                    val preparation = exportOwner.beginPreparation(StudioExportKind.VIDEO)
                    if (preparation == null) {
                        photoSnapshot.recycleStudioBitmap()
                        onNotice("A Studio export is already in progress.")
                        return@KitsuPetStudioScreen
                    }
                    // Retain only tiny authenticated XBM bytes here. The exporter creates
                    // and recycles one 512px ARGB frame at a time after SAF confirms a file.
                    val request = StudioVideoRequest(
                        background = photoSnapshot,
                        frames = clip.toList(),
                        placement = placement,
                    )
                    if (!exportOwner.stageVideo(preparation, request)) {
                        request.background.recycleStudioBitmap()
                        onNotice("Studio video could not be prepared.")
                    } else try {
                        saveVideo.launch(studioFileName("kitsu-clip", "mp4"))
                    } catch (_: Exception) {
                        exportOwner.discard(preparation)
                        onNotice("Studio video could not be prepared.")
                    }
                }
            }
        },
        modifier = modifier,
    )
}

private fun studioFileName(prefix: String, extension: String): String =
    "$prefix-${System.currentTimeMillis()}.$extension".lowercase(Locale.ROOT)

private fun Bitmap.recycleStudioBitmap() {
    if (!isRecycled) recycle()
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

private suspend fun writeStudioPng(
    context: android.content.Context,
    uri: Uri,
    bitmap: Bitmap,
): Boolean = writeStudioDocument(context, uri) {
    context.contentResolver.openOutputStream(uri, "w")?.use { output ->
        PetFrameRenderer.writePng(bitmap, output)
    } ?: error("studio_destination_unavailable")
}

private suspend fun writeStudioVideo(
    context: android.content.Context,
    uri: Uri,
    request: StudioVideoRequest,
): Boolean = writeStudioDocument(context, uri) {
    context.contentResolver.openFileDescriptor(uri, "w")?.use { output ->
        PetClipExporter.writeGeneratedMp4(
            frameCount = request.frames.size,
            destination = output.fileDescriptor,
            width = PetStudioPolicy.VIDEO_OUTPUT_SIZE,
            height = PetStudioPolicy.VIDEO_OUTPUT_SIZE,
        ) { index ->
            val captured = request.frames[index]
            val pet = PetFrameRenderer.decodeXbm(
                captured.bytes,
                captured.width,
                captured.height,
            )
            try {
                PetStudioPolicy.squareComposite(
                    request.background,
                    pet,
                    request.placement,
                    size = PetStudioPolicy.VIDEO_OUTPUT_SIZE,
                )
            } finally {
                pet.recycle()
            }
        }
    } ?: error("studio_destination_unavailable")
}

private suspend fun writeStudioDocument(
    context: android.content.Context,
    uri: Uri,
    writer: suspend () -> Unit,
): Boolean = try {
    withContext(Dispatchers.IO) { writer() }
    true
} catch (cancelled: CancellationException) {
    deleteStudioUri(context, uri)
    throw cancelled
} catch (_: Exception) {
    deleteStudioUri(context, uri)
    false
}

private suspend fun deleteStudioUri(context: Context, uri: Uri) {
    withContext(NonCancellable + Dispatchers.IO) {
        val resolver = context.contentResolver
        val deletedAsDocument = runCatching {
            DocumentsContract.isDocumentUri(context, uri) &&
                DocumentsContract.deleteDocument(resolver, uri)
        }.getOrDefault(false)
        if (!deletedAsDocument) {
            runCatching { resolver.delete(uri, null, null) }
        }
    }
}

private fun decodeBoundedBitmap(context: android.content.Context, uri: Uri): Bitmap {
    val decoded = if (Build.VERSION.SDK_INT >= 28) {
        ImageDecoder.decodeBitmap(ImageDecoder.createSource(context.contentResolver, uri)) { decoder, info, _ ->
            decoder.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
            val longest = maxOf(info.size.width, info.size.height)
            if (longest > 1_920) {
                val scale = 1_920f / longest
                decoder.setTargetSize(
                    (info.size.width * scale).toInt().coerceAtLeast(1),
                    (info.size.height * scale).toInt().coerceAtLeast(1),
                )
            }
        }
    } else {
        @Suppress("DEPRECATION")
        decodeLegacyBoundedBitmap(context, uri)
    }
    return if (decoded.config == Bitmap.Config.ARGB_8888) decoded
    else decoded.copy(Bitmap.Config.ARGB_8888, false).also { decoded.recycle() }
}

@Suppress("DEPRECATION")
private fun decodeLegacyBoundedBitmap(context: android.content.Context, uri: Uri): Bitmap {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    context.contentResolver.openInputStream(uri)?.use { input ->
        BitmapFactory.decodeStream(input, null, bounds)
    } ?: error("studio_photo_decode_failed")
    require(bounds.outWidth > 0 && bounds.outHeight > 0) { "studio_photo_decode_failed" }
    var sampleSize = 1
    while (maxOf(bounds.outWidth, bounds.outHeight) / sampleSize > 1_920) {
        sampleSize *= 2
    }
    val decoded = context.contentResolver.openInputStream(uri)?.use { input ->
        BitmapFactory.decodeStream(
            input,
            null,
            BitmapFactory.Options().apply {
                inSampleSize = sampleSize
                inPreferredConfig = Bitmap.Config.ARGB_8888
            },
        )
    } ?: error("studio_photo_decode_failed")
    val orientation = context.contentResolver.openInputStream(uri)?.use { input ->
        runCatching {
            ExifInterface(input).getAttributeInt(
                ExifInterface.TAG_ORIENTATION,
                ExifInterface.ORIENTATION_NORMAL,
            )
        }.getOrDefault(ExifInterface.ORIENTATION_NORMAL)
    } ?: ExifInterface.ORIENTATION_NORMAL
    val matrix = Matrix().apply {
        when (orientation) {
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> setScale(-1f, 1f)
            ExifInterface.ORIENTATION_ROTATE_180 -> setRotate(180f)
            ExifInterface.ORIENTATION_FLIP_VERTICAL -> setScale(1f, -1f)
            ExifInterface.ORIENTATION_TRANSPOSE -> {
                setRotate(90f)
                postScale(-1f, 1f)
            }
            ExifInterface.ORIENTATION_ROTATE_90 -> setRotate(90f)
            ExifInterface.ORIENTATION_TRANSVERSE -> {
                setRotate(-90f)
                postScale(-1f, 1f)
            }
            ExifInterface.ORIENTATION_ROTATE_270 -> setRotate(-90f)
        }
    }
    if (matrix.isIdentity) return decoded
    return Bitmap.createBitmap(
        decoded,
        0,
        0,
        decoded.width,
        decoded.height,
        matrix,
        true,
    ).also { transformed ->
        if (transformed !== decoded) decoded.recycle()
    }
}
