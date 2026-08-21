package app.kitsu.mobile.security

import android.util.Log

object SafeLog {
    private val sensitive = Regex(
        "token|claim|password|passphrase|secret|authorization|cookie|credential|pairing|" +
            "code_verifier|ssid|certificate|spki|private",
        RegexOption.IGNORE_CASE,
    )

    fun info(event: String, fields: Map<String, Any?> = emptyMap()) {
        Log.i("Kitsu", render(event, fields))
    }

    fun warn(event: String, code: String, throwable: Throwable? = null) {
        val safeType = throwable?.javaClass?.simpleName ?: "none"
        Log.w("Kitsu", "$event code=$code cause=$safeType")
    }

    internal fun render(event: String, fields: Map<String, Any?>): String = buildString {
        append(event.take(80).replace(Regex("[\\r\\n]"), "_"))
        fields.toSortedMap().forEach { (key, value) ->
            append(' ')
            append(key.replace(Regex("[^A-Za-z0-9_.-]"), "_"))
            append('=')
            append(if (sensitive.containsMatchIn(key)) "<redacted>" else sanitize(value))
        }
    }

    private fun sanitize(value: Any?): String = when (value) {
        null -> "null"
        is Number, is Boolean -> value.toString()
        else -> value.toString().take(96).replace(Regex("[\\r\\n\\t]"), "_")
    }
}
