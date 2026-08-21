package app.kitsu.mobile.transport

import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import okhttp3.HttpUrl.Companion.toHttpUrl
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GatewayCatalogContractTest {
    private val json = Json { ignoreUnknownKeys = false }

    @Test fun ownerCatalogUsesTheFrozenSafeRecordAndMapsDirectlyToBleConfiguration() {
        val envelope = json.decodeFromString<ListEnvelope<List<GatewayProvisioningRecord>>>(FIXTURE)
        val record = envelope.items.single()
        val keys = json.parseToJsonElement(FIXTURE).jsonObject
            .getValue("items").jsonArray.single().jsonObject.keys

        assertEquals(
            setOf(
                "gateway_id",
                "display_name",
                "host",
                "bootstrap_port",
                "port",
                "server_name",
                "ca_cert_der_b64",
                "spki_sha256_b64",
                "state",
            ),
            keys,
        )
        assertEquals("Kitsu Home Gateway", record.displayName)
        assertEquals("192.0.2.10", record.host)
        assertEquals(7442, record.bootstrapPort)
        assertEquals(7443, record.port)
        assertEquals("gateway.example", record.serverName)
        assertNull(GatewayCatalogPolicy.validate(record))
        assertEquals(record.gatewayId, record.toGatewayConfiguration().gatewayId)
        assertEquals(record.caCertificateDerB64, record.toGatewayConfiguration().caCertificateDerB64)
        assertEquals(record.spkiSha256B64, record.toGatewayConfiguration().spkiSha256B64)
    }

    @Test fun catalogTrustIsPinnedToTheOfficialApiOrigin() {
        assertTrue(GatewayCatalogPolicy.isTrustedOrigin("https://api.k32.run".toHttpUrl()))
        assertTrue(GatewayCatalogPolicy.isTrustedOrigin("https://api.k32.run/".toHttpUrl()))
        assertEquals(
            listOf(false, false, false),
            listOf(
                GatewayCatalogPolicy.isTrustedOrigin("https://api.k32.run:8443".toHttpUrl()),
                GatewayCatalogPolicy.isTrustedOrigin("https://api.k32.run.example".toHttpUrl()),
                GatewayCatalogPolicy.isTrustedOrigin("http://api.k32.run".toHttpUrl()),
            ),
        )
        assertEquals("/v1/gateways", BackendRoutes().gateways)
        assertEquals("/v1/companions/{id}/channels", BackendRoutes().channels)
        assertEquals("https://docs.k32.run/connectivity/", GatewayCatalogPolicy.GATEWAY_SETUP_URL)
    }

    @Test fun malformedDisplayStateOrTrustMaterialFailsClosed() {
        val valid = json.decodeFromString<ListEnvelope<List<GatewayProvisioningRecord>>>(FIXTURE).items.single()
        assertEquals(
            "invalid_gateway_display_name",
            GatewayCatalogPolicy.validate(valid.copy(displayName = "bad\nname")),
        )
        assertEquals(
            "invalid_gateway_state",
            GatewayCatalogPolicy.validate(valid.copy(state = "READY")),
        )
        assertEquals(
            "invalid_spki",
            GatewayCatalogPolicy.validate(valid.copy(spkiSha256B64 = "AAAA")),
        )
    }

    companion object {
        // Kept byte-for-byte identical to the Swift fixture.
        private const val FIXTURE =
            """{"items":[{"gateway_id":"00112233-4455-6677-8899-aabbccddeeff","display_name":"Kitsu Home Gateway","host":"192.0.2.10","bootstrap_port":7442,"port":7443,"server_name":"gateway.example","ca_cert_der_b64":"MIIBijCCATCgAwIBAgIBATAKBggqhkjOPQQDAjArMQ4wDAYDVQQKDAVLaXRzdTEZMBcGA1UEAwwQS2l0c3UgRml4dHVyZSBDQTAeFw0yNTAxMDEwMDAwMDBaFw0zNTAxMDEwMDAwMDBaMCsxDjAMBgNVBAoMBUtpdHN1MRkwFwYDVQQDDBBLaXRzdSBGaXh0dXJlIENBMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEVNMdTE9rFptHUEqD0NjAySYNjiO21qRpOGyxiBYyUoxvU-RkCg8p_iUcZckaJFTyacBnVw4ll6DOIalSnIt_cKNFMEMwEgYDVR0TAQH_BAgwBgEB_wIBADAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFO7oIIdwNziJAhO5aqzJIMgUEWfVMAoGCCqGSM49BAMCA0gAMEUCIFBtIRHdumQs1YiAy4Ie-a0wYlsgXKpgw2HxmvL4FkNbAiEA_VEgaRleRy4qN0rgY5kuLRnjw5vsJ0FD8Kj8DfKgkQE","spki_sha256_b64":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","state":"ready"}]}"""
    }
}
