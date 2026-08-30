package ptl.kitsu.app

import kotlin.coroutines.CoroutineContext
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import ptl.kitsu.app.security.MessageDraftPolicy
import ptl.kitsu.app.security.MessageDraftRecord
import ptl.kitsu.app.security.MessageDraftStore
import ptl.kitsu.app.security.SafeLog

/** Owns the small in-memory draft projection and coalesces encrypted atomic writes. */
class MessageDraftCoordinator(
    private val store: MessageDraftStore,
    private val scope: CoroutineScope,
    private val ioContext: CoroutineContext = Dispatchers.IO,
    private val persistDelayMillis: Long = 200L,
    private val nowMillis: () -> Long = System::currentTimeMillis,
) {
    private val mutableRecords = MutableStateFlow<List<MessageDraftRecord>>(emptyList())
    val records: StateFlow<List<MessageDraftRecord>> = mutableRecords.asStateFlow()
    private val persistMutex = Mutex()
    private val initialLoadComplete = CompletableDeferred<Unit>()
    private var persistJob: Job? = null
    private var mutationGeneration = 0L

    init {
        require(persistDelayMillis >= 0L) { "invalid_draft_persist_delay" }
        scope.launch {
            try {
                val loaded = withContext(ioContext) { store.read() }
                synchronized(this@MessageDraftCoordinator) {
                    val localBindings = mutableRecords.value.mapTo(mutableSetOf()) { it.bindingKey }
                    mutableRecords.value = MessageDraftPolicy.bounded(
                        loaded.filterNot { it.bindingKey in localBindings } + mutableRecords.value,
                    )
                }
            } finally {
                initialLoadComplete.complete(Unit)
            }
        }
    }

    @Synchronized
    fun update(deviceAddress: String, threadKey: String, text: String) {
        mutableRecords.value = MessageDraftPolicy.upsert(
            records = mutableRecords.value,
            deviceAddress = deviceAddress,
            threadKey = threadKey,
            text = text,
            updatedAtMillis = nowMillis(),
        )
        mutationGeneration += 1L
        schedulePersist(immediate = text.isEmpty())
    }

    fun forDevice(deviceAddress: String?): Map<String, String> =
        MessageDraftPolicy.forDevice(mutableRecords.value, deviceAddress)

    @Synchronized
    private fun schedulePersist(immediate: Boolean) {
        persistJob?.cancel()
        val expectedGeneration = mutationGeneration
        persistJob = scope.launch {
            if (!immediate && persistDelayMillis > 0L) delay(persistDelayMillis)
            // Never replace an unread on-disk snapshot with the first local edit.
            initialLoadComplete.await()
            val snapshot = synchronized(this@MessageDraftCoordinator) {
                if (expectedGeneration != mutationGeneration) return@launch
                mutableRecords.value
            }
            // Once the atomic write starts, let it finish. A newer generation waits on
            // the mutex and writes its newer snapshot immediately afterward.
            withContext(NonCancellable + ioContext) {
                persistMutex.withLock {
                    runCatching { store.write(snapshot) }
                        .onFailure { SafeLog.warn("message_drafts", "message_draft_write_failed", it) }
                }
            }
        }
    }
}
