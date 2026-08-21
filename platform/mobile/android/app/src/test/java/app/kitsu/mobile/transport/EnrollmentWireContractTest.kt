package app.kitsu.mobile.transport

import app.kitsu.mobile.model.GatewayEnrollmentBeginBody
import app.kitsu.mobile.model.GatewayEnrollmentFinishBody
import java.util.Base64
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class EnrollmentWireContractTest {
    private val json = Json { explicitNulls = false; encodeDefaults = true }
    private val enrollmentId = "00112233-4455-6677-8899-aabbccddeeff"
    private val claimToken = Base64.getUrlEncoder().withoutPadding().encodeToString(ByteArray(32) { 7 })

    @Test fun beginAndFinishBodiesHaveOnlyFrozenFields() {
        val begin = json.parseToJsonElement(
            json.encodeToString(GatewayEnrollmentBeginBody(enrollmentId = enrollmentId, claimToken = claimToken)),
        ).jsonObject
        assertEquals(setOf("schema", "enrollment_id", "claim_token"), begin.keys)
        assertEquals("kitsu.gateway-enrollment.begin.v1", begin.getValue("schema").jsonPrimitive.content)
        assertEquals(enrollmentId, begin.getValue("enrollment_id").jsonPrimitive.content)
        assertEquals(claimToken, begin.getValue("claim_token").jsonPrimitive.content)

        val finish = json.parseToJsonElement(
            json.encodeToString(GatewayEnrollmentFinishBody(enrollmentId = enrollmentId)),
        ).jsonObject
        assertEquals(setOf("schema", "enrollment_id"), finish.keys)
        assertEquals("kitsu.gateway-enrollment.finish.v1", finish.getValue("schema").jsonPrimitive.content)
    }

    @Test fun requestPolicyRequiresCanonicalUuidAndExact32ByteToken() {
        assertNull(EnrollmentPolicy.beginError(GatewayEnrollmentBeginBody(
            enrollmentId = enrollmentId,
            claimToken = claimToken,
        )))
        assertEquals("invalid_request", EnrollmentPolicy.beginError(GatewayEnrollmentBeginBody(
            enrollmentId = enrollmentId.uppercase(),
            claimToken = claimToken,
        )))
        assertEquals("invalid_request", EnrollmentPolicy.beginError(GatewayEnrollmentBeginBody(
            enrollmentId = enrollmentId,
            claimToken = claimToken + "=",
        )))
        assertEquals("invalid_request", EnrollmentPolicy.beginError(GatewayEnrollmentBeginBody(
            enrollmentId = enrollmentId,
            claimToken = Base64.getUrlEncoder().withoutPadding().encodeToString(ByteArray(31)),
        )))
    }

    @Test fun receiptMapperAcceptsOnlyBoundEnrollmentStates() {
        val begin = FirmwareBlePayloadMapper.gatewayEnrollmentBegin(
            """{"schema":"kitsu.gateway-enrollment.receipt.v1","accepted":true,"state":"physical_confirmation_required","enrollment_id":"$enrollmentId","expires_in_ms":60000,"error_code":null}"""
                .toByteArray(),
            enrollmentId,
        )
        assertTrue(begin.accepted)
        assertEquals(60_000, begin.expiresInMs)

        val waiting = FirmwareBlePayloadMapper.gatewayEnrollmentFinish(
            """{"schema":"kitsu.gateway-enrollment.receipt.v1","accepted":false,"state":"physical_confirmation_required","enrollment_id":"$enrollmentId","expires_in_ms":45000,"error_code":"physical_confirmation_required"}"""
                .toByteArray(),
            enrollmentId,
        )
        assertFalse(waiting.accepted)

        val ready = FirmwareBlePayloadMapper.gatewayEnrollmentFinish(
            """{"schema":"kitsu.gateway-enrollment.receipt.v1","accepted":true,"state":"ready_for_wifi","enrollment_id":"$enrollmentId","expires_in_ms":300000,"error_code":null}"""
                .toByteArray(),
            enrollmentId,
        )
        assertTrue(ready.accepted)
        assertNull(ready.errorCode)
    }

    @Test fun receiptBindingRejectsAnotherEnrollment() {
        val failure = runCatching {
            FirmwareBlePayloadMapper.gatewayEnrollmentBegin(
                """{"schema":"kitsu.gateway-enrollment.receipt.v1","accepted":true,"state":"physical_confirmation_required","enrollment_id":"11112233-4455-6677-8899-aabbccddeeff","expires_in_ms":60000,"error_code":null}"""
                    .toByteArray(),
                enrollmentId,
            )
        }.exceptionOrNull()
        assertEquals("enrollment_response_binding_failed", (failure as TransportException).code)
    }

    @Test fun timeoutEventWithWipedEnrollmentIdIsTerminalButWellFormed() {
        val event = FirmwareBlePayloadMapper.gatewayEnrollmentEvent(
            """{"schema":"kitsu.gateway-enrollment.event.v1","accepted":false,"state":"expired","enrollment_id":null,"expires_in_ms":0,"error_code":"expired"}""".toByteArray(),
        )
        assertFalse(event.accepted)
        assertEquals("expired", event.state)
        assertNull(event.enrollmentId)
    }
}
