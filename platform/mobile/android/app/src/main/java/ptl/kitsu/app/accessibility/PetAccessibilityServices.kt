package ptl.kitsu.app.accessibility

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.speech.tts.TextToSpeech
import java.util.Locale
import ptl.kitsu.app.model.CompanionRequestState
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.WalkPhase

enum class PetSpeechKind {
    STATE,
    CHECK_IN,
    FOCUS,
    WALK,
}

data class PetSpeechCue(
    val kind: PetSpeechKind,
    val text: String,
)

object PetSpeechPolicy {
    const val MAX_UTF8_BYTES = 512

    fun prepare(cue: PetSpeechCue): String? {
        val cleaned = cue.text
            .mapNotNull { character ->
                when {
                    character.isWhitespace() -> ' '
                    character.code >= 0x20 -> character
                    else -> null
                }
            }
            .joinToString("")
            .replace(Regex("\\s+"), " ")
            .trim()
        if (cleaned.isEmpty()) return null
        val output = StringBuilder()
        var bytes = 0
        for (codePoint in cleaned.codePoints().toArray()) {
            val value = String(Character.toChars(codePoint))
            val next = value.toByteArray(Charsets.UTF_8).size
            if (bytes + next > MAX_UTF8_BYTES) break
            output.append(value)
            bytes += next
        }
        return output.toString().trim().takeIf(String::isNotEmpty)
    }
}

class PetSpeechController(context: Context) : AutoCloseable {
    private var ready = false
    private var closed = false
    private val engine = TextToSpeech(context.applicationContext) { status ->
        ready = status == TextToSpeech.SUCCESS
        if (ready) engineLanguage()
    }

    fun speak(cue: PetSpeechCue): Boolean {
        val text = PetSpeechPolicy.prepare(cue) ?: return false
        if (!ready || closed) return false
        return engine.speak(
            text,
            TextToSpeech.QUEUE_FLUSH,
            null,
            "kitsu-${cue.kind.name.lowercase()}",
        ) == TextToSpeech.SUCCESS
    }

    fun stop() {
        if (!closed) engine.stop()
    }

    override fun close() {
        if (closed) return
        closed = true
        engine.stop()
        engine.shutdown()
    }

    private fun engineLanguage() {
        val result = engine.setLanguage(Locale.getDefault())
        if (result == TextToSpeech.LANG_MISSING_DATA ||
            result == TextToSpeech.LANG_NOT_SUPPORTED
        ) {
            ready = engine.setLanguage(Locale.US) >= TextToSpeech.LANG_AVAILABLE
        }
    }
}

enum class PetHapticCue {
    CHECK_IN,
    FOCUS_COMPLETE,
    WALK_RETURNED,
}

data class PetAccessibilityMoment(
    val requestState: CompanionRequestState? = null,
    val focusPhase: FocusPhase? = null,
    val walkPhase: WalkPhase? = null,
)

object PetAccessibilityTransitionPolicy {
    fun hapticCue(
        previous: PetAccessibilityMoment,
        current: PetAccessibilityMoment,
    ): PetHapticCue? = when {
        previous.walkPhase != WalkPhase.RETURNED && current.walkPhase == WalkPhase.RETURNED ->
            PetHapticCue.WALK_RETURNED
        previous.focusPhase != FocusPhase.COMPLETED && current.focusPhase == FocusPhase.COMPLETED ->
            PetHapticCue.FOCUS_COMPLETE
        previous.requestState != CompanionRequestState.PENDING &&
            current.requestState == CompanionRequestState.PENDING -> PetHapticCue.CHECK_IN
        else -> null
    }
}

class PetHaptics(context: Context) {
    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
        context.getSystemService(VibratorManager::class.java)?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    }

    fun play(cue: PetHapticCue): Boolean {
        val target = vibrator?.takeIf(Vibrator::hasVibrator) ?: return false
        val timings = when (cue) {
            PetHapticCue.CHECK_IN -> longArrayOf(0, 35)
            PetHapticCue.FOCUS_COMPLETE -> longArrayOf(0, 45, 70, 45)
            PetHapticCue.WALK_RETURNED -> longArrayOf(0, 30, 55, 30, 55, 55)
        }
        target.vibrate(VibrationEffect.createWaveform(timings, -1))
        return true
    }
}

data class PetAccessibilityPreferences(
    val speechEnabled: Boolean = false,
    val hapticsEnabled: Boolean = false,
    val highContrast: Boolean = false,
    val reducedMotion: Boolean = false,
)

class PetAccessibilityPreferenceStore(context: Context) {
    private val values = context.getSharedPreferences("pet-accessibility-v1", Context.MODE_PRIVATE)

    fun read(): PetAccessibilityPreferences = PetAccessibilityPreferences(
        speechEnabled = values.getBoolean("speech", false),
        hapticsEnabled = values.getBoolean("haptics", false),
        highContrast = values.getBoolean("contrast", false),
        reducedMotion = values.getBoolean("reduced_motion", false),
    )

    fun write(value: PetAccessibilityPreferences) {
        check(
            values.edit()
                .putBoolean("speech", value.speechEnabled)
                .putBoolean("haptics", value.hapticsEnabled)
                .putBoolean("contrast", value.highContrast)
                .putBoolean("reduced_motion", value.reducedMotion)
                .commit(),
        ) { "pet_accessibility_preferences_write_failed" }
    }
}
