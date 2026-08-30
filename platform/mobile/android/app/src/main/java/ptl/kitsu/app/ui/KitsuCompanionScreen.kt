package ptl.kitsu.app.ui

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
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
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
import kotlin.math.max

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

    fun focusStatus(state: FocusSessionState): String = when (state.phase) {
        FocusPhase.IDLE -> "Ready when you are"
        FocusPhase.FOCUS -> "Focusing · ${minutesAndSeconds(state.remainingMs)} left"
        FocusPhase.BREAK -> "Break · ${minutesAndSeconds(state.remainingMs)} left"
        FocusPhase.COMPLETED -> state.prompt.title.ifBlank { "Session complete" }
    }

    fun walkStatus(state: WalkAdventureState): String = when (state.phase) {
        WalkPhase.IDLE -> "Choose a route and take Kitsu with you."
        WalkPhase.ACTIVE -> "${state.steps} of ${state.targetSteps} steps · ${state.progressPercent}%"
        WalkPhase.AWAITING_RESCUE -> "Kitsu needs your decision before continuing."
        WalkPhase.RETURNED -> state.postcard?.title ?: "Back home"
    }

    private fun minutesAndSeconds(milliseconds: Long): String {
        val totalSeconds = max(0L, milliseconds) / 1_000L
        return "%d:%02d".format(totalSeconds / 60L, totalSeconds % 60L)
    }
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
    onOpenAccessibility: () -> Unit,
    onOpenStudio: () -> Unit,
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

        item {
            KitsuCard(title = "See Kitsu your way") {
                Text(
                    "Open a large accessible mirror, or capture the exact animation frame your Kitsu is showing.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    OutlinedButton(
                        onClick = onOpenAccessibility,
                        enabled = enabled,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text("Accessible view") }
                    OutlinedButton(
                        onClick = onOpenStudio,
                        enabled = enabled && owner.companionProfileSupported,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text("Pet Studio") }
                }
                if (canOpenGuide) {
                    OutlinedButton(
                        onClick = onOpenGuide,
                        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    ) { Text("Creature guide") }
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
    KitsuCard(title = "Focus with Kitsu") {
        Text(
            CompanionFeaturePresentationPolicy.focusStatus(state),
            style = MaterialTheme.typography.titleMedium,
        )
        when (state.phase) {
            FocusPhase.IDLE -> {
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Button(onClick = { onStart(25) }, enabled = enabled) { Text("25 min") }
                    FilledTonalButton(onClick = { onStart(50) }, enabled = enabled) { Text("50 min") }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    OutlinedButton(
                        onClick = { customMinutes = max(5, customMinutes - 5) },
                        enabled = enabled && customMinutes > 5,
                    ) { Text("−5") }
                    Text("$customMinutes min", modifier = Modifier.padding(top = 12.dp))
                    OutlinedButton(
                        onClick = { customMinutes = (customMinutes + 5).coerceAtMost(120) },
                        enabled = enabled && customMinutes < 120,
                    ) { Text("+5") }
                    OutlinedButton(onClick = { onStart(customMinutes) }, enabled = enabled) {
                        Text("Start")
                    }
                }
            }
            FocusPhase.FOCUS, FocusPhase.BREAK -> {
                val total = if (state.phase == FocusPhase.FOCUS) {
                    state.focusMinutes * 60_000L
                } else {
                    state.breakMinutes * 60_000L
                }
                LinearProgressIndicator(
                    progress = { (1f - state.remainingMs.toFloat() / max(1L, total)).coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth(),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    OutlinedButton(onClick = onStop, enabled = enabled) { Text("Finish early") }
                    OutlinedButton(onClick = onCancel, enabled = enabled) { Text("Cancel") }
                }
            }
            FocusPhase.COMPLETED -> {
                Text(state.prompt.detail)
                if (state.prompt.recommendPulseBreathing) {
                    Text("Kitsu suggests a short breathing break.")
                }
                Button(onClick = onAcknowledge, enabled = enabled) { Text("Done") }
            }
        }
    }
}

@Composable
private fun WalkCompanionCard(
    state: WalkAdventureState,
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
        when (walkSteps.availability) {
            WalkStepAvailability.AVAILABLE -> Text(
                if (walkSteps.routeId == state.routeId && state.routeId != 0L) {
                    "Phone step counter: ${walkSteps.stepsTotal} steps for this walk"
                } else {
                    "Phone step counter ready"
                },
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            WalkStepAvailability.PERMISSION_REQUIRED -> {
                Text(
                    "Allow Physical activity so phone steps can advance this walk.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedButton(onClick = onRequestWalkPermission) {
                    Text("Allow step access")
                }
            }
            WalkStepAvailability.SENSOR_UNAVAILABLE -> Text(
                "This phone has no compatible step counter. Kitsu's walk can still be used without automatic steps.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            WalkStepAvailability.REGISTRATION_FAILED -> {
                Text(
                    "The phone step counter is temporarily unavailable.",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedButton(onClick = onRequestWalkPermission) { Text("Retry step counter") }
            }
            WalkStepAvailability.CLOSED -> Text(
                "Phone step tracking is unavailable until the app is reopened.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
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
                Button(
                    onClick = {
                        onStart(
                            WalkStartCommand(
                                terrain = terrain,
                                objective = WalkObjective.EXPLORE,
                                risk = WalkRisk.BALANCED,
                                weather = WalkWeather.UNKNOWN,
                                targetSteps = targetSteps.toLong(),
                                commuteSafe = false,
                            ),
                        )
                    },
                    enabled = enabled,
                ) { Text("Start walk") }
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
