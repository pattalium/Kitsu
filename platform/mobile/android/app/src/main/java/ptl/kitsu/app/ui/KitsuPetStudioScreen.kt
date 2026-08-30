package ptl.kitsu.app.ui

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.media.PetFrameRenderer
import ptl.kitsu.app.media.PetStudioPlacement
import ptl.kitsu.app.media.PetStudioPolicy
import ptl.kitsu.app.model.PetPresentationState

@Composable
internal fun KitsuPetStudioScreen(
    presentation: PetPresentationState?,
    frame: ByteArray?,
    background: Bitmap?,
    clipFrameCount: Int,
    enabled: Boolean,
    onRefreshFrame: () -> Unit,
    onChoosePhoto: () -> Unit,
    onTakePhoto: () -> Unit,
    onSaveTransparentPng: (PetStudioPlacement) -> Unit,
    onSaveCompositedPng: (PetStudioPlacement) -> Unit,
    onAppendClipFrame: () -> Unit,
    onClearClip: () -> Unit,
    onExportClip: (PetStudioPlacement) -> Unit,
    modifier: Modifier = Modifier,
) {
    var horizontal by rememberSaveable { mutableFloatStateOf(0.5f) }
    var vertical by rememberSaveable { mutableFloatStateOf(0.82f) }
    var scale by rememberSaveable { mutableFloatStateOf(4f) }
    val placement = PetStudioPlacement(horizontal, vertical, scale)
    val pet = remember(presentation?.frame?.sha256, frame) {
        val pack = presentation?.pack
        if (frame == null || pack == null || !presentation.frame.available) null
        else runCatching { PetFrameRenderer.decodeXbm(frame, pack.width, pack.height) }.getOrNull()
    }

    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("screen-pet-studio"),
        contentPadding = PaddingValues(18.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SectionHeading(
                title = "Pet Studio",
                supporting = "Use the exact animation frame currently shown by your Kitsu.",
            )
        }
        item {
            KitsuCard {
                BoxWithConstraints(
                    modifier = Modifier
                        .fillMaxWidth()
                        .aspectRatio(1f)
                        .background(MaterialTheme.colorScheme.surfaceVariant),
                    contentAlignment = Alignment.Center,
                ) {
                    if (background != null) {
                        Image(
                            bitmap = background.asImageBitmap(),
                            contentDescription = "Selected studio background",
                            modifier = Modifier.fillMaxSize(),
                            contentScale = ContentScale.Crop,
                        )
                    }
                    if (pet != null) {
                        val layout = PetStudioPolicy.layout(pet.width, pet.height, placement)
                        val petWidth = maxWidth * layout.widthFraction
                        val petHeight = maxHeight * layout.heightFraction
                        Image(
                            bitmap = pet.asImageBitmap(),
                            contentDescription = "Current Kitsu animation frame",
                            modifier = Modifier
                                .align(Alignment.TopStart)
                                .offset(
                                    x = maxWidth * layout.leftFraction,
                                    y = maxHeight * layout.topFraction,
                                )
                                .size(petWidth, petHeight),
                        )
                    } else {
                        Text(
                            "Refresh to capture Kitsu's current frame",
                            modifier = Modifier.padding(24.dp),
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
        item {
            KitsuCard(title = "Place Kitsu") {
                Text("Size ${"%.1f".format(scale)}×")
                Slider(
                    value = scale,
                    onValueChange = { scale = it },
                    valueRange = PetStudioPolicy.MIN_SCALE..PetStudioPolicy.MAX_SCALE,
                    enabled = pet != null,
                )
                Text("Left / right")
                Slider(
                    value = horizontal,
                    onValueChange = { horizontal = it },
                    valueRange = 0f..1f,
                    enabled = pet != null,
                )
                Text("Up / down")
                Slider(
                    value = vertical,
                    onValueChange = { vertical = it },
                    valueRange = 0f..1f,
                    enabled = pet != null,
                )
            }
        }
        item {
            KitsuCard(title = "Background") {
                Text(
                    "Choose a photo, or take one with your camera. Photos stay on this phone unless you share them.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    OutlinedButton(onClick = onChoosePhoto, enabled = enabled) {
                        Text("Choose photo")
                    }
                    OutlinedButton(onClick = onTakePhoto, enabled = enabled) {
                        Text("Take photo")
                    }
                }
            }
        }
        item {
            KitsuCard(title = "Save") {
                Button(
                    onClick = { onSaveTransparentPng(placement) },
                    enabled = enabled && pet != null,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                ) { Text("Save transparent pet PNG") }
                Button(
                    onClick = { onSaveCompositedPng(placement) },
                    enabled = enabled && pet != null && background != null,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                ) { Text("Save photo with Kitsu") }
            }
        }
        item {
            KitsuCard(title = "Short animation") {
                Text(
                    "Capture a few different live frames, then export a local ${PetStudioPolicy.DEFAULT_CLIP_FPS} fps clip.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text("$clipFrameCount / ${PetStudioPolicy.MAX_CLIP_FRAMES} frames")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onRefreshFrame, enabled = enabled) { Text("Refresh frame") }
                    OutlinedButton(
                        onClick = onAppendClipFrame,
                        enabled = enabled && pet != null && PetStudioPolicy.canAppendFrame(clipFrameCount),
                    ) { Text("Add frame") }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onClearClip, enabled = clipFrameCount > 0) {
                        Text("Clear")
                    }
                    Button(
                        onClick = { onExportClip(placement) },
                        enabled = enabled && background != null && clipFrameCount >= 2,
                    ) {
                        Text("Export video")
                    }
                }
            }
        }
        item {
            OutlinedButton(
                onClick = onRefreshFrame,
                enabled = enabled,
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
            ) { Text("Capture Kitsu's current frame") }
        }
    }
}
