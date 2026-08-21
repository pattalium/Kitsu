package app.kitsu.mobile.transport

import app.kitsu.mobile.model.OwnerEnrollmentChallenge
import app.kitsu.mobile.model.OwnerEnrollmentView
import app.kitsu.mobile.relay.DeviceRelayAuthorization
import app.kitsu.mobile.relay.DeviceRelayHttpRoutes
import app.kitsu.mobile.relay.MAX_RELAY_ENROLLMENT_REQUEST_BYTES
import app.kitsu.mobile.relay.MAX_RELAY_FRAME_BYTES
import app.kitsu.mobile.relay.MobileRelayBackend
import app.kitsu.mobile.relay.MobileRelayBindingRequest
import app.kitsu.mobile.relay.MobileRelayClaimResponse
import app.kitsu.mobile.relay.MobileRelayEnvelopeAccepted
import app.kitsu.mobile.relay.MobileRelayIdentity
import app.kitsu.mobile.relay.MobileRelayWirePolicy
import app.kitsu.mobile.security.CredentialStore
import java.io.IOException
import java.time.Instant
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.withContext
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import okhttp3.HttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString

/** Account-free relay transport. Its scoped token is never accepted by owner routes. */
class DeviceRelayTransport(
    private val configuration: BackendConfiguration,
    private val credentials: CredentialStore,
    private val routes: DeviceRelayHttpRoutes = DeviceRelayHttpRoutes(),
    private val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .build(),
) : MobileRelayBackend {
    private val json = Json { ignoreUnknownKeys = true; explicitNulls = false }

    override suspend fun ensureRelay(installationId: String, gatewayId: String): MobileRelayIdentity {
        if (!MobileRelayWirePolicy.canonicalUuid(installationId) ||
            !MobileRelayWirePolicy.canonicalUuid(gatewayId)
        ) throw TransportException("invalid_mobile_relay_identity")
        val identity: MobileRelayIdentity = put(
            installationId,
            route(routes.binding, installationId),
            json.encodeToString(MobileRelayBindingRequest(gatewayId)),
        )
        if (identity.installationId != installationId || identity.gatewayId != gatewayId ||
            runCatching { Instant.parse(identity.createdAt) }.getOrNull() == null ||
            runCatching {
                MobileRelayWirePolicy.decodeCanonical(identity.caCertificateDerB64, 8 * 1024)
                    .isNotEmpty()
            }.getOrDefault(false).not()
        ) throw TransportException("mobile_relay_binding_failed")
        return identity
    }

    override suspend fun createEnrollment(
        installationId: String,
        hardwareUid: String,
        displayName: String,
    ): OwnerEnrollmentChallenge {
        if (!MobileRelayWirePolicy.canonicalUuid(installationId)) {
            throw TransportException("invalid_mobile_relay_identity")
        }
        EnrollmentPolicy.ownerCreateError(hardwareUid, displayName)?.let { throw TransportException(it) }
        val response: CreateEnrollmentResponse = post(
            installationId,
            route(routes.enrollments, installationId),
            json.encodeToString(CreateEnrollmentRequest(hardwareUid, displayName)),
        )
        val enrollment = response.enrollment
        if (!MobileRelayWirePolicy.canonicalUuid(enrollment.id) ||
            enrollment.hardwareUid != hardwareUid || enrollment.displayName != displayName ||
            enrollment.status != "pending" ||
            runCatching { Instant.parse(enrollment.expiresAt) }.getOrNull() == null ||
            runCatching {
                MobileRelayWirePolicy.decodeCanonical(response.claimToken, 32).size == 32
            }.getOrDefault(false).not()
        ) throw TransportException("malformed_enrollment_response")
        return OwnerEnrollmentChallenge(enrollment, response.claimToken)
    }

    override suspend fun claimEnrollment(
        installationId: String,
        enrollmentId: String,
        exactRequest: ByteArray,
    ): ByteArray {
        if (!MobileRelayWirePolicy.canonicalUuid(installationId) ||
            !MobileRelayWirePolicy.canonicalUuid(enrollmentId) ||
            exactRequest.isEmpty() || exactRequest.size > MAX_RELAY_ENROLLMENT_REQUEST_BYTES
        ) throw TransportException("invalid_mobile_relay_claim")
        val response = postExact(
            installationId,
            route(routes.claim, installationId, enrollmentId),
            exactRequest,
            existingEnrollmentConflictRequiresReset = true,
        )
        val parsed = runCatching {
            json.decodeFromString(MobileRelayClaimResponse.serializer(), response.toString(Charsets.UTF_8))
        }.getOrElse { throw TransportException("malformed_enrollment_response", it) }
        if (!MobileRelayWirePolicy.canonicalUuid(parsed.companionId) ||
            !MobileRelayWirePolicy.canonicalUuid(parsed.gatewayId) || parsed.keyVersion <= 0
        ) throw TransportException("malformed_enrollment_response")
        return response
    }

    override suspend fun uploadEnvelope(
        installationId: String,
        spoolRecordId: String,
        exactEnvelope: ByteArray,
    ): ByteArray {
        if (!MobileRelayWirePolicy.canonicalUuid(installationId) ||
            !MobileRelayWirePolicy.canonicalU64(spoolRecordId) ||
            exactEnvelope.isEmpty() || exactEnvelope.size > MAX_RELAY_FRAME_BYTES
        ) throw TransportException("invalid_mobile_relay_envelope")
        val response = postExact(
            installationId,
            route(routes.envelopes, installationId),
            exactEnvelope,
            mapOf("X-Kitsu-Spool-Record-Id" to spoolRecordId),
        )
        val accepted = runCatching {
            json.decodeFromString(MobileRelayEnvelopeAccepted.serializer(), response.toString(Charsets.UTF_8))
        }.getOrElse { throw TransportException("malformed_envelope_ack", it) }
        if (!accepted.accepted || accepted.spoolRecordId != spoolRecordId ||
            accepted.spoolRecordId == "0" || accepted.sequence == "0" ||
            !MobileRelayWirePolicy.canonicalU64(accepted.sequence)
        ) throw TransportException("malformed_envelope_ack")
        return MobileRelayWirePolicy.gatewayAcknowledgement(accepted.spoolRecordId, accepted.sequence)
    }

    override fun downlinks(installationId: String): Flow<ByteArray> = callbackFlow {
        if (!MobileRelayWirePolicy.canonicalUuid(installationId)) {
            close(TransportException("invalid_mobile_relay_identity"))
            return@callbackFlow
        }
        val token = runCatching { credential(installationId) }.getOrElse {
            close(it)
            return@callbackFlow
        }
        val request = Request.Builder()
            .url(endpoint(route(routes.session, installationId)).newBuilder().scheme("wss").build())
            .header(AUTHORIZATION, DeviceRelayAuthorization.headerValue(token))
            .build()
        val socket = client.newWebSocket(request, object : WebSocketListener() {
            override fun onMessage(webSocket: WebSocket, text: String) =
                accept(webSocket, text.toByteArray())

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) =
                accept(webSocket, bytes.toByteArray())

            private fun accept(webSocket: WebSocket, bytes: ByteArray) {
                if (bytes.isEmpty() || bytes.size > MAX_RELAY_FRAME_BYTES) {
                    webSocket.close(1009, "relay_downlink_size")
                    close(TransportException("relay_downlink_size"))
                } else if (!trySend(bytes).isSuccess) {
                    webSocket.close(1011, "relay_queue_unavailable")
                    close(TransportException("relay_queue_unavailable"))
                }
            }

            override fun onFailure(webSocket: WebSocket, failure: Throwable, response: Response?) {
                close(TransportException("device_relay_session_failed", failure))
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                close()
            }
        })
        awaitClose { socket.close(1000, "relay_stop") }
    }

    private suspend inline fun <reified T> post(
        installationId: String,
        route: String,
        body: String,
    ): T = execute(
        installationId,
        Request.Builder().url(endpoint(route)).post(body.toRequestBody(JSON_MEDIA_TYPE)),
    )

    private suspend inline fun <reified T> put(
        installationId: String,
        route: String,
        body: String,
    ): T = execute(
        installationId,
        Request.Builder().url(endpoint(route)).put(body.toRequestBody(JSON_MEDIA_TYPE)),
    )

    private suspend fun postExact(
        installationId: String,
        route: String,
        body: ByteArray,
        headers: Map<String, String> = emptyMap(),
        existingEnrollmentConflictRequiresReset: Boolean = false,
    ): ByteArray = executeRaw(
        installationId,
        Request.Builder()
            .url(endpoint(route))
            .apply { headers.forEach { (name, value) -> header(name, value) } }
            .post(body.toRequestBody(JSON_MEDIA_TYPE)),
        existingEnrollmentConflictRequiresReset,
    )

    private suspend inline fun <reified T> execute(
        installationId: String,
        builder: Request.Builder,
    ): T {
        val request = authorize(installationId, builder)
        return withContext(Dispatchers.IO) {
            val response = try {
                client.newCall(request).execute()
            } catch (failure: IOException) {
                throw TransportException("backend_unavailable", failure)
            }
            response.use {
                val body = boundedBodyBytes(it)
                if (it.code == 401) throw TransportException("relay_credential_rejected")
                if (!it.isSuccessful) throw backendFailure(it.code, body)
                runCatching { json.decodeFromString<T>(body.toString(Charsets.UTF_8)) }
                    .getOrElse { failure -> throw TransportException("malformed_response", failure) }
            }
        }
    }

    private suspend fun executeRaw(
        installationId: String,
        builder: Request.Builder,
        existingEnrollmentConflictRequiresReset: Boolean = false,
    ): ByteArray {
        val request = authorize(installationId, builder)
        return withContext(Dispatchers.IO) {
            val response = try {
                client.newCall(request).execute()
            } catch (failure: IOException) {
                throw TransportException("backend_unavailable", failure)
            }
            response.use {
                val body = boundedBodyBytes(it)
                if (it.code == 401) throw TransportException("relay_credential_rejected")
                if (!it.isSuccessful) {
                    throw backendFailure(it.code, body, existingEnrollmentConflictRequiresReset)
                }
                body
            }
        }
    }

    private suspend fun authorize(installationId: String, builder: Request.Builder): Request =
        builder.header(
            AUTHORIZATION,
            DeviceRelayAuthorization.headerValue(credential(installationId)),
        ).build()

    private suspend fun credential(installationId: String): String {
        val settings = credentials.mobileRelaySettings()
            ?: throw TransportException("relay_credential_unavailable")
        val token = settings.relayCredentialB64
        if (settings.installationId != installationId ||
            !MobileRelayWirePolicy.canonicalRelayCredential(token)
        ) throw TransportException("relay_credential_unavailable")
        return token!!
    }

    private fun backendFailure(
        status: Int,
        body: ByteArray,
        existingEnrollmentConflictRequiresReset: Boolean = false,
    ): TransportException {
        val error = runCatching {
            json.decodeFromString<ApiErrorEnvelope>(body.toString(Charsets.UTF_8)).error
        }.getOrNull()
        val code = if (existingEnrollmentConflictRequiresReset && status == 409 &&
            error?.code == "conflict" &&
            error.message == "record already exists"
        ) {
            "existing_gateway_enrollment_requires_reset"
        } else {
            error?.code ?: "http_$status"
        }
        return TransportException(code)
    }

    private fun boundedBodyBytes(response: Response): ByteArray {
        val body = response.body ?: throw TransportException("empty_response")
        if (body.contentLength() > MAX_RESPONSE_BYTES) throw TransportException("response_too_large")
        val source = body.source()
        source.request(MAX_RESPONSE_BYTES + 1L)
        if (source.buffer.size > MAX_RESPONSE_BYTES) throw TransportException("response_too_large")
        return source.readByteArray()
    }

    private fun route(
        template: String,
        installationId: String,
        enrollmentId: String? = null,
    ): String {
        if (!MobileRelayWirePolicy.canonicalUuid(installationId) ||
            enrollmentId?.let { !MobileRelayWirePolicy.canonicalUuid(it) } == true
        ) throw TransportException("invalid_mobile_relay_identity")
        var value = template.replace("{installation_id}", installationId)
        if (enrollmentId != null) value = value.replace("{enrollment_id}", enrollmentId)
        if (value.contains('{') || value.contains('}')) {
            throw TransportException("invalid_mobile_relay_route")
        }
        return value
    }

    private fun endpoint(route: String): HttpUrl {
        if (!route.startsWith('/') || route.startsWith("//")) {
            throw IllegalArgumentException("invalid_backend_route")
        }
        val resolved = configuration.parsedBaseUrl.resolve(route)
            ?: throw IllegalArgumentException("invalid_backend_route")
        require(
            resolved.scheme == configuration.parsedBaseUrl.scheme &&
                resolved.host == configuration.parsedBaseUrl.host &&
                resolved.port == configuration.parsedBaseUrl.port,
        ) { "invalid_backend_route" }
        return resolved
    }

    @Serializable
    private data class CreateEnrollmentRequest(
        @SerialName("hardware_uid") val hardwareUid: String,
        @SerialName("display_name") val displayName: String,
    )

    @Serializable
    private data class CreateEnrollmentResponse(
        val enrollment: OwnerEnrollmentView,
        @SerialName("claim_token") val claimToken: String,
    )

    @Serializable
    private data class ApiErrorBody(
        val code: String = "request_failed",
        val message: String = "request_failed",
    )

    @Serializable
    private data class ApiErrorEnvelope(val error: ApiErrorBody? = null)

    private companion object {
        const val AUTHORIZATION = "Authorization"
        val JSON_MEDIA_TYPE = "application/json; charset=utf-8".toMediaType()
        const val MAX_RESPONSE_BYTES = 1024 * 1024L
    }
}
