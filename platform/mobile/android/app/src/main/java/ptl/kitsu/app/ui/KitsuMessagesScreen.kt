package ptl.kitsu.app.ui

import android.text.format.DateFormat
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.automirrored.filled.HelpOutline
import androidx.compose.material.icons.filled.AddComment
import androidx.compose.material.icons.filled.Cancel
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.Forum
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Route
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material.icons.filled.WifiTethering
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import java.time.Instant
import java.util.Date
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import ptl.kitsu.app.BuildConfig
import ptl.kitsu.app.MainViewModel
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.MessageRoute
import ptl.kitsu.app.model.MeshPeerKeyPolicy
import ptl.kitsu.app.repository.OwnerState

@Composable
internal fun KitsuMessagesScreen(
    owner: OwnerState,
    viewModel: MainViewModel,
    selectedThreadKey: String?,
    onSelectedThreadKeyChange: (String?) -> Unit,
    useMasterDetail: Boolean,
    updateBusy: Boolean,
    messageSubmissionInFlight: Boolean = false,
    policyAccepted: Boolean,
    blockedPeerIds: Set<String>,
    onAcceptPolicy: () -> Unit,
    onBlockPeer: (String) -> Unit,
    onExportReport: (ModerationReport) -> Unit,
    modifier: Modifier = Modifier,
) {
    var showPolicy by rememberSaveable { mutableStateOf(false) }
    var showNewConversation by rememberSaveable { mutableStateOf(false) }
    var manualDirectTarget by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingReportKey by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingReportType by rememberSaveable { mutableStateOf(ReportType.MESSAGE) }
    var pendingBlockPeerId by rememberSaveable { mutableStateOf<String?>(null) }
    var reportReason by rememberSaveable { mutableStateOf(ReportReason.SPAM_OR_SCAM) }
    var reportNote by rememberSaveable { mutableStateOf("") }
    var conversationDrafts by rememberSaveable { mutableStateOf<Map<String, String>>(emptyMap()) }
    val lifecycleOwner = LocalLifecycleOwner.current
    var detailVisibleAndResumed by remember {
        mutableStateOf(lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED))
    }
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, _ ->
            detailVisibleAndResumed = lifecycleOwner.lifecycle.currentState
                .isAtLeast(Lifecycle.State.RESUMED)
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    val discoveredThreads = MessageThreadPolicy.build(
        messages = owner.messages,
        peers = owner.peers,
        channels = owner.channels,
        blockedPeerIds = blockedPeerIds,
        currentJournalSession = owner.messageJournalSession,
    )
    val manualThread = manualDirectTarget
        ?.takeIf { MeshPeerKeyPolicy.isCanonicalBase64Url(it) && it !in blockedPeerIds }
        ?.takeIf { target -> discoveredThreads.none { it.key == MessageThreadPolicy.directKey(target) } }
        ?.let { target ->
            MessageThread(
                key = MessageThreadPolicy.directKey(target),
                route = MessageRoute.DIRECT,
                target = target,
                title = MessageComposerPolicy.compactReference(target),
                subtitle = "Direct message · ${MessageComposerPolicy.compactReference(target)}",
                messages = emptyList(),
                unreadCount = 0,
                latestJournalPosition = -1,
            )
        }
    val threads = if (manualThread == null) discoveredThreads else discoveredThreads + manualThread
    val selectedThread = threads.firstOrNull { it.key == selectedThreadKey }
    val hiddenMessageCount = owner.messages.count { it.peerId in blockedPeerIds }

    LaunchedEffect(selectedThreadKey, selectedThread?.key, threads.size) {
        if (selectedThreadKey != null && selectedThread == null) onSelectedThreadKeyChange(null)
    }

    val unreadLiveIds = selectedThread?.messages.orEmpty()
        .filter { message ->
            owner.messageJournalSession != null &&
                message.journalSession == owner.messageJournalSession &&
                message.direction.equals("inbound", ignoreCase = true) &&
                message.unreadOnKitsu == true
        }
        .map(Message::id)
    val unreadSignature = unreadLiveIds.joinToString(",") { id ->
        "${owner.messageJournalSession}:$id"
    }
    LaunchedEffect(
        selectedThread?.key,
        unreadSignature,
        owner.connection.connected,
        owner.messageMarkReadSupported,
        detailVisibleAndResumed,
        updateBusy,
    ) {
        val session = owner.messageJournalSession
        if (selectedThread != null && session != null && unreadLiveIds.isNotEmpty() &&
            owner.connection.connected && owner.messageMarkReadSupported && !updateBusy
            && detailVisibleAndResumed
        ) {
            viewModel.markMessagesRead(session, unreadLiveIds)
        }
    }

    Box(modifier.fillMaxSize().testTag("messages-screen")) {
        val expanded = useMasterDetail
        if (expanded) {
            Row(Modifier.fillMaxSize().testTag("layout-messages-master-detail")) {
                ConversationListPane(
                    owner = owner,
                    threads = threads,
                    selectedThreadKey = selectedThreadKey,
                    hiddenMessageCount = hiddenMessageCount,
                    updateBusy = updateBusy,
                    onSelectThread = onSelectedThreadKeyChange,
                    onNewConversation = { showNewConversation = true },
                    onRefresh = viewModel::refreshMessages,
                    modifier = Modifier.width(336.dp).fillMaxHeight(),
                )
                Surface(
                    color = MaterialTheme.colorScheme.outlineVariant,
                    modifier = Modifier.fillMaxHeight().width(1.dp),
                ) {}
                if (selectedThread == null) {
                    EmptyDetailPane(Modifier.weight(1f))
                } else {
                    key(selectedThread.key) {
                        ConversationDetailPane(
                            owner = owner,
                            thread = selectedThread,
                            viewModel = viewModel,
                            updateBusy = updateBusy,
                            messageSubmissionInFlight = messageSubmissionInFlight,
                            policyAccepted = policyAccepted,
                            body = conversationDrafts[selectedThread.key].orEmpty(),
                            onBodyChange = { draft ->
                                conversationDrafts = conversationDrafts + (selectedThread.key to draft)
                            },
                            showHeader = true,
                            unreadLiveIds = unreadLiveIds,
                            onReviewPolicy = { showPolicy = true },
                            onRetryRead = {
                                owner.messageJournalSession?.let { session ->
                                    viewModel.markMessagesRead(session, unreadLiveIds)
                                }
                            },
                            onReport = { message, type ->
                                pendingReportKey = message.uiStableJournalKey()
                                pendingReportType = type
                                reportReason = ReportReason.SPAM_OR_SCAM
                                reportNote = ""
                            },
                            onBlock = { pendingBlockPeerId = it },
                            modifier = Modifier.weight(1f),
                        )
                    }
                }
            }
        } else if (selectedThread == null) {
            ConversationListPane(
                owner = owner,
                threads = threads,
                selectedThreadKey = null,
                hiddenMessageCount = hiddenMessageCount,
                updateBusy = updateBusy,
                onSelectThread = onSelectedThreadKeyChange,
                onNewConversation = { showNewConversation = true },
                onRefresh = viewModel::refreshMessages,
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            key(selectedThread.key) {
                ConversationDetailPane(
                    owner = owner,
                    thread = selectedThread,
                    viewModel = viewModel,
                    updateBusy = updateBusy,
                    messageSubmissionInFlight = messageSubmissionInFlight,
                    policyAccepted = policyAccepted,
                    body = conversationDrafts[selectedThread.key].orEmpty(),
                    onBodyChange = { draft ->
                        conversationDrafts = conversationDrafts + (selectedThread.key to draft)
                    },
                    showHeader = false,
                    unreadLiveIds = unreadLiveIds,
                    onReviewPolicy = { showPolicy = true },
                    onRetryRead = {
                        owner.messageJournalSession?.let { session ->
                            viewModel.markMessagesRead(session, unreadLiveIds)
                        }
                    },
                    onReport = { message, type ->
                        pendingReportKey = message.uiStableJournalKey()
                        pendingReportType = type
                        reportReason = ReportReason.SPAM_OR_SCAM
                        reportNote = ""
                    },
                    onBlock = { pendingBlockPeerId = it },
                    modifier = Modifier.fillMaxSize(),
                )
            }
        }
    }

    if (showNewConversation) {
        NewConversationDialog(
            owner = owner,
            blockedPeerIds = blockedPeerIds,
            onSelect = { route, target ->
                if (route == MessageRoute.DIRECT) manualDirectTarget = target
                onSelectedThreadKeyChange(
                    if (route == MessageRoute.DIRECT) MessageThreadPolicy.directKey(target)
                    else MessageThreadPolicy.channelKey(target.toInt()),
                )
                showNewConversation = false
            },
            onDismiss = { showNewConversation = false },
        )
    }

    if (showPolicy) {
        MeshPolicyDialog(
            canAccept = !updateBusy,
            onAccept = {
                onAcceptPolicy()
                showPolicy = false
            },
            onDismiss = { showPolicy = false },
        )
    }

    pendingBlockPeerId?.let { peerId ->
        AlertDialog(
            onDismissRequest = { pendingBlockPeerId = null },
            title = { Text("Block this sender on this phone?") },
            text = {
                Text(
                    "Messages from this peer will be hidden here and the peer will be removed from recipient suggestions. Your Kitsu firmware may still receive its mesh traffic.",
                )
            },
            confirmButton = {
                Button(onClick = {
                    onBlockPeer(peerId)
                    pendingBlockPeerId = null
                    if (selectedThread?.target == peerId) onSelectedThreadKeyChange(null)
                }) { Text("Block sender") }
            },
            dismissButton = {
                TextButton(onClick = { pendingBlockPeerId = null }) { Text("Cancel") }
            },
        )
    }

    pendingReportKey?.let { stableKey ->
        owner.messages.firstOrNull { it.uiStableJournalKey() == stableKey }?.let { message ->
            ReportMessageDialog(
                message = message,
                reason = reportReason,
                reportType = pendingReportType,
                note = reportNote,
                onReasonChange = { reportReason = it },
                onNoteChange = { reportNote = it },
                onExport = {
                    onExportReport(
                        message.toModerationReport(owner, pendingReportType, reportReason, reportNote),
                    )
                    pendingReportKey = null
                },
                onDismiss = { pendingReportKey = null },
            )
        }
    }
}

@Composable
private fun ConversationListPane(
    owner: OwnerState,
    threads: List<MessageThread>,
    selectedThreadKey: String?,
    hiddenMessageCount: Int,
    updateBusy: Boolean,
    onSelectThread: (String) -> Unit,
    onNewConversation: () -> Unit,
    onRefresh: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val channelThreads = threads.filter { it.route == MessageRoute.CHANNEL }
    val directThreads = threads.filter { it.route == MessageRoute.DIRECT }
    LazyColumn(
        modifier = modifier.testTag("messages-thread-list"),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                SectionHeading(
                    title = "Messages",
                    supporting = if (owner.connection.connected) {
                        "Recent conversations carried by this Kitsu."
                    } else {
                        "Encrypted history is available offline. Connect to refresh or send."
                    },
                    modifier = Modifier.weight(1f),
                )
                IconButton(
                    onClick = onNewConversation,
                    enabled = !updateBusy,
                    modifier = Modifier.testTag("messages-new"),
                ) {
                    Icon(Icons.Default.AddComment, contentDescription = "New conversation")
                }
            }
        }

        if (owner.messagesErrorCode != null) {
            item {
                StatePanel(
                    title = "Messages could not refresh",
                    message = owner.messagesErrorCode.humanized(),
                    kind = StatePanelKind.ERROR,
                    actionLabel = if (owner.connection.connected && !updateBusy) "Try again" else null,
                    onAction = if (owner.connection.connected && !updateBusy) onRefresh else null,
                    testTag = "messages-error",
                )
            }
        }

        when {
            owner.loading && threads.isEmpty() -> item {
                StatePanel(
                    title = "Loading conversations",
                    message = "Reading the local message journal from your Kitsu.",
                    kind = StatePanelKind.LOADING,
                    testTag = "messages-loading",
                )
            }
            threads.isEmpty() -> item {
                StatePanel(
                    title = if (hiddenMessageCount > 0) "Blocked conversations hidden" else "No conversations yet",
                    message = if (hiddenMessageCount > 0) {
                        "Manage blocked peers in Settings. Blocking is local to this phone."
                    } else {
                        "Start with a nearby peer, configured channel, or trusted public key."
                    },
                    actionLabel = "New conversation",
                    onAction = onNewConversation,
                    testTag = "messages-empty",
                )
            }
        }

        if (channelThreads.isNotEmpty()) {
            item { ConversationSectionLabel("Channels") }
            items(channelThreads, key = MessageThread::key) { thread ->
                ConversationRow(
                    thread = thread,
                    selected = selectedThreadKey == thread.key,
                    onClick = { onSelectThread(thread.key) },
                )
            }
        }
        if (directThreads.isNotEmpty()) {
            item { ConversationSectionLabel("Direct") }
            items(directThreads, key = MessageThread::key) { thread ->
                ConversationRow(
                    thread = thread,
                    selected = selectedThreadKey == thread.key,
                    onClick = { onSelectThread(thread.key) },
                )
            }
        }

        if (hiddenMessageCount > 0) {
            item {
                Text(
                    "$hiddenMessageCount message${if (hiddenMessageCount == 1) "" else "s"} hidden from blocked peers on this phone.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.testTag("blocked-message-count"),
                )
            }
        }
    }
}

@Composable
private fun ConversationSectionLabel(label: String) {
    Text(
        label.uppercase(),
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(top = 8.dp, start = 4.dp),
    )
}

@Composable
private fun ConversationRow(
    thread: MessageThread,
    selected: Boolean,
    onClick: () -> Unit,
) {
    val latest = thread.latestMessage
    val preview = when {
        latest == null -> "No messages yet"
        latest.direction.equals("outbound", ignoreCase = true) -> "You: ${latest.text}"
        thread.route == MessageRoute.CHANNEL && latest.senderName.isNotBlank() ->
            "${latest.senderName}: ${latest.text}"
        else -> latest.text
    }
    Surface(
        onClick = onClick,
        shape = MaterialTheme.shapes.medium,
        color = if (selected) MaterialTheme.colorScheme.secondaryContainer
        else MaterialTheme.colorScheme.surface,
        tonalElevation = if (selected) 4.dp else 1.dp,
        modifier = Modifier.fillMaxWidth()
            .testTag("message-thread-${thread.key}")
            .semantics {
                contentDescription = "${thread.title}. $preview"
                if (thread.unreadCount > 0) {
                    stateDescription = "${thread.unreadCount} unread"
                }
            },
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SenderAvatar(thread.title, thread.route, 44.dp)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    thread.title,
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    preview,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(5.dp)) {
                latest?.let {
                    val timeLabel = localizedMessageTimeLabel(it.occurredAt)
                    if (timeLabel.isNotEmpty()) {
                        Text(
                            timeLabel,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                if (thread.unreadCount > 0) {
                    Surface(
                        shape = CircleShape,
                        color = MaterialTheme.colorScheme.primary,
                        contentColor = MaterialTheme.colorScheme.onPrimary,
                    ) {
                        Text(
                            thread.unreadCount.coerceAtMost(99).toString(),
                            style = MaterialTheme.typography.labelLarge,
                            modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun EmptyDetailPane(modifier: Modifier = Modifier) {
    Box(modifier.fillMaxSize().padding(24.dp), contentAlignment = Alignment.Center) {
        StatePanel(
            title = "Select a conversation",
            message = "Choose a direct peer or channel to read and send messages.",
            testTag = "conversation-detail-empty",
        )
    }
}

@Composable
private fun ConversationDetailPane(
    owner: OwnerState,
    thread: MessageThread,
    viewModel: MainViewModel,
    updateBusy: Boolean,
    messageSubmissionInFlight: Boolean,
    policyAccepted: Boolean,
    body: String,
    onBodyChange: (String) -> Unit,
    showHeader: Boolean,
    unreadLiveIds: List<String>,
    onReviewPolicy: () -> Unit,
    onRetryRead: () -> Unit,
    onReport: (Message, ReportType) -> Unit,
    onBlock: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val timeline = remember(thread.messages) { MessageThreadPolicy.timeline(thread.messages) }
    val channelRouting = if (thread.route == MessageRoute.CHANNEL && thread.channelRoutingKnown) {
        ChannelRoutingPresentationPolicy.present(thread.channelRegionScope)
    } else {
        null
    }
    val listState = rememberLazyListState()
    val scrollScope = rememberCoroutineScope()
    val lastTimelineKey = timeline.lastOrNull()?.stableKey
    var previousTimelineSize by remember(thread.key) { mutableStateOf(0) }
    var showNewMessage by remember(thread.key) { mutableStateOf(false) }
    val composerEnabled = policyAccepted && owner.connection.connected &&
        !updateBusy && !messageSubmissionInFlight
    val draftEnabled = !updateBusy && !messageSubmissionInFlight
    val validation = MessageComposerPolicy.validationError(thread.route, thread.target, body)

    LaunchedEffect(thread.key, lastTimelineKey) {
        if (timeline.isNotEmpty()) {
            val previousLastVisible = listState.layoutInfo.visibleItemsInfo.lastOrNull()?.index ?: -1
            val wasNearPreviousBottom = previousTimelineSize == 0 ||
                previousLastVisible >= previousTimelineSize - 2
            if (wasNearPreviousBottom) {
                listState.scrollToItem(timeline.lastIndex)
                showNewMessage = false
            } else {
                showNewMessage = true
            }
            previousTimelineSize = timeline.size
        }
    }
    LaunchedEffect(listState, timeline.size) {
        snapshotFlow {
            val layout = listState.layoutInfo
            layout.totalItemsCount > 0 &&
                (layout.visibleItemsInfo.lastOrNull()?.index ?: -1) >= layout.totalItemsCount - 2
        }.collect { nearBottom ->
            if (nearBottom) showNewMessage = false
        }
    }

    Column(
        modifier = modifier.fillMaxSize().imePadding().testTag("conversation-detail"),
    ) {
        if (showHeader) ConversationHeader(thread)
        owner.messagesErrorCode?.let { code ->
            ConversationErrorBanner(
                text = "History may be out of date: ${code.humanized()}",
                actionLabel = if (owner.connection.connected && !updateBusy) "Retry" else null,
                onAction = if (owner.connection.connected && !updateBusy) {
                    { viewModel.refreshMessages() }
                } else null,
                testTag = "conversation-refresh-error",
            )
        }
        if (owner.messageReadErrorCode != null && unreadLiveIds.isNotEmpty()) {
            ConversationErrorBanner(
                text = "Could not mark incoming messages read: ${owner.messageReadErrorCode.humanized()}",
                actionLabel = if (owner.connection.connected && !updateBusy &&
                    owner.messageMarkReadSupported
                ) "Retry" else null,
                onAction = if (owner.connection.connected && !updateBusy &&
                    owner.messageMarkReadSupported
                ) onRetryRead else null,
                testTag = "message-read-error",
            )
        }
        Box(Modifier.weight(1f).fillMaxWidth()) {
            if (timeline.isEmpty()) {
                Box(Modifier.fillMaxSize().padding(18.dp), contentAlignment = Alignment.Center) {
                    StatePanel(
                        title = "Start the conversation",
                        message = if (thread.route == MessageRoute.CHANNEL) {
                            "No retained messages in this channel yet."
                        } else {
                            "No retained direct messages with ${thread.title} yet."
                        },
                        testTag = "conversation-empty",
                    )
                }
            } else {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize().testTag("conversation-list"),
                    contentPadding = PaddingValues(horizontal = 14.dp, vertical = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    items(timeline, key = ConversationTimelineItem::stableKey) { item ->
                        when (item) {
                            is ConversationTimelineItem.DateSeparator -> DateSeparator(item.label)
                            is ConversationTimelineItem.Bubble -> MessageBubble(
                                message = item.message,
                                thread = thread,
                                peers = owner.peers,
                                currentJournalSession = owner.messageJournalSession,
                                updateBusy = updateBusy,
                                onReport = { onReport(item.message, it) },
                                onBlock = item.message.peerId?.let { peerId -> { onBlock(peerId) } },
                            )
                        }
                    }
                }
            }
            if (showNewMessage && timeline.isNotEmpty()) {
                Button(
                    onClick = {
                        scrollScope.launch { listState.animateScrollToItem(timeline.lastIndex) }
                        showNewMessage = false
                    },
                    modifier = Modifier.align(Alignment.BottomCenter)
                        .padding(12.dp)
                        .testTag("conversation-new-message"),
                ) { Text("New message") }
            }
        }
        ConversationComposer(
            body = body,
            onBodyChange = { onBodyChange(MessageComposerPolicy.acceptText(body, it)) },
            onSend = {
                viewModel.sendMessage(thread.target, body, thread.route) { onBodyChange("") }
            },
            enabled = composerEnabled && validation == null,
            fieldEnabled = draftEnabled,
            messageSubmissionInFlight = messageSubmissionInFlight,
            connectionAvailable = owner.connection.connected,
            policyAccepted = policyAccepted,
            validation = validation,
            channelRouting = channelRouting,
            onReviewPolicy = onReviewPolicy,
        )
    }
}

@Composable
private fun ConversationErrorBanner(
    text: String,
    actionLabel: String?,
    onAction: (() -> Unit)?,
    testTag: String,
) {
    Surface(
        color = MaterialTheme.colorScheme.errorContainer,
        contentColor = MaterialTheme.colorScheme.onErrorContainer,
        modifier = Modifier.fillMaxWidth().testTag(testTag),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(Icons.Default.ErrorOutline, contentDescription = null, modifier = Modifier.size(20.dp))
            Text(text, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
            if (actionLabel != null && onAction != null) {
                TextButton(onClick = onAction) { Text(actionLabel) }
            }
        }
    }
}

@Composable
private fun ConversationHeader(thread: MessageThread) {
    val channelRouting = if (thread.route == MessageRoute.CHANNEL && thread.channelRoutingKnown) {
        ChannelRoutingPresentationPolicy.present(thread.channelRegionScope)
    } else {
        null
    }
    Surface(color = MaterialTheme.colorScheme.surface, tonalElevation = 2.dp) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 18.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SenderAvatar(thread.title, thread.route, 42.dp)
            Column(Modifier.weight(1f)) {
                Text(
                    thread.title,
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.testTag("conversation-title"),
                )
                Text(
                    if (thread.route == MessageRoute.CHANNEL) {
                        "${thread.subtitle} · sender names are unverified"
                    } else {
                        thread.subtitle
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                channelRouting?.let { routing ->
                    Text(
                        routing.detail,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.testTag("conversation-channel-routing-header"),
                    )
                }
            }
        }
    }
}

@Composable
private fun DateSeparator(label: String) {
    Row(
        Modifier.fillMaxWidth().padding(vertical = 10.dp).semantics { heading() },
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        HorizontalDivider(Modifier.weight(1f), color = MaterialTheme.colorScheme.outlineVariant)
        Text(
            label,
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        HorizontalDivider(Modifier.weight(1f), color = MaterialTheme.colorScheme.outlineVariant)
    }
}

@Composable
private fun MessageBubble(
    message: Message,
    thread: MessageThread,
    peers: List<ptl.kitsu.app.model.Peer>,
    currentJournalSession: String?,
    updateBusy: Boolean,
    onReport: (ReportType) -> Unit,
    onBlock: (() -> Unit)?,
) {
    val inbound = message.direction.equals("inbound", ignoreCase = true)
    val sender = if (inbound) {
        if (thread.route == MessageRoute.CHANNEL) {
            val displayName = message.senderName.takeIf(String::isNotBlank)
                ?: "Unknown channel sender"
            "$displayName · unverified"
        } else thread.title
    } else "You"
    val directKeyEvidence = message.peerId
        ?.takeIf { inbound && thread.route == MessageRoute.DIRECT }
        ?.takeIf(MeshPeerKeyPolicy::isCanonicalBase64Url)
        ?.let(MessageComposerPolicy::compactReference)
    val spokenStatus = if (inbound) {
        val readStatus = when {
            message.journalSession != currentJournalSession -> "Received"
            message.unreadOnKitsu == true -> "Unread on Kitsu"
            message.unreadOnKitsu == false -> "Read on Kitsu"
            else -> "Received"
        }
        val signalStatus = listOfNotNull(
            message.rssiDbm?.let { "${it.compactNumber()} dBm last hop" },
            message.snrDb?.let { "${it.compactNumber()} dB SNR" },
        ).joinToString(" · ").takeIf(String::isNotEmpty)
        listOfNotNull(
            readStatus,
            MessagePresentationPolicy.inboundCopyRouteLine(message),
            signalStatus,
        ).joinToString(" · ")
    } else MessagePresentationPolicy.accessibilityLine(message, peers)
    Box(
        Modifier.fillMaxWidth()
            .testTag("message-bubble-${message.uiStableJournalKey()}")
            .semantics {
                contentDescription = buildString {
                    append(sender)
                    if (directKeyEvidence != null) append(" · key $directKeyEvidence")
                    append(": ${message.text}. $spokenStatus")
                }
            },
    ) {
        if (inbound) {
            IncomingBubble(
                message = message,
                senderLabel = sender,
                route = thread.route,
                isLiveSession = message.journalSession != null &&
                    message.journalSession == currentJournalSession,
                updateBusy = updateBusy,
                onReport = onReport,
                onBlock = onBlock,
            )
        } else {
            OutgoingBubble(message, peers)
        }
    }
}

@Composable
private fun IncomingBubble(
    message: Message,
    senderLabel: String,
    route: MessageRoute,
    isLiveSession: Boolean,
    updateBusy: Boolean,
    onReport: (ReportType) -> Unit,
    onBlock: (() -> Unit)?,
) {
    val timeLabel = localizedMessageTimeLabel(message.occurredAt)
    var actionsExpanded by rememberSaveable(message.uiStableJournalKey()) { mutableStateOf(false) }
    val stableDirectSender = message.channel == null && message.peerId != null
    Row(
        Modifier.fillMaxWidth().padding(end = 42.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.Top,
    ) {
        SenderAvatar(senderLabel, route, 34.dp)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text(
                senderLabel,
                style = MaterialTheme.typography.labelLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (route == MessageRoute.DIRECT && stableDirectSender) {
                Text(
                    "Key ${MessageComposerPolicy.compactReference(requireNotNull(message.peerId))}",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.testTag(
                        "message-peer-key-${message.uiStableJournalKey()}",
                    ),
                )
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
                Surface(
                    color = if (isLiveSession && message.unreadOnKitsu == true) {
                        MaterialTheme.colorScheme.secondaryContainer
                    } else {
                        MaterialTheme.colorScheme.surfaceVariant
                    },
                    contentColor = MaterialTheme.colorScheme.onSurface,
                    shape = RoundedCornerShape(5.dp, 20.dp, 20.dp, 20.dp),
                    tonalElevation = 1.dp,
                    modifier = Modifier.widthIn(max = 560.dp).weight(1f, fill = false),
                ) {
                    Text(
                        message.text,
                        style = MaterialTheme.typography.bodyLarge,
                        modifier = Modifier.padding(horizontal = 14.dp, vertical = 9.dp),
                    )
                }
                Box(Modifier.size(48.dp)) {
                    IconButton(
                        onClick = { actionsExpanded = true },
                        enabled = !updateBusy,
                        modifier = Modifier.fillMaxSize()
                            .testTag("message-actions-${message.uiStableJournalKey()}"),
                    ) {
                        Icon(Icons.Default.MoreVert, contentDescription = "Message actions")
                    }
                    DropdownMenu(
                        expanded = actionsExpanded,
                        onDismissRequest = { actionsExpanded = false },
                    ) {
                        DropdownMenuItem(
                            text = { Text("Report message") },
                            onClick = {
                                actionsExpanded = false
                                onReport(ReportType.MESSAGE)
                            },
                            modifier = Modifier.testTag("report-message"),
                        )
                        if (stableDirectSender) {
                            DropdownMenuItem(
                                text = { Text("Report sender") },
                                onClick = {
                                    actionsExpanded = false
                                    onReport(ReportType.SENDER)
                                },
                                modifier = Modifier.testTag("report-sender"),
                            )
                            onBlock?.let {
                                DropdownMenuItem(
                                    text = { Text("Block sender") },
                                    onClick = {
                                        actionsExpanded = false
                                        it()
                                    },
                                    modifier = Modifier.testTag("block-sender"),
                                )
                            }
                        }
                    }
                }
            }
            val readLabel = when {
                !isLiveSession -> "Received"
                message.unreadOnKitsu == true -> "Unread on Kitsu"
                message.unreadOnKitsu == false -> "Read on Kitsu"
                else -> "Received"
            }
            val signal = listOfNotNull(
                message.rssiDbm?.let { "${it.compactNumber()} dBm last hop" },
                message.snrDb?.let { "${it.compactNumber()} dB SNR" },
            ).joinToString(" · ")
            val routeEvidence = MessagePresentationPolicy.inboundCopyRouteLine(message)
            Text(
                buildString {
                    if (timeLabel.isNotEmpty()) {
                        append(timeLabel)
                        append(" · ")
                    }
                    append(readLabel)
                    if (routeEvidence != null) append(" · $routeEvidence")
                    if (signal.isNotEmpty()) append(" · $signal")
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.testTag("message-read-${message.uiStableJournalKey()}"),
            )
        }
    }
}

@Composable
private fun OutgoingBubble(message: Message, peers: List<ptl.kitsu.app.model.Peer>) {
    val timeLabel = localizedMessageTimeLabel(message.occurredAt)
    val lifecycle = MessagePresentationPolicy.conversationLine(message)
    val repeatSource = RepeatSourcePresentationPolicy.visibleLine(message, peers)
    val lifecycleIcon = when (message.state) {
        "queued" -> Icons.Default.Schedule
        "sent" -> Icons.Default.WifiTethering
        "delivered" -> Icons.Default.Route
        "unconfirmed" -> Icons.AutoMirrored.Filled.HelpOutline
        "failed" -> Icons.Default.ErrorOutline
        "cancelled" -> Icons.Default.Cancel
        else -> Icons.AutoMirrored.Filled.HelpOutline
    }
    Row(Modifier.fillMaxWidth().padding(start = 58.dp), horizontalArrangement = Arrangement.End) {
        Column(
            horizontalAlignment = Alignment.End,
            verticalArrangement = Arrangement.spacedBy(3.dp),
            modifier = Modifier.widthIn(max = 560.dp),
        ) {
            Text("You", style = MaterialTheme.typography.labelLarge)
            Surface(
                color = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                shape = RoundedCornerShape(20.dp, 5.dp, 20.dp, 20.dp),
                tonalElevation = 2.dp,
            ) {
                Text(
                    message.text,
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.padding(horizontal = 14.dp, vertical = 9.dp),
                )
            }
            Row(
                horizontalArrangement = Arrangement.spacedBy(5.dp),
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.testTag("message-status-${message.uiStableJournalKey()}"),
            ) {
                Text(
                    if (timeLabel.isEmpty()) lifecycle else "$timeLabel · $lifecycle",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Icon(
                    lifecycleIcon,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(15.dp),
                )
            }
            repeatSource?.let { source ->
                Text(
                    source,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.testTag("message-repeat-source-${message.uiStableJournalKey()}"),
                )
            }
        }
    }
}

@Composable
private fun ConversationComposer(
    body: String,
    onBodyChange: (String) -> Unit,
    onSend: () -> Unit,
    enabled: Boolean,
    fieldEnabled: Boolean,
    messageSubmissionInFlight: Boolean,
    connectionAvailable: Boolean,
    policyAccepted: Boolean,
    validation: String?,
    channelRouting: ChannelRoutingPresentation?,
    onReviewPolicy: () -> Unit,
) {
    Surface(
        color = MaterialTheme.colorScheme.surface,
        tonalElevation = 6.dp,
        modifier = Modifier.fillMaxWidth().testTag("conversation-composer"),
    ) {
        Column(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            if (!policyAccepted) {
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "Accept the mesh messaging policy before sending.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                        modifier = Modifier.weight(1f).testTag("message-policy-gate"),
                    )
                    TextButton(onClick = onReviewPolicy) { Text("Review") }
                }
            }
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                OutlinedTextField(
                    value = body,
                    onValueChange = onBodyChange,
                    placeholder = { Text("Send a message…") },
                    enabled = fieldEnabled,
                    minLines = 1,
                    maxLines = 4,
                    shape = RoundedCornerShape(26.dp),
                    modifier = Modifier.weight(1f).testTag("conversation-body"),
                )
                IconButton(
                    onClick = onSend,
                    enabled = enabled,
                    modifier = Modifier.size(52.dp).testTag("conversation-send"),
                ) {
                    Icon(
                        Icons.AutoMirrored.Filled.Send,
                        contentDescription = if (messageSubmissionInFlight) "Queuing message" else "Send message",
                        tint = if (enabled) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                val assistive = when {
                    messageSubmissionInFlight -> "Waiting for Kitsu to accept the message."
                    !connectionAvailable -> "Offline · connect this Kitsu to send."
                    policyAccepted && body.isNotEmpty() && validation != null -> validation.humanized()
                    channelRouting != null -> "Text only · ${channelRouting.detail}"
                    else -> "Text only · sent over this mesh conversation."
                }
                Text(
                    assistive,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (connectionAvailable) MaterialTheme.colorScheme.onSurfaceVariant
                    else MaterialTheme.colorScheme.error,
                    modifier = Modifier.weight(1f).then(
                        if (channelRouting != null) {
                            Modifier.testTag("conversation-channel-routing")
                        } else {
                            Modifier
                        },
                    ),
                )
                Text(
                    "${MessageComposerPolicy.utf8Bytes(body)}/$MAX_MESSAGE_UTF8_BYTES bytes",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.testTag("conversation-byte-count"),
                )
            }
        }
    }
}

@Composable
private fun SenderAvatar(label: String, route: MessageRoute, size: Dp) {
    Surface(
        shape = CircleShape,
        color = if (route == MessageRoute.CHANNEL) MaterialTheme.colorScheme.tertiaryContainer
        else MaterialTheme.colorScheme.secondaryContainer,
        contentColor = if (route == MessageRoute.CHANNEL) MaterialTheme.colorScheme.onTertiaryContainer
        else MaterialTheme.colorScheme.onSecondaryContainer,
        modifier = Modifier.size(size),
    ) {
        Box(contentAlignment = Alignment.Center) {
            if (route == MessageRoute.CHANNEL) {
                Icon(Icons.Default.Forum, contentDescription = null, modifier = Modifier.size(size * 0.5f))
            } else {
                Text(
                    label.trim().firstOrNull()?.uppercase() ?: "?",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                )
            }
        }
    }
}

@Composable
private fun NewConversationDialog(
    owner: OwnerState,
    blockedPeerIds: Set<String>,
    onSelect: (MessageRoute, String) -> Unit,
    onDismiss: () -> Unit,
) {
    var manualKey by rememberSaveable { mutableStateOf("") }
    val peers = MessageComposerPolicy.contactRecipients(owner.peers.filterNot { it.id in blockedPeerIds })
    val channels = MessageComposerPolicy.channelRecipients(owner.channels)
    val validManual = MeshPeerKeyPolicy.isCanonicalBase64Url(manualKey.trim()) &&
        manualKey.trim() !in blockedPeerIds
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("New conversation") },
        text = {
            Column(
                Modifier.verticalScroll(rememberScrollState()).testTag("new-conversation-content"),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (channels.isNotEmpty()) {
                    Text("Channels", style = MaterialTheme.typography.titleMedium)
                    channels.forEach { recipient ->
                        OutlinedButton(
                            onClick = { onSelect(MessageRoute.CHANNEL, recipient.reference) },
                            modifier = Modifier.fillMaxWidth()
                                .testTag("message-recipient-${recipient.reference}"),
                        ) {
                            Column(Modifier.fillMaxWidth()) {
                                Text(recipient.label)
                                recipient.supporting?.let { supporting ->
                                    Text(
                                        supporting,
                                        style = MaterialTheme.typography.bodyMedium,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                            }
                        }
                    }
                }
                if (peers.isNotEmpty()) {
                    Text("Nearby direct peers", style = MaterialTheme.typography.titleMedium)
                    peers.forEach { recipient ->
                        OutlinedButton(
                            onClick = { onSelect(MessageRoute.DIRECT, recipient.reference) },
                            modifier = Modifier.fillMaxWidth()
                                .testTag("message-recipient-${recipient.reference}"),
                        ) {
                            Column(Modifier.fillMaxWidth()) {
                                Text(recipient.label)
                                Text(
                                    MessageComposerPolicy.compactReference(recipient.reference),
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                }
                if (channels.isEmpty() && peers.isEmpty()) {
                    Text(
                        "No nearby peers or configured channels are available. You can still paste a trusted direct identity below.",
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                HorizontalDivider()
                Text("Trusted public key", style = MaterialTheme.typography.titleMedium)
                OutlinedTextField(
                    value = manualKey,
                    onValueChange = { manualKey = it },
                    label = { Text("43-character peer key") },
                    supportingText = { Text("Direct authenticated mesh identity") },
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.None,
                        autoCorrectEnabled = false,
                        keyboardType = KeyboardType.Ascii,
                    ),
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth().testTag("message-target"),
                )
                Button(
                    onClick = { onSelect(MessageRoute.DIRECT, manualKey.trim()) },
                    enabled = validManual,
                    modifier = Modifier.fillMaxWidth().testTag("start-manual-direct"),
                ) { Text("Open direct conversation") }
                Text(
                    MeshUserPolicy.PROHIBITED_CONTENT,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.testTag("prohibited-content-copy"),
                )
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

/** Compatibility surface used by focused moderation/lifecycle Compose tests. */
@Composable
internal fun MessageCard(
    message: Message,
    peerLabel: String? = null,
    updateBusy: Boolean,
    onReport: (ReportType) -> Unit,
    onBlock: (() -> Unit)?,
) {
    val thread = MessageThread(
        key = MessageThreadPolicy.conversationKey(message) ?: "message:${message.uiStableJournalKey()}",
        route = if (message.channel == null) MessageRoute.DIRECT else MessageRoute.CHANNEL,
        target = message.peerId ?: message.channel.orEmpty(),
        title = when {
            message.channel != null -> message.senderName.takeIf(String::isNotBlank)
                ?.let { "$it · Channel ${message.channel}" }
                ?: "Channel ${message.channel}"
            else -> peerLabel?.takeIf(String::isNotBlank)
                ?: message.senderName.takeIf(String::isNotBlank)
                ?: message.peerId?.let(MessageComposerPolicy::compactReference)
                ?: "Mesh message"
        },
        subtitle = "Message",
        messages = listOf(message),
        unreadCount = if (message.unreadOnKitsu == true) 1 else 0,
        latestJournalPosition = 0,
    )
    MessageBubble(
        message = message,
        thread = thread,
        peers = emptyList(),
        currentJournalSession = message.journalSession,
        updateBusy = updateBusy,
        onReport = onReport,
        onBlock = onBlock,
    )
}

@Composable
internal fun MeshPolicyDialog(
    canAccept: Boolean,
    onAccept: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Mesh messaging terms & user policy") },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState()).testTag("message-policy-content"),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text("Policy ${MeshUserPolicy.VERSION_LABEL}", style = MaterialTheme.typography.titleMedium)
                Text(MeshUserPolicy.TERMS)
                Text("Privacy", style = MaterialTheme.typography.titleMedium)
                Text(MeshUserPolicy.PRIVACY)
            }
        },
        confirmButton = {
            Button(
                onClick = onAccept,
                enabled = canAccept,
                modifier = Modifier.testTag("accept-message-policy"),
            ) { Text("Accept policy") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Not now") } },
    )
}

@Composable
private fun ReportMessageDialog(
    message: Message,
    reportType: ReportType,
    reason: ReportReason,
    note: String,
    onReasonChange: (ReportReason) -> Unit,
    onNoteChange: (String) -> Unit,
    onExport: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            Text(if (reportType == ReportType.SENDER) "Report sender" else "Report message")
        },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState()).testTag("message-report-content"),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    "The report is saved only to the location you choose. It is not submitted automatically.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                ReportReason.entries.forEach { option ->
                    Row(
                        modifier = Modifier.fillMaxWidth().clickable { onReasonChange(option) },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = reason == option, onClick = { onReasonChange(option) })
                        Text(option.label)
                    }
                }
                OutlinedTextField(
                    value = note,
                    onValueChange = {
                        if (it.toByteArray(Charsets.UTF_8).size <= ModerationReport.MAX_NOTE_BYTES) {
                            onNoteChange(it)
                        }
                    },
                    label = { Text("Optional context") },
                    supportingText = { Text("${note.toByteArray(Charsets.UTF_8).size}/${ModerationReport.MAX_NOTE_BYTES} bytes") },
                    minLines = 2,
                    maxLines = 4,
                    modifier = Modifier.fillMaxWidth(),
                )
                Text(
                    "Message: ${message.text}",
                    maxLines = 3,
                    overflow = TextOverflow.Ellipsis,
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
        },
        confirmButton = {
            Button(onClick = onExport, modifier = Modifier.testTag("export-message-report")) {
                Text("Choose export location")
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

private fun Double.compactNumber(): String =
    if (this % 1.0 == 0.0) toInt().toString() else "%.1f".format(this)

@Composable
private fun localizedMessageTimeLabel(epochSeconds: Long): String {
    if (!MessageTimePolicy.isAvailable(epochSeconds)) return ""
    val context = LocalContext.current
    return remember(epochSeconds, context.resources.configuration) {
        DateFormat.getTimeFormat(context).format(Date(epochSeconds * 1_000L))
    }
}

private val ReportReason.label: String
    get() = when (this) {
        ReportReason.SPAM_OR_SCAM -> "Spam or scam"
        ReportReason.HARASSMENT_OR_HATE -> "Harassment or hate"
        ReportReason.ILLEGAL_OR_EXPLOITATIVE -> "Illegal or exploitative content"
        ReportReason.PRIVACY_VIOLATION -> "Privacy violation"
        ReportReason.OTHER -> "Other"
    }

private fun Message.toModerationReport(
    owner: OwnerState,
    reportType: ReportType,
    reason: ReportReason,
    note: String,
): ModerationReport = ModerationReport(
    appId = BuildConfig.APPLICATION_ID,
    appVersion = BuildConfig.VERSION_NAME,
    createdAtEpoch = Instant.now().epochSecond,
    deviceId = owner.status?.deviceId,
    reportType = reportType,
    reason = reason,
    note = note.trim().ifBlank { null },
    messageId = id,
    cursor = cursor,
    direction = direction,
    peerId = peerId,
    channel = channel,
    text = text,
    state = state,
    occurredAt = occurredAt,
)
