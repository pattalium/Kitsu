package ptl.kitsu.app.ui

import androidx.compose.foundation.Image
import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.media.PetFrameRenderer
import ptl.kitsu.app.accessibility.PetAccessibilityPreferences
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.PetPresentationState

internal object PetStateNarrator {
    fun describe(
        status: KitsuStatus?,
        profile: CompanionProfile?,
        presentation: PetPresentationState?,
    ): String {
        val name = profile?.nickname?.takeIf(String::isNotBlank)
            ?: status?.companionName?.takeIf(String::isNotBlank)
            ?: "Kitsu"
        val mood = profile?.mood?.name?.lowercase()?.replace('_', ' ')
            ?: status?.mood?.lowercase()?.replace('_', ' ')
            ?: "unknown mood"
        val activity = presentation?.animation?.resolvedRole?.name
            ?.takeUnless { it == "UNKNOWN" }
            ?.lowercase()
            ?.replace('_', ' ')
        val needs = status?.needs?.let {
            "Energy ${it.energy} percent, curiosity ${it.curiosity} percent, affection ${it.affection} percent."
        }.orEmpty()
        return buildString {
            append(name)
            append(" feels ")
            append(mood)
            activity?.let { append(" and is ").append(it) }
            append(". ")
            append(needs)
        }.trim()
    }
}

@Composable
internal fun KitsuAccessibilityMirror(
    status: KitsuStatus?,
    profile: CompanionProfile?,
    presentation: PetPresentationState?,
    frame: ByteArray?,
    enabled: Boolean,
    captureEnabled: Boolean,
    onRefresh: () -> Unit,
    onPet: () -> Unit,
    onFeed: () -> Unit,
    onPlay: () -> Unit,
    preferences: PetAccessibilityPreferences = PetAccessibilityPreferences(),
    onPreferencesChange: (PetAccessibilityPreferences) -> Unit = {},
    onSpeakState: () -> Unit = {},
    onTestHaptic: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val narration = PetStateNarrator.describe(status, profile, presentation)
    val mirrorBackground = if (preferences.highContrast) Color.Black else Color.Transparent
    val mirrorForeground = if (preferences.highContrast) Color.White else Color.Unspecified
    val bitmap = remember(presentation?.frame?.sha256, frame) {
        val pack = presentation?.pack
        if (frame == null || pack == null || !presentation.frame.available) null
        else runCatching {
            PetFrameRenderer.decodeXbm(frame, pack.width, pack.height)
        }.getOrNull()
    }
    LazyColumn(
        modifier = modifier
            .fillMaxWidth()
            .semantics {
                liveRegion = LiveRegionMode.Polite
                stateDescription = narration
            },
        contentPadding = PaddingValues(bottom = 18.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SectionHeading(
                title = "Accessibility companion",
                supporting = "A large, readable mirror of Kitsu's current state.",
            )
        }
        item {
            KitsuCard {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(mirrorBackground)
                        .then(if (preferences.reducedMotion) Modifier else Modifier.animateContentSize())
                        .padding(12.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(14.dp),
                ) {
                    if (bitmap != null) {
                        Image(
                            bitmap = bitmap.asImageBitmap(),
                            contentDescription = narration,
                            modifier = Modifier
                                .size(256.dp)
                                .background(Color.Black)
                                .padding(8.dp),
                        )
                    } else {
                        Text(
                            "Live pet picture unavailable",
                            modifier = Modifier.heightIn(min = 72.dp),
                            style = MaterialTheme.typography.titleLarge,
                            color = mirrorForeground,
                        )
                    }
                    Text(
                        profile?.nickname?.takeIf(String::isNotBlank)
                            ?: status?.companionName ?: "Kitsu",
                        style = MaterialTheme.typography.headlineMedium,
                        fontWeight = FontWeight.Bold,
                        color = mirrorForeground,
                    )
                    Text(
                        narration,
                        style = MaterialTheme.typography.titleLarge,
                        color = mirrorForeground,
                    )
                    status?.batteryPercent?.let {
                        Text(
                            "Battery $it%",
                            style = MaterialTheme.typography.titleMedium,
                            color = mirrorForeground,
                        )
                    }
                }
            }
        }
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Button(
                    onClick = onPet,
                    enabled = enabled,
                    modifier = Modifier.weight(1f).heightIn(min = 56.dp),
                ) { Text("Pet") }
                Button(
                    onClick = onFeed,
                    enabled = enabled,
                    modifier = Modifier.weight(1f).heightIn(min = 56.dp),
                ) { Text("Feed") }
                Button(
                    onClick = onPlay,
                    enabled = enabled,
                    modifier = Modifier.weight(1f).heightIn(min = 56.dp),
                ) { Text("Play") }
            }
        }
        item {
            OutlinedButton(
                onClick = onRefresh,
                enabled = captureEnabled,
                modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp),
            ) { Text("Refresh mirror") }
        }
        item {
            KitsuCard(title = "Reading & cues") {
                AccessibilityPreferenceRow(
                    title = "Speak Kitsu's state",
                    supporting = "Only speaks when you press Read state; mesh messages are never read aloud.",
                    checked = preferences.speechEnabled,
                    onCheckedChange = {
                        onPreferencesChange(preferences.copy(speechEnabled = it))
                    },
                )
                AccessibilityPreferenceRow(
                    title = "Haptic cues",
                    supporting = "Use short local vibration cues for companion moments.",
                    checked = preferences.hapticsEnabled,
                    onCheckedChange = {
                        onPreferencesChange(preferences.copy(hapticsEnabled = it))
                    },
                )
                AccessibilityPreferenceRow(
                    title = "High contrast",
                    supporting = "Keep the pet mirror on a solid high-contrast background.",
                    checked = preferences.highContrast,
                    onCheckedChange = {
                        onPreferencesChange(preferences.copy(highContrast = it))
                    },
                )
                AccessibilityPreferenceRow(
                    title = "Reduced motion",
                    supporting = "Prefer still authenticated snapshots over animated transitions.",
                    checked = preferences.reducedMotion,
                    onCheckedChange = {
                        onPreferencesChange(preferences.copy(reducedMotion = it))
                    },
                )
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    OutlinedButton(
                        onClick = onSpeakState,
                        enabled = preferences.speechEnabled,
                    ) { Text("Read state") }
                    OutlinedButton(
                        onClick = onTestHaptic,
                        enabled = preferences.hapticsEnabled,
                    ) { Text("Test vibration") }
                }
            }
        }
    }
}

@Composable
private fun AccessibilityPreferenceRow(
    title: String,
    supporting: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(supporting, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}
