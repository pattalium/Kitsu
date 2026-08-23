package ptl.kitsu.app.connection

import android.content.Context
import android.content.SharedPreferences

/**
 * Stores only the owner's non-secret "stay disconnected" choice.
 *
 * Reads are deliberately synchronous: [ConnectionCoordinator] consumes this value in its
 * constructor, before MainViewModel can schedule the cold-start automatic connection attempt.
 */
internal interface ReconnectSuppressionStore {
    fun readSuppressed(): Boolean
    fun writeSuppressed(suppressed: Boolean): Boolean
}

internal class InMemoryReconnectSuppressionStore(
    initialValue: Boolean = false,
) : ReconnectSuppressionStore {
    @Volatile private var suppressed = initialValue

    override fun readSuppressed(): Boolean = suppressed

    override fun writeSuppressed(suppressed: Boolean): Boolean {
        this.suppressed = suppressed
        return true
    }
}

internal class AndroidReconnectSuppressionStore(
    private val preferences: SharedPreferences,
) : ReconnectSuppressionStore {
    constructor(context: Context) : this(
        context.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE),
    )

    private val lock = Any()

    override fun readSuppressed(): Boolean = synchronized(lock) {
        val values = preferences.all
        val version = values[KEY_SCHEMA_VERSION] as? Int ?: 0
        when (version) {
            0 -> {
                // 1.0.1 and earlier had no durable choice. A fresh/legacy install therefore
                // preserves the historical default (automatic connection allowed). If an
                // unversioned build already wrote the boolean, retain that owner's choice.
                val migrated = values[KEY_SUPPRESSED] as? Boolean ?: false
                preferences.edit()
                    .putInt(KEY_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION)
                    .putBoolean(KEY_SUPPRESSED, migrated)
                    .commit()
                migrated
            }
            CURRENT_SCHEMA_VERSION -> values[KEY_SUPPRESSED] as? Boolean ?: false
            else -> {
                // A downgraded app must not reinterpret a newer preference schema by silently
                // reconnecting. Fail closed and leave the unknown data untouched.
                true
            }
        }
    }

    override fun writeSuppressed(suppressed: Boolean): Boolean = synchronized(lock) {
        preferences.edit()
            .putInt(KEY_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION)
            .putBoolean(KEY_SUPPRESSED, suppressed)
            .commit()
    }

    internal companion object {
        const val PREFERENCES_NAME = "kitsu_connection_preferences"
        const val KEY_SCHEMA_VERSION = "schema_version"
        const val KEY_SUPPRESSED = "automatic_reconnect_suppressed"
        const val CURRENT_SCHEMA_VERSION = 1
    }
}
