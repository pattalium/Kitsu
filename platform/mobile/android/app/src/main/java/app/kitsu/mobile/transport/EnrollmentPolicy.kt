package app.kitsu.mobile.transport

import app.kitsu.mobile.model.GatewayEnrollmentBeginBody
import app.kitsu.mobile.model.GatewayEnrollmentFinishBody
import java.util.Base64
import java.util.UUID

internal object EnrollmentPolicy {
    private const val BEGIN_SCHEMA = "kitsu.gateway-enrollment.begin.v1"
    private const val FINISH_SCHEMA = "kitsu.gateway-enrollment.finish.v1"
    private val hardwareUid = Regex("^[A-Za-z0-9_.:-]{4,128}$")

    fun ownerCreateError(hardwareUid: String, displayName: String): String? = when {
        !this.hardwareUid.matches(hardwareUid) -> "invalid_hardware_uid"
        displayName.trim() != displayName || displayName.toByteArray(Charsets.UTF_8).size !in 1..80 ||
            displayName.any(Char::isISOControl) -> "invalid_display_name"
        else -> null
    }

    fun beginError(request: GatewayEnrollmentBeginBody): String? = when {
        request.schema != BEGIN_SCHEMA -> "invalid_request"
        !canonicalUuid(request.enrollmentId) -> "invalid_request"
        !canonicalBase64Url(request.claimToken, 32) -> "invalid_request"
        else -> null
    }

    fun finishError(request: GatewayEnrollmentFinishBody): String? = when {
        request.schema != FINISH_SCHEMA -> "invalid_request"
        !canonicalUuid(request.enrollmentId) -> "invalid_request"
        else -> null
    }

    fun canonicalUuid(value: String): Boolean = runCatching {
        value.length == 36 && value.none(Char::isUpperCase) &&
            UUID.fromString(value).toString() == value && UUID.fromString(value) != UUID(0, 0)
    }.getOrDefault(false)

    fun canonicalBase64Url(value: String, bytes: Int): Boolean = runCatching {
        value.isNotEmpty() && '=' !in value && value.none(Char::isWhitespace) &&
            Base64.getUrlDecoder().decode(value).let { decoded ->
                decoded.size == bytes && Base64.getUrlEncoder().withoutPadding()
                    .encodeToString(decoded) == value
            }
    }.getOrDefault(false)
}
