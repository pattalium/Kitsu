package app.kitsu.mobile.transport

import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.WifiProvisioning
import app.kitsu.mobile.model.WifiSecurity
import app.kitsu.mobile.model.toConfigureBody
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class ProvisioningWireContractTest {
    private val json = Json { explicitNulls = false }

    @Test fun wifiRequestHasOnlyFrozenFieldsAndCanonicalSsid() {
        val root = json.parseToJsonElement(
            json.encodeToString(WifiProvisioning("Home", "abcdefgh", WifiSecurity.WPA3).toConfigureBody()),
        ).jsonObject
        assertEquals(setOf("ssid_b64", "security", "passphrase"), root.keys)
        assertEquals("SG9tZQ", root.getValue("ssid_b64").jsonPrimitive.content)
        assertEquals("wpa3", root.getValue("security").jsonPrimitive.content)
        assertEquals("abcdefgh", root.getValue("passphrase").jsonPrimitive.content)
        assertFalse("ssid" in root)
    }

    @Test fun gatewayRequestHasOnlyFrozenPublicTrustFields() {
        val root = json.parseToJsonElement(
            json.encodeToString(
                GatewayConfiguration(
                    gatewayId = "00000000-0000-0000-0000-000000000001",
                    host = "192.0.2.10",
                    bootstrapPort = 7442,
                    port = 7443,
                    serverName = "gateway.example",
                    caCertificateDerB64 = "AQ",
                    spkiSha256B64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                ),
            ),
        ).jsonObject
        assertEquals(
            setOf(
                "gateway_id", "host", "bootstrap_port", "port", "server_name",
                "ca_cert_der_b64", "spki_sha256_b64",
            ),
            root.keys,
        )
        assertEquals(7442, root.getValue("bootstrap_port").jsonPrimitive.content.toInt())
        assertEquals(7443, root.getValue("port").jsonPrimitive.content.toInt())
        assertFalse(root.keys.any { "private" in it || "secret" in it || "password" in it })
    }
}
