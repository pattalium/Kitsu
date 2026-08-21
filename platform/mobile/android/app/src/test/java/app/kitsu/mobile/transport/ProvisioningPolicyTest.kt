package app.kitsu.mobile.transport

import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.LanState
import app.kitsu.mobile.model.ProvisioningReceipt
import app.kitsu.mobile.model.WifiProvisioning
import app.kitsu.mobile.model.WifiSecurity
import app.kitsu.mobile.model.toConfigureBody
import java.security.cert.X509Certificate
import java.util.Base64
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509TrustManager
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ProvisioningPolicyTest {
    @Test fun wifiContractAcceptsExactBoundsAndCanonicalizesSsid() {
        val value = WifiProvisioning("Kitsu 🦊", "printable-pass", WifiSecurity.WPA2_WPA3)
        assertNull(ProvisioningPolicy.wifiError(value))
        val body = value.toConfigureBody()
        assertEquals(
            Base64.getUrlEncoder().withoutPadding().encodeToString(value.ssid.toByteArray()),
            body.ssidB64,
        )
        assertEquals("printable-pass", body.passphrase)
    }

    @Test fun wifiRejectsNulOversizedUtf8AndNonPrintablePassphrase() {
        assertEquals("invalid_ssid", ProvisioningPolicy.wifiError(WifiProvisioning("a\u0000b", "12345678")))
        assertEquals("invalid_ssid", ProvisioningPolicy.wifiError(WifiProvisioning("🦊".repeat(9), "12345678")))
        assertEquals("invalid_passphrase", ProvisioningPolicy.wifiError(WifiProvisioning("Home", "short")))
        assertEquals(
            "invalid_passphrase",
            ProvisioningPolicy.wifiError(WifiProvisioning("Home", "line\nbreak")),
        )
    }

    @Test fun wifiStorageIsClaimedOnlyAfterAuthenticatedStateConfirmsIt() {
        val stored = ProvisioningReceipt(accepted = true, state = "stored")
        val verified = KitsuStatus(
            deviceId = "KTTEST",
            displayName = "Kitsu",
            lan = LanState(wifiConfigured = true, wifiState = "connected"),
            updatedAt = 1,
        )
        assertNull(ProvisioningPolicy.wifiVerificationError(stored, verified))
        assertEquals(
            "wifi_storage_not_confirmed",
            ProvisioningPolicy.wifiVerificationError(
                stored,
                verified.copy(lan = verified.lan.copy(wifiConfigured = false)),
            ),
        )
        assertEquals(
            "wifi_configuration_rejected",
            ProvisioningPolicy.wifiVerificationError(
                ProvisioningReceipt(accepted = false, state = "rejected"),
                verified,
            ),
        )
    }

    @Test fun gatewayAcceptsPinnedCaHostnameAndExactSpki() {
        val ca = platformCa()
        val value = GatewayConfiguration(
            gatewayId = "abcdefab-cdef-4abc-8def-abcdefabcdef",
            host = "192.0.2.10",
            bootstrapPort = 7442,
            port = 7443,
            serverName = "gateway.example",
            caCertificateDerB64 = b64(ca.encoded),
            spkiSha256B64 = b64(ByteArray(32) { it.toByte() }),
        )
        assertNull(ProvisioningPolicy.gatewayError(value))
        assertNull(ProvisioningPolicy.gatewayError(value.copy(host = "192.0.2.10")))
        assertNull(ProvisioningPolicy.gatewayError(value.copy(host = "fd00::32")))
    }

    @Test fun gatewayRejectsUrlUppercaseUuidBadPortAndNonCaMaterial() {
        val valid = GatewayConfiguration(
            gatewayId = "abcdefab-cdef-4abc-8def-abcdefabcdef",
            host = "192.0.2.10",
            bootstrapPort = 7442,
            port = 7443,
            serverName = "gateway.example",
            caCertificateDerB64 = b64(platformCa().encoded),
            spkiSha256B64 = b64(ByteArray(32)),
        )
        assertEquals("invalid_gateway_id", ProvisioningPolicy.gatewayError(valid.copy(gatewayId = valid.gatewayId.uppercase())))
        assertEquals("invalid_host", ProvisioningPolicy.gatewayError(valid.copy(host = "https://gateway.example/path")))
        assertEquals("invalid_server_name", ProvisioningPolicy.gatewayError(valid.copy(serverName = "192.0.2.11")))
        assertEquals("invalid_port", ProvisioningPolicy.gatewayError(valid.copy(port = 65_536)))
        assertEquals("invalid_bootstrap_port", ProvisioningPolicy.gatewayError(valid.copy(bootstrapPort = 0)))
        assertEquals("gateway_ports_must_differ", ProvisioningPolicy.gatewayError(valid.copy(bootstrapPort = 7443)))
        assertEquals("invalid_ca_certificate", ProvisioningPolicy.gatewayError(valid.copy(caCertificateDerB64 = b64(byteArrayOf(1, 2, 3)))))
        assertEquals("invalid_spki", ProvisioningPolicy.gatewayError(valid.copy(spkiSha256B64 = b64(ByteArray(31)))))
    }

    private fun platformCa(): X509Certificate {
        val factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
        factory.init(null as java.security.KeyStore?)
        return factory.trustManagers.filterIsInstance<X509TrustManager>()
            .flatMap { it.acceptedIssuers.asIterable() }
            .first { it.basicConstraints >= 0 && it.encoded.size <= 8_192 }
    }

    private fun b64(bytes: ByteArray): String =
        Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)
}
