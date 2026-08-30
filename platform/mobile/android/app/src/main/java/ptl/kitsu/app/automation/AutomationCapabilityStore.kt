package ptl.kitsu.app.automation

import android.content.Context
import java.io.FileOutputStream
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64

class AutomationCapabilityStore(context: Context) {
    private val file = context.noBackupFilesDir.resolve(FILE_NAME)

    @Synchronized
    fun enabledToken(): String? = runCatching {
        if (!file.isFile || file.length() !in 1..128) return@runCatching null
        file.readText(Charsets.US_ASCII).takeIf(TOKEN_PATTERN::matches)
    }.getOrNull()

    @Synchronized
    fun enable(): String {
        enabledToken()?.let { return it }
        val token = Base64.getUrlEncoder().withoutPadding().encodeToString(
            ByteArray(32).also(SecureRandom()::nextBytes),
        )
        check(TOKEN_PATTERN.matches(token)) { "automation_capability_generation_failed" }
        val temporary = file.resolveSibling("$FILE_NAME.tmp")
        temporary.delete()
        FileOutputStream(temporary).use { output ->
            output.write(token.toByteArray(Charsets.US_ASCII))
            output.fd.sync()
        }
        check(temporary.renameTo(file)) { "automation_capability_write_failed" }
        return token
    }

    @Synchronized
    fun disable() {
        if (file.exists() && !file.delete()) error("automation_capability_delete_failed")
        file.resolveSibling("$FILE_NAME.tmp").delete()
    }

    fun accepts(candidate: String): Boolean {
        val expected = enabledToken() ?: return false
        return MessageDigest.isEqual(
            expected.toByteArray(Charsets.US_ASCII),
            candidate.toByteArray(Charsets.US_ASCII),
        )
    }

    companion object {
        private const val FILE_NAME = "automation-capability-v1.txt"
        private val TOKEN_PATTERN = Regex("^[A-Za-z0-9_-]{43}$")
    }
}
