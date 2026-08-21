package app.kitsu.mobile.security

import android.content.Context
import app.kitsu.mobile.transport.RemoteCompanionSelectionStore

/** Companion selection is non-secret metadata; downloaded owner data remains encrypted separately. */
class AndroidRemoteCompanionSelectionStore(context: Context) : RemoteCompanionSelectionStore {
    private val preferences = context.getSharedPreferences("kitsu_owner_selection", Context.MODE_PRIVATE)

    override fun selectedCompanionId(): String? = preferences.getString(KEY, null)

    override fun saveSelectedCompanionId(value: String?) {
        val edit = preferences.edit()
        if (value == null) edit.remove(KEY) else edit.putString(KEY, value)
        check(edit.commit()) { "selection_write_failed" }
    }

    private companion object { const val KEY = "remote_companion_id" }
}
