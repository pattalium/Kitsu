package ptl.kitsu.app.ui

import android.os.SystemClock
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.model.CompanionAction
import ptl.kitsu.app.model.CompanionProfile
import ptl.kitsu.app.model.CompanionQuestionKind
import ptl.kitsu.app.model.CompanionRequestState
import ptl.kitsu.app.model.FocusPhase
import ptl.kitsu.app.model.FocusSessionState
import ptl.kitsu.app.model.PetFeaturePolicy
import ptl.kitsu.app.model.WalkAdventureState
import ptl.kitsu.app.model.WalkDecision
import ptl.kitsu.app.model.WalkObjective
import ptl.kitsu.app.model.WalkPhase
import ptl.kitsu.app.model.WalkRisk
import ptl.kitsu.app.model.WalkStartCommand
import ptl.kitsu.app.model.WalkTerrain
import ptl.kitsu.app.model.WalkWeather
import ptl.kitsu.app.repository.OwnerState
import ptl.kitsu.app.walk.WalkStepAvailability
import ptl.kitsu.app.walk.WalkStepSnapshot
import kotlinx.coroutines.delay
import kotlin.math.max

internal data class FocusTimelinePresentation(
    val phase: FocusPhase,
    val phaseElapsedMs: Long,
    val phaseRemainingMs: Long,
    val phaseDurationMs: Long,
    val sessionElapsedMs: Long,
    val sessionDurationMs: Long,
    val phaseProgress: Float,
    val sessionProgress: Float,
)

internal object CompanionFeaturePresentationPolicy {
    fun actionLabel(action: CompanionAction): String = when (action) {
        CompanionAction.PET -> "Pet"
        CompanionAction.FEED -> "Feed"
        CompanionAction.PLAY -> "Play"
        CompanionAction.LISTEN -> "Listen nearby"
        CompanionAction.SLEEP -> "Rest"
        CompanionAction.WAKE -> "Wake up"
        CompanionAction.MEET -> "Meet someone"
        CompanionAction.GIFT -> "Spend time together"
    }

    fun questionTitle(kind: CompanionQuestionKind): String = when (kind) {
        CompanionQuestionKind.QUIET_OR_PLAY -> "What kind of day should we have?"
        CompanionQuestionKind.DAWN_OR_NIGHT -> "When should we spend time together?"
        CompanionQuestionKind.HOME_OR_EXPLORE -> "Stay cozy or explore?"
    }

    fun focusStatus(state: FocusSessionState, elapsedSinceSnapshotMs: Long = 0L): String {
        val timeline = focusTimeline(state, elapsedSinceSnapshotMs)
        return when (timeline.phase) {
            FocusPhase.IDLE -> "Ready when you are"
            FocusPhase.FOCUS -> "Focus phase · ${formatDuration(timeline.phaseRemainingMs)} left"
            FocusPhase.BREAK -> "Break phase · ${formatDuration(timeline.phaseRemainingMs)} left"
            FocusPhase.COMPLETED -> state.prompt.title.ifBlank { "Session complete" }
        }
    }

    fun focusTimeline(
        state: FocusSessionState,
        elapsedSinceSnapshotMs: Long = 0L,
    ): FocusTimelinePresentation {
        val focusDurationMs = state.focusMinutes.toLong().coerceAtLeast(0L) * 60_000L
        val breakDurationMs = state.breakMinutes.toLong().coerceAtLeast(0L) * 60_000L
        val sessionDurationMs = focusDurationMs + breakDurationMs
        val active = state.phase == FocusPhase.FOCUS || state.phase == FocusPhase.BREAK
        val phaseCeiling = when (state.phase) {
            FocusPhase.FOCUS -> focusDurationMs
            FocusPhase.BREAK, FocusPhase.COMPLETED -> sessionDurationMs
            FocusPhase.IDLE -> state.elapsedMs.coerceAtLeast(0L)
        }
        val localElapsedMs = if (active) elapsedSinceSnapshotMs.coerceAtLeast(0L) else 0L
        val sessionElapsedMs = (state.elapsedMs.coerceAtLeast(0L) + localElapsedMs)
            .coerceAtMost(phaseCeiling)
        val phaseDurationMs = when (state.phase) {
            FocusPhase.FOCUS -> focusDurationMs
            FocusPhase.BREAK -> breakDurationMs
            FocusPhase.IDLE, FocusPhase.COMPLETED -> 0L
        }
        val phaseElapsedMs = when (state.phase) {
            FocusPhase.FOCUS -> sessionElapsedMs.coerceAtMost(focusDurationMs)
            FocusPhase.BREAK -> (sessionElapsedMs - focusDurationMs).coerceIn(0L, breakDurationMs)
            FocusPhase.IDLE, FocusPhase.COMPLETED -> 0L
        }
        val phaseRemainingMs = (phaseDurationMs - phaseElapsedMs).coerceAtLeast(0L)
        return FocusTimelinePresentation(
            phase = state.phase,
            phaseElapsedMs = phaseElapsedMs,
            phaseRemainingMs = phaseRemainingMs,
            phaseDurationMs = phaseDurationMs,
            sessionElapsedMs = sessionElapsedMs,
            sessionDurationMs = sessionDurationMs,
            phaseProgress = ratio(phaseElapsedMs, phaseDurationMs),
            sessionProgress = ratio(sessionElapsedMs, sessionDurationMs),
        )
    }

    fun formatDuration(milliseconds: Long): String {
        val totalSeconds = max(0L, milliseconds) / 1_000L
        return "%d:%02d".format(totalSeconds / 60L, totalSeconds % 60L)
    }

    fun walkStatus(state: WalkAdventureState): String = when (state.phase) {
        WalkPhase.IDLE -> "Choose a route and take Kitsu with you."
        WalkPhase.ACTIVE -> "${state.steps} of ${state.targetSteps} steps · ${state.progressPercent}%"
        WalkPhase.AWAITING_RESCUE -> "Kitsu needs your decision before continuing."
        WalkPhase.RETURNED -> state.postcard?.title ?: "Back home"
    }

    private fun ratio(part: Long, total: Long): Float = if (total <= 0L) 0f else {
        (part.toFloat() / total.toFloat()).coerceIn(0f, 1f)
    }
}

internal object WalkStepPresentationPolicy {
    fun status(
        snapshot: WalkStepSnapshot,
        walk: WalkAdventureState,
        selectedDeviceAddress: String?,
    ): String = when (
        snapshot.availability
    ) {
        WalkStepAvailability.PERMISSION_REQUIRED ->
            "Allow Physical activity to count phone steps during a Kitsu walk."
        WalkStepAvailability.SENSOR_UNAVAILABLE ->
            "This phone has no compatible step counter. You can still start a walk without automatic steps."
        WalkStepAvailability.REGISTRATION_FAILED ->
            "The phone step sensor could not start. Retry it, or continue without automatic steps."
        WalkStepAvailability.CLOSED ->
            "Phone step tracking is closed until the app is reopened."
        WalkStepAvailability.AVAILABLE -> when {
            !snapshot.observing -> "Phone step tracking is paused."
            !snapshot.sensorBaselineReady -> if (walk.hasActiveRoute()) {
                "Calibrating phone steps. Keep this phone with you; counting starts after its first sensor update."
            } else {
                "Calibrating phone steps. Take a few steps with this phone before starting so the walk begins from a clean baseline."
            }
            walk.hasActiveRoute() && selectedDeviceAddress != null &&
                snapshot.matches(selectedDeviceAddress, walk.routeId) ->
                "Phone walk total: ${snapshot.stepsTotal} steps · Kitsu total: ${walk.steps} steps"
            walk.hasActiveRoute() -> "Linking phone steps to this Kitsu walk…"
            else -> "Phone steps are calibrated. Automatic counting starts only after you start a Kitsu walk."
        }
    }

    fun requestsPermissionInsteadOfStarting(snapshot: WalkStepSnapshot): Boolean =
        snapshot.availability == WalkStepAvailability.PERMISSION_REQUIRED

    fun requestsStepSetupInsteadOfStarting(snapshot: WalkStepSnapshot): Boolean =
        requestsPermissionInsteadOfStarting(snapshot) ||
            snapshot.availability == WalkStepAvailability.REGISTRATION_FAILED ||
            (snapshot.availability == WalkStepAvailability.AVAILABLE && !snapshot.observing)

    fun startLabel(snapshot: WalkStepSnapshot): String = when (snapshot.availability) {
        WalkStepAvailability.PERMISSION_REQUIRED -> "Allow steps to start"
        WalkStepAvailability.REGISTRATION_FAILED -> "Retry phone steps"
        WalkStepAvailability.AVAILABLE -> if (snapshot.observing) {
            "Start tracked walk"
        } else {
            "Enable phone steps"
        }
        WalkStepAvailability.SENSOR_UNAVAILABLE,
        WalkStepAvailability.CLOSED,
        -> "Start without phone steps"
    }

    private fun WalkAdventureState.hasActiveRoute(): Boolean =
        phase == WalkPhase.ACTIVE || phase == WalkPhase.AWAITING_RESCUE
}

@Composable
internal fun KitsuCompanionScreen(
    owner: OwnerState,
    walkSteps: WalkStepSnapshot,
    canManageProfile: Boolean,
    canOpenGuide: Boolean,
    updateBusy: Boolean,
    onRefresh: () -> Unit,
    onSetNickname: (String) -> Unit,
    onAnswerRequest: (Boolean) -> Unit,
    onAnswerQuestion: (Int) -> Unit,
    onStartFocus: (Int) -> Unit,
    onStopFocus: () -> Unit,
    onCancelFocus: () -> Unit,
    onAcknowledgeFocus: () -> Unit,
    onStartWalk: (WalkStartCommand) -> Unit,
    onSyncWalk: () -> Unit,
    onDecideWalk: (WalkDecision) -> Unit,
    onFinishWalk: () -> Unit,
    onAcknowledgeWalk: () -> Unit,
    onRequestWalkPermission: () -> Unit,
    onOpenGuide: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val enabled = owner.connection.connected && !updateBusy
    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("screen-companion"),
        contentPadding = PaddingValues(18.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            SectionHeading(
                title = "Your companion",
                supporting = "Personality, daily conversations, focus sessions, and walks live on your Kitsu.",
            )
        }

        if (!owner.companionProfileSupported && owner.status != null) {
            item {
                StatePanel(
                    title = "Companion features need newer firmware",
                    message = "Your current Kitsu remains fully usable. Update when the compatible firmware is available.",
                    testTag = "companion-unsupported",
                )
            }
        } else {
            owner.companionProfile?.let { profile ->
                item {
                    CompanionProfileCard(
                        profile = profile,
                        enabled = enabled && !owner.companionProfileMutationInFlight,
                        canRename = canManageProfile,
                        onSetNickname = onSetNickname,
                    )
                }
                item {
                    CompanionCheckInCard(
                        profile = profile,
                        enabled = enabled && !owner.companionProfileMutationInFlight,
                        canAnswer = canManageProfile,
                        onAnswerRequest = onAnswerRequest,
                        onAnswerQuestion = onAnswerQuestion,
                    )
                }
            }
        }

        if (owner.focusSupported) {
            owner.focusState?.let { state ->
                item {
                    FocusCompanionCard(
                        state = state,
                        enabled = enabled && !owner.focusMutationInFlight,
                        onStart = onStartFocus,
                        onStop = onStopFocus,
                        onCancel = onCancelFocus,
                        onAcknowledge = onAcknowledgeFocus,
                    )
                }
            }
        }

        if (owner.walkSupported) {
            owner.walkState?.let { state ->
                item {
                    WalkCompanionCard(
                        state = state,
                        selectedDeviceAddress = owner.activeDeviceAddress,
                        enabled = enabled && !owner.walkMutationInFlight,
                        onStart = onStartWalk,
                        onSync = onSyncWalk,
                        onDecide = onDecideWalk,
                        onFinish = onFinishWalk,
                        onAcknowledge = onAcknowledgeWalk,
                        walkSteps = walkSteps,
                        onRequestWalkPermission = onRequestWalkPermission,
                    )
                }
            }
        }

        if (canOpenGuide) {
            item {
                KitsuCard(title = "Creature guide") {
                    Text(
                        "See all 21 creatures and track which ones you have seen or own.",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    OutlinedButton(
                        onClick = onOpenGuide,
                        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    ) { Text("Open creature guide") }
                }
            }
        }

        owner.companionProfileErrorCode?.let { code ->
            item { FeatureErrorCard("Companion", code, enabled, onRefresh) }
        }
        owner.focusErrorCode?.let { code ->
            item { FeatureErrorCard("Focus", code, enabled, onRefresh) }
        }
        owner.walkErrorCode?.let { code ->
            item { FeatureErrorCard("Walk", code, enabled, onRefresh) }
        }
    }
}

@Composable
private fun CompanionProfileCard(
    profile: CompanionProfile,
    enabled: Boolean,
    canRename: Boolean,
    onSetNickname: (String) -> Unit,
) {
    var editing by rememberSaveable { mutableStateOf(false) }
    var nickname by rememberSaveable(profile.nickname) { mutableStateOf(profile.nickname) }
    KitsuCard(title = profile.nickname.ifBlank { "Kitsu" }) {
        Text(
            "${profile.personality.kind.name.lowercase().replaceFirstChar(Char::uppercase)} · " +
                "bond level ${profile.bond.level}",
            style = MaterialTheme.typography.titleMedium,
        )
        Text(
            "Warmth ${profile.personality.warmth} · Play ${profile.personality.playfulness} · " +
                "Bold ${profile.personality.boldness} · Curiosity ${profile.personality.curiosity}",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        profile.favorite?.let {
            Text("Favorite: ${CompanionFeaturePresentationPolicy.actionLabel(it.action)}")
        }
        profile.routine?.let {
            Text(
                "Learned routine: ${CompanionFeaturePresentationPolicy.actionLabel(it.action)} " +
                    "in the ${it.time.name.lowercase()}",
            )
        }
        profile.latestMemory?.let {
            Text(
                listOf(it.line1, it.line2).filter(String::isNotBlank).joinToString(" "),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (canRename && editing) {
            OutlinedTextField(
                value = nickname,
                onValueChange = { value ->
                    nickname = value.filter { it.code in 0x20..0x7e }.take(24)
                },
                singleLine = true,
                label = { Text("Nickname") },
                supportingText = { Text("Up to 24 letters, numbers, spaces, or punctuation") },
                modifier = Modifier.fillMaxWidth().testTag("companion-nickname-field"),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(
                    onClick = {
                        onSetNickname(nickname.trim())
                        editing = false
                    },
                    enabled = enabled && nickname.trim().isNotEmpty(),
                ) { Text("Save") }
                OutlinedButton(onClick = { editing = false }) { Text("Cancel") }
            }
        } else if (canRename) {
            OutlinedButton(onClick = { editing = true }, enabled = enabled) {
                Text("Change nickname")
            }
        }
    }
}

@Composable
private fun CompanionCheckInCard(
    profile: CompanionProfile,
    enabled: Boolean,
    canAnswer: Boolean,
    onAnswerRequest: (Boolean) -> Unit,
    onAnswerQuestion: (Int) -> Unit,
) {
    val checkIn = profile.checkIn
    KitsuCard(title = "Today's check-in") {
        if (checkIn.comfort.kind.name != "NONE") {
            Text(checkIn.comfort.line1, style = MaterialTheme.typography.titleMedium)
            if (checkIn.comfort.line2.isNotBlank()) Text(checkIn.comfort.line2)
        }
        if (canAnswer && checkIn.request.state == CompanionRequestState.PENDING) {
            Text(
                "Kitsu wants to ${CompanionFeaturePresentationPolicy.actionLabel(checkIn.request.action).lowercase()}.",
                style = MaterialTheme.typography.titleMedium,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(onClick = { onAnswerRequest(true) }, enabled = enabled) { Text("Let's do it") }
                OutlinedButton(onClick = { onAnswerRequest(false) }, enabled = enabled) {
                    Text("Not now")
                }
            }
        }
        checkIn.question?.takeIf { canAnswer }?.let { question ->
            Text(
                CompanionFeaturePresentationPolicy.questionTitle(question.kind),
                style = MaterialTheme.typography.titleMedium,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                FilledTonalButton(onClick = { onAnswerQuestion(0) }, enabled = enabled) {
                    Text(question.option0)
                }
                FilledTonalButton(onClick = { onAnswerQuestion(1) }, enabled = enabled) {
                    Text(question.option1)
                }
            }
        }
        val goal = profile.goal
        Text(
            "Daily goal: ${CompanionFeaturePresentationPolicy.actionLabel(goal.action)} " +
                "${goal.progress}/${goal.target}",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        LinearProgressIndicator(
            progress = { (goal.progress.toFloat() / max(1, goal.target)).coerceIn(0f, 1f) },
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

@Composable
private fun FocusCompanionCard(
    state: FocusSessionState,
    enabled: Boolean,
    onStart: (Int) -> Unit,
    onStop: () -> Unit,
    onCancel: () -> Unit,
    onAcknowledge: () -> Unit,
) {
    var customMinutes by rememberSaveable { mutableIntStateOf(25) }
    val receivedAt = remember(state.sessionId, state.sequence, state.phase, state.elapsedMs) {
        SystemClock.elapsedRealtime()
    }
    var now by remember(state.sessionId, state.sequence, state.phase, state.elapsedMs) {
        mutableLongStateOf(receivedAt)
    }
    val active = state.phase == FocusPhase.FOCUS || state.phase == FocusPhase.BREAK
    LaunchedEffect(state.sessionId, state.sequence, state.phase, state.elapsedMs) {
        while (active) {
            delay(1_000L)
            now = SystemClock.elapsedRealtime()
        }
    }
    val elapsedSinceSnapshotMs = (now - receivedAt).coerceAtLeast(0L)
    val timeline = CompanionFeaturePresentationPolicy.focusTimeline(
        state,
        elapsedSinceSnapshotMs,
    )
    KitsuCard(title = "Focus timer with Kitsu") {
        Text(
            "This timer is stored on Kitsu, so it keeps running if you leave the app. " +
                "It does not block apps or silence phone notifications.",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            "Kitsu's part: it keeps the timer and wakes its display when focus time ends " +
                "and when the full session completes. There is no hidden item or bond reward.",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            CompanionFeaturePresentationPolicy.focusStatus(state, elapsedSinceSnapshotMs),
            style = MaterialTheme.typography.titleMedium,
        )
        when (state.phase) {
            FocusPhase.IDLE -> {
                Text("Choose focus time. Kitsu adds a recommended break automatically.")
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Button(
                        onClick = { onStart(25) },
                        enabled = enabled,
                        modifier = Modifier.weight(1f),
                    ) { Text("25 min focus") }
                    FilledTonalButton(
                        onClick = { onStart(50) },
                        enabled = enabled,
                        modifier = Modifier.weight(1f),
                    ) { Text("50 min focus") }
                }
                Text(
                    "25 minutes adds a 5-minute break; 50 minutes adds a 10-minute break.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    "Custom: $customMinutes min focus · " +
                        "${PetFeaturePolicy.recommendedBreakMinutes(customMinutes)} min break",
                    style = MaterialTheme.typography.titleSmall,
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    OutlinedButton(
                        onClick = { customMinutes = max(5, customMinutes - 5) },
                        enabled = enabled && customMinutes > 5,
                        modifier = Modifier.weight(1f),
                    ) { Text("−5 min") }
                    OutlinedButton(
                        onClick = { customMinutes = (customMinutes + 5).coerceAtMost(120) },
                        enabled = enabled && customMinutes < 120,
                        modifier = Modifier.weight(1f),
                    ) { Text("+5 min") }
                }
                OutlinedButton(
                    onClick = { onStart(customMinutes) },
                    enabled = enabled,
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Start custom timer") }
            }
            FocusPhase.FOCUS, FocusPhase.BREAK -> {
                Text(
                    "${CompanionFeaturePresentationPolicy.formatDuration(timeline.phaseElapsedMs)} of " +
                        CompanionFeaturePresentationPolicy.formatDuration(timeline.phaseDurationMs),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                LinearProgressIndicator(
                    progress = { timeline.phaseProgress },
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(
                    "Full session: ${CompanionFeaturePresentationPolicy.formatDuration(timeline.sessionElapsedMs)} " +
                        "of ${CompanionFeaturePresentationPolicy.formatDuration(timeline.sessionDurationMs)}",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                LinearProgressIndicator(
                    progress = { timeline.sessionProgress },
                    modifier = Modifier.fillMaxWidth(),
                )
                if (state.prompt.detail.isNotBlank()) {
                    Text("Kitsu says: ${state.prompt.detail.lowercase().replaceFirstChar(Char::uppercase)}")
                }
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    OutlinedButton(onClick = onStop, enabled = enabled) { Text("Finish early") }
                    OutlinedButton(onClick = onCancel, enabled = enabled) { Text("Cancel") }
                }
            }
            FocusPhase.COMPLETED -> {
                Text(state.prompt.detail.ifBlank { "The timer has finished." })
                if (state.prompt.recommendPulseBreathing) {
                    Text("Kitsu suggests a short breathing break.")
                }
                Text(
                    "Session time: ${CompanionFeaturePresentationPolicy.formatDuration(timeline.sessionElapsedMs)} " +
                        "of ${CompanionFeaturePresentationPolicy.formatDuration(timeline.sessionDurationMs)}",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Button(onClick = onAcknowledge, enabled = enabled) { Text("Clear timer") }
            }
        }
    }
}

@Composable
private fun WalkCompanionCard(
    state: WalkAdventureState,
    selectedDeviceAddress: String?,
    walkSteps: WalkStepSnapshot,
    enabled: Boolean,
    onStart: (WalkStartCommand) -> Unit,
    onSync: () -> Unit,
    onDecide: (WalkDecision) -> Unit,
    onFinish: () -> Unit,
    onAcknowledge: () -> Unit,
    onRequestWalkPermission: () -> Unit,
) {
    var terrain by rememberSaveable { mutableStateOf(WalkTerrain.MEADOW) }
    var targetSteps by rememberSaveable { mutableIntStateOf(2_000) }
    KitsuCard(title = "Walk with Kitsu") {
        Text(
            CompanionFeaturePresentationPolicy.walkStatus(state),
            style = MaterialTheme.typography.titleMedium,
        )
        Text(
            "Phone steps count only while a Kitsu walk is active; this is not a daily step counter.",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            WalkStepPresentationPolicy.status(walkSteps, state, selectedDeviceAddress),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        when (walkSteps.availability) {
            WalkStepAvailability.AVAILABLE,
            WalkStepAvailability.SENSOR_UNAVAILABLE,
            WalkStepAvailability.CLOSED,
            -> Unit
            WalkStepAvailability.PERMISSION_REQUIRED -> {
                if (state.phase != WalkPhase.IDLE) {
                    OutlinedButton(onClick = onRequestWalkPermission) {
                        Text("Allow phone steps")
                    }
                }
            }
            WalkStepAvailability.REGISTRATION_FAILED -> {
                if (state.phase != WalkPhase.IDLE) {
                    OutlinedButton(onClick = onRequestWalkPermission) { Text("Retry phone steps") }
                }
            }
        }
        when (state.phase) {
            WalkPhase.IDLE -> {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    WalkTerrain.entries.take(3).forEach { option ->
                        val selected = option == terrain
                        if (selected) {
                            Button(onClick = { terrain = option }, enabled = enabled) {
                                Text(option.name.lowercase().replaceFirstChar(Char::uppercase))
                            }
                        } else {
                            OutlinedButton(onClick = { terrain = option }, enabled = enabled) {
                                Text(option.name.lowercase().replaceFirstChar(Char::uppercase))
                            }
                        }
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    OutlinedButton(
                        onClick = { targetSteps = max(500, targetSteps - 500) },
                        enabled = enabled && targetSteps > 500,
                    ) { Text("−500") }
                    Text("$targetSteps steps", modifier = Modifier.padding(top = 12.dp))
                    OutlinedButton(
                        onClick = { targetSteps = (targetSteps + 500).coerceAtMost(50_000) },
                        enabled = enabled && targetSteps < 50_000,
                    ) { Text("+500") }
                }
                val startCommand = WalkStartCommand(
                    terrain = terrain,
                    objective = WalkObjective.EXPLORE,
                    risk = WalkRisk.BALANCED,
                    weather = WalkWeather.UNKNOWN,
                    targetSteps = targetSteps.toLong(),
                    commuteSafe = false,
                )
                val stepSetupRequired =
                    WalkStepPresentationPolicy.requestsStepSetupInsteadOfStarting(walkSteps)
                Button(
                    onClick = {
                        if (stepSetupRequired) {
                            onRequestWalkPermission()
                        } else {
                            onStart(startCommand)
                        }
                    },
                    enabled = enabled || stepSetupRequired,
                    modifier = Modifier.fillMaxWidth(),
                ) { Text(WalkStepPresentationPolicy.startLabel(walkSteps)) }
                if (stepSetupRequired) {
                    OutlinedButton(
                        onClick = { onStart(startCommand) },
                        enabled = enabled,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("Start without phone steps") }
                }
            }
            WalkPhase.ACTIVE -> {
                LinearProgressIndicator(
                    progress = { (state.progressPercent / 100f).coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(
                    "${state.terrain.name.lowercase().replaceFirstChar(Char::uppercase)} · " +
                        "${state.distanceMeters} m",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Button(
                        onClick = onSync,
                        enabled = enabled,
                        modifier = Modifier.weight(1f),
                    ) { Text("Sync steps") }
                    OutlinedButton(
                        onClick = { onDecide(WalkDecision.CONTINUE) },
                        enabled = enabled,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text("Continue")
                    }
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    OutlinedButton(
                        onClick = { onDecide(WalkDecision.DETOUR) },
                        enabled = enabled,
                        modifier = Modifier.weight(1f),
                    ) {
                        Text("Detour")
                    }
                    OutlinedButton(
                        onClick = onFinish,
                        enabled = enabled,
                        modifier = Modifier.weight(1f),
                    ) { Text("Head home") }
                }
            }
            WalkPhase.AWAITING_RESCUE -> {
                Button(onClick = { onDecide(WalkDecision.HELP) }, enabled = enabled) {
                    Text("Help Kitsu")
                }
                OutlinedButton(onClick = { onDecide(WalkDecision.RETURN) }, enabled = enabled) {
                    Text("Return together")
                }
            }
            WalkPhase.RETURNED -> {
                state.postcard?.let {
                    Text(it.title, fontWeight = FontWeight.Bold)
                    Text(it.line)
                }
                Text("${state.steps} steps · ${state.distanceMeters} m")
                Button(onClick = onAcknowledge, enabled = enabled) { Text("Keep postcard") }
            }
        }
    }
}

@Composable
private fun FeatureErrorCard(
    feature: String,
    code: String,
    enabled: Boolean,
    onRefresh: () -> Unit,
) {
    KitsuCard {
        Text("$feature needs attention", style = MaterialTheme.typography.titleMedium)
        Text(code.humanized(), color = MaterialTheme.colorScheme.onSurfaceVariant)
        OutlinedButton(onClick = onRefresh, enabled = enabled) { Text("Refresh") }
    }
}
