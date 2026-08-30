package ptl.kitsu.app.walk

import android.content.Context
import android.content.pm.PackageManager
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.AtomicFile
import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.io.IOException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import ptl.kitsu.app.model.PetFeaturePolicy

/** Android TYPE_STEP_COUNTER source with durable, no-backup board-and-route baselines. */
class AndroidWalkStepSource(context: Context) : WalkStepSource, SensorEventListener {
    private val appContext = context.applicationContext ?: context
    private val sensorManager = appContext.getSystemService(SensorManager::class.java)
    private val stepCounter = sensorManager?.getDefaultSensor(Sensor.TYPE_STEP_COUNTER)
    private val checkpointStore = AndroidNoBackupWalkStepStore(appContext.noBackupFilesDir)
    private val lock = Any()

    private val initialLoad = checkpointStore.load()
    private var ledger = WalkStepLedger(
        persisted = (initialLoad as? WalkStepCheckpointLoad.Loaded)?.state
            ?: WalkStepCheckpointSet(),
    )
    private var storageErrorCode: String? = when (initialLoad) {
        WalkStepCheckpointLoad.Corrupt -> CHECKPOINT_CORRUPT
        WalkStepCheckpointLoad.Empty,
        is WalkStepCheckpointLoad.Loaded -> null
    }
    private var operationalErrorCode: String? = null
    private var availability = initialAvailability()
    private var observing = false
    private var closed = false
    private var sensorThread: HandlerThread? = null
    private var sensorHandler: Handler? = null

    private val mutableSnapshots = MutableStateFlow(snapshotLocked())
    override val snapshots: StateFlow<WalkStepSnapshot> = mutableSnapshots.asStateFlow()

    override fun startObserving(): WalkStepSnapshot = synchronized(lock) {
        if (closed) return@synchronized publishLocked()
        val refreshedAvailability = initialAvailability()
        if (observing && refreshedAvailability != WalkStepAvailability.AVAILABLE) {
            stopSensorLocked()
        }
        availability = refreshedAvailability
        operationalErrorCode = when (availability) {
            WalkStepAvailability.SENSOR_UNAVAILABLE -> SENSOR_UNAVAILABLE
            else -> null
        }
        if (availability != WalkStepAvailability.AVAILABLE || observing) {
            return@synchronized publishLocked()
        }

        val manager = sensorManager ?: return@synchronized unavailableLocked()
        val sensor = stepCounter ?: return@synchronized unavailableLocked()
        val thread = HandlerThread(SENSOR_THREAD_NAME).also { it.start() }
        val handler = Handler(thread.looper)
        val registered = try {
            manager.registerListener(
                this,
                sensor,
                SensorManager.SENSOR_DELAY_NORMAL,
                handler,
            )
        } catch (_: SecurityException) {
            availability = WalkStepAvailability.PERMISSION_REQUIRED
            operationalErrorCode = null
            false
        } catch (_: Throwable) {
            availability = WalkStepAvailability.REGISTRATION_FAILED
            operationalErrorCode = REGISTRATION_FAILED
            false
        }
        if (registered) {
            sensorThread = thread
            sensorHandler = handler
            observing = true
            availability = WalkStepAvailability.AVAILABLE
            operationalErrorCode = null
        } else {
            thread.quitSafely()
            if (availability == WalkStepAvailability.AVAILABLE) {
                availability = WalkStepAvailability.REGISTRATION_FAILED
                operationalErrorCode = REGISTRATION_FAILED
            }
        }
        publishLocked()
    }

    override fun stopObserving(): WalkStepSnapshot = synchronized(lock) {
        stopSensorLocked()
        if (!closed) {
            availability = initialAvailability()
            operationalErrorCode = when (availability) {
                WalkStepAvailability.SENSOR_UNAVAILABLE -> SENSOR_UNAVAILABLE
                else -> null
            }
        }
        publishLocked()
    }

    override fun selectDevice(deviceAddress: String): WalkStepSnapshot = synchronized(lock) {
        requireOpenLocked()
        val transition = try {
            ledger.selectDevice(deviceAddress)
        } catch (failure: IllegalArgumentException) {
            throw WalkStepSourceException(INVALID_DEVICE_ADDRESS, failure)
        }
        if (transition.storageChanged) {
            try {
                checkpointStore.save(transition.next.persisted)
            } catch (failure: Throwable) {
                // Runtime isolation wins over stale persistence: stop crediting the old board now.
                ledger = transition.next
                storageErrorCode = STORAGE_FAILED
                publishLocked()
                throw WalkStepSourceException(STORAGE_FAILED, failure)
            }
        }
        ledger = transition.next
        storageErrorCode = null
        publishLocked()
    }

    override fun bindRoute(
        deviceAddress: String,
        routeId: Long,
        firmwareStepsTotal: Long,
    ): WalkStepSnapshot =
        synchronized(lock) {
            requireOpenLocked()
            if (!PetFeaturePolicy.validOperationId(routeId)) {
                throw WalkStepSourceException("invalid_walk_route")
            }
            if (firmwareStepsTotal !in 0L..PetFeaturePolicy.MAX_WALK_STEPS) {
                throw WalkStepSourceException("invalid_walk_steps_total")
            }
            val transition = try {
                ledger.bindRoute(deviceAddress, routeId, firmwareStepsTotal)
            } catch (failure: IllegalArgumentException) {
                if (failure.message == "invalid_walk_device_address") {
                    throw WalkStepSourceException(INVALID_DEVICE_ADDRESS, failure)
                }
                throw failure
            }
            if (transition.storageChanged) {
                try {
                    checkpointStore.save(transition.next.persisted)
                } catch (failure: Throwable) {
                    // A device switch must take effect in memory even if durability is degraded,
                    // otherwise the old board would continue receiving this phone's steps.
                    if (transition.deviceRebased) ledger = transition.next
                    storageErrorCode = STORAGE_FAILED
                    publishLocked()
                    throw WalkStepSourceException(STORAGE_FAILED, failure)
                }
            }
            ledger = transition.next
            storageErrorCode = null
            publishLocked()
        }

    override fun clearRoute(deviceAddress: String, routeId: Long): WalkStepSnapshot = synchronized(lock) {
        requireOpenLocked()
        if (!PetFeaturePolicy.validOperationId(routeId)) {
            throw WalkStepSourceException("invalid_walk_route")
        }
        val transition = try {
            ledger.clearRoute(deviceAddress, routeId)
        } catch (failure: IllegalArgumentException) {
            if (failure.message == "invalid_walk_device_address") {
                throw WalkStepSourceException(INVALID_DEVICE_ADDRESS, failure)
            }
            throw failure
        }
        if (transition.storageChanged) {
            try {
                if (transition.next.persisted.checkpoints.isEmpty()) {
                    checkpointStore.clear()
                } else {
                    checkpointStore.save(transition.next.persisted)
                }
            } catch (failure: Throwable) {
                // Never keep crediting a route which the caller definitively completed.
                ledger = transition.next
                storageErrorCode = STORAGE_FAILED
                publishLocked()
                throw WalkStepSourceException(STORAGE_FAILED, failure)
            }
            ledger = transition.next
            storageErrorCode = null
        }
        publishLocked()
    }

    override fun clearDevice(deviceAddress: String): WalkStepSnapshot = synchronized(lock) {
        requireOpenLocked()
        val transition = try {
            ledger.clearDevice(deviceAddress)
        } catch (failure: IllegalArgumentException) {
            throw WalkStepSourceException(INVALID_DEVICE_ADDRESS, failure)
        }
        if (transition.storageChanged) {
            try {
                if (transition.next.persisted.checkpoints.isEmpty()) {
                    checkpointStore.clear()
                } else {
                    checkpointStore.save(transition.next.persisted)
                }
            } catch (failure: Throwable) {
                ledger = transition.next
                storageErrorCode = STORAGE_FAILED
                publishLocked()
                throw WalkStepSourceException(STORAGE_FAILED, failure)
            }
            ledger = transition.next
            storageErrorCode = null
        }
        publishLocked()
    }

    override fun close() {
        synchronized(lock) {
            if (closed) return
            stopSensorLocked()
            closed = true
            availability = WalkStepAvailability.CLOSED
            operationalErrorCode = null
            publishLocked()
        }
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (event.sensor.type != Sensor.TYPE_STEP_COUNTER || event.values.isEmpty()) return
        val value = event.values[0]
        if (!value.isFinite() || value < 0f) {
            synchronized(lock) {
                if (!closed && observing) {
                    operationalErrorCode = INVALID_SAMPLE
                    publishLocked()
                }
            }
            return
        }
        val counter = value.toLong()
        synchronized(lock) {
            if (closed || !observing) return
            val transition = ledger.observeCounter(counter)
            operationalErrorCode = null
            if (!transition.storageChanged) {
                ledger = transition.next
                publishLocked()
                return
            }
            try {
                checkpointStore.save(transition.next.persisted)
            } catch (_: Throwable) {
                // Keep the durable baseline untouched. The next valid event will retry
                // the entire uncommitted delta, while a new route can still baseline at
                // the most recently observed phone counter.
                ledger = ledger.copy(latestCounter = counter)
                storageErrorCode = STORAGE_FAILED
                publishLocked()
                return
            }
            ledger = transition.next
            storageErrorCode = null
            publishLocked()
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    private fun requireOpenLocked() {
        if (closed) throw WalkStepSourceException(SOURCE_CLOSED)
    }

    private fun initialAvailability(): WalkStepAvailability = when {
        sensorManager == null || stepCounter == null -> WalkStepAvailability.SENSOR_UNAVAILABLE
        permissionRequired() -> WalkStepAvailability.PERMISSION_REQUIRED
        else -> WalkStepAvailability.AVAILABLE
    }

    private fun permissionRequired(): Boolean {
        val granted = Build.VERSION.SDK_INT < Build.VERSION_CODES.Q ||
            appContext.checkSelfPermission(
                WalkStepPermissionPolicy.ACTIVITY_RECOGNITION_PERMISSION,
            ) == PackageManager.PERMISSION_GRANTED
        return WalkStepPermissionPolicy.permissionRequired(Build.VERSION.SDK_INT, granted)
    }

    private fun unavailableLocked(): WalkStepSnapshot {
        availability = WalkStepAvailability.SENSOR_UNAVAILABLE
        operationalErrorCode = SENSOR_UNAVAILABLE
        return publishLocked()
    }

    private fun stopSensorLocked() {
        if (observing) sensorManager?.unregisterListener(this)
        observing = false
        sensorHandler = null
        sensorThread?.quitSafely()
        sensorThread = null
    }

    private fun publishLocked(): WalkStepSnapshot = snapshotLocked().also {
        mutableSnapshots.value = it
    }

    private fun snapshotLocked(): WalkStepSnapshot = WalkStepSnapshot(
        availability = availability,
        observing = observing,
        sensorBaselineReady = ledger.hasSafeCounterBaseline,
        deviceAddress = ledger.selectedDeviceAddress,
        routeId = ledger.activeCheckpoint?.routeId,
        stepsTotal = ledger.activeCheckpoint?.stepsTotal ?: 0L,
        requiredPermission = WalkStepPermissionPolicy.ACTIVITY_RECOGNITION_PERMISSION.takeIf {
            availability == WalkStepAvailability.PERMISSION_REQUIRED
        },
        errorCode = storageErrorCode ?: operationalErrorCode,
    )

    private companion object {
        const val SENSOR_THREAD_NAME = "kitsu-walk-steps"
        const val SENSOR_UNAVAILABLE = "step_counter_unavailable"
        const val REGISTRATION_FAILED = "step_counter_registration_failed"
        const val INVALID_SAMPLE = "invalid_step_counter_sample"
        const val CHECKPOINT_CORRUPT = "walk_step_checkpoint_corrupt"
        const val STORAGE_FAILED = "walk_step_storage_failed"
        const val SOURCE_CLOSED = "walk_step_source_closed"
        const val INVALID_DEVICE_ADDRESS = "invalid_walk_device_address"
    }
}

internal sealed interface WalkStepCheckpointLoad {
    data object Empty : WalkStepCheckpointLoad
    data object Corrupt : WalkStepCheckpointLoad
    data class Loaded(val state: WalkStepCheckpointSet) : WalkStepCheckpointLoad
}

internal interface WalkStepCheckpointStore {
    fun load(): WalkStepCheckpointLoad
    fun save(state: WalkStepCheckpointSet)
    fun clear()
}

internal class AndroidNoBackupWalkStepStore(directory: File) : WalkStepCheckpointStore {
    private val checkpointFile = AtomicFile(File(directory, FILE_NAME))

    override fun load(): WalkStepCheckpointLoad {
        val bytes = try {
            checkpointFile.openRead().use { input ->
                val bounded = ByteArray(WalkStepCheckpointCodec.ENCODED_MAX_BYTES + 1)
                var offset = 0
                while (offset < bounded.size) {
                    val count = input.read(bounded, offset, bounded.size - offset)
                    if (count < 0) break
                    if (count == 0) return WalkStepCheckpointLoad.Corrupt
                    offset += count
                }
                if (offset == bounded.size || input.read() != -1) {
                    return WalkStepCheckpointLoad.Corrupt
                }
                bounded.copyOf(offset)
            }
        } catch (_: FileNotFoundException) {
            return WalkStepCheckpointLoad.Empty
        } catch (_: Throwable) {
            return WalkStepCheckpointLoad.Corrupt
        }
        val state = WalkStepCheckpointCodec.decode(bytes)
            ?: return WalkStepCheckpointLoad.Corrupt
        return WalkStepCheckpointLoad.Loaded(state)
    }

    override fun save(state: WalkStepCheckpointSet) {
        val bytes = WalkStepCheckpointCodec.encode(state)
        var output: FileOutputStream? = null
        try {
            output = checkpointFile.startWrite()
            output.write(bytes)
            checkpointFile.finishWrite(output)
        } catch (failure: Throwable) {
            output?.let(checkpointFile::failWrite)
            throw IOException("walk step checkpoint write failed", failure)
        }
    }

    override fun clear() {
        checkpointFile.delete()
    }

    private companion object {
        const val FILE_NAME = "walk-step-source-v2.bin"
    }
}
