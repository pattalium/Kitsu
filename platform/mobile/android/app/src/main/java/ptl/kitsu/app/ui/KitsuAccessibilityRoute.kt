package ptl.kitsu.app.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import ptl.kitsu.app.accessibility.PetAccessibilityPreferenceStore
import ptl.kitsu.app.accessibility.PetAccessibilityMoment
import ptl.kitsu.app.accessibility.PetAccessibilityPreferences
import ptl.kitsu.app.accessibility.PetAccessibilityTransitionPolicy
import ptl.kitsu.app.accessibility.PetHapticCue
import ptl.kitsu.app.accessibility.PetHaptics
import ptl.kitsu.app.accessibility.PetSpeechController
import ptl.kitsu.app.accessibility.PetSpeechCue
import ptl.kitsu.app.accessibility.PetSpeechKind
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.KitsuStatus
import ptl.kitsu.app.model.PetPresentationState
import ptl.kitsu.app.model.WalkAdventureState

@Composable
internal fun KitsuAccessibilityRoute(
    status: KitsuStatus?,
    profile: CompanionProfile?,
    focus: FocusSessionState?,
    walk: WalkAdventureState?,
    presentation: PetPresentationState?,
    frame: ByteArray?,
    enabled: Boolean,
    captureEnabled: Boolean,
    onRefresh: () -> Unit,
    onPet: () -> Unit,
    onFeed: () -> Unit,
    onPlay: () -> Unit,
    onNotice: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val preferenceStore = remember(context) { PetAccessibilityPreferenceStore(context) }
    val speech = remember(context) { PetSpeechController(context) }
    val haptics = remember(context) { PetHaptics(context) }
    var preferences by remember {
        mutableStateOf(runCatching(preferenceStore::read).getOrDefault(PetAccessibilityPreferences()))
    }
    val currentMoment = PetAccessibilityMoment(
        requestState = profile?.checkIn?.request?.state,
        focusPhase = focus?.phase,
        walkPhase = walk?.phase,
    )
    var previousMoment by remember { mutableStateOf(currentMoment) }
    DisposableEffect(speech) { onDispose(speech::close) }
    LaunchedEffect(currentMoment) {
        if (preferences.hapticsEnabled) {
            PetAccessibilityTransitionPolicy.hapticCue(previousMoment, currentMoment)
                ?.let(haptics::play)
        }
        previousMoment = currentMoment
    }

    fun updatePreferences(value: PetAccessibilityPreferences) {
        runCatching { preferenceStore.write(value) }
            .onSuccess { preferences = value }
            .onFailure { onNotice("Accessibility preferences could not be saved.") }
    }

    KitsuAccessibilityMirror(
        status = status,
        profile = profile,
        presentation = presentation,
        frame = frame,
        enabled = enabled,
        captureEnabled = captureEnabled,
        onRefresh = onRefresh,
        onPet = onPet,
        onFeed = onFeed,
        onPlay = onPlay,
        preferences = preferences,
        onPreferencesChange = ::updatePreferences,
        onSpeakState = {
            val spoken = speech.speak(
                PetSpeechCue(
                    PetSpeechKind.STATE,
                    PetStateNarrator.describe(status, profile, presentation),
                ),
            )
            if (!spoken) onNotice("Android text-to-speech is not ready yet.")
        },
        onTestHaptic = {
            if (!haptics.play(PetHapticCue.CHECK_IN)) {
                onNotice("This phone has no available vibration cue.")
            }
        },
        modifier = modifier,
    )
}
