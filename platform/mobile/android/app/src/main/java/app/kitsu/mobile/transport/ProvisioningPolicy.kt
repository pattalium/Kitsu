package app.kitsu.mobile.transport

import app.kitsu.mobile.model.GatewayConfiguration
import app.kitsu.mobile.model.KitsuStatus
import app.kitsu.mobile.model.ProvisioningReceipt
import app.kitsu.mobile.model.WifiProvisioning
import java.net.Inet6Address
import java.net.InetAddress
import java.security.cert.X509Certificate
import java.security.cert.CertificateFactory
import java.util.Base64
import java.util.UUID

internal object ProvisioningPolicy {
    private val dnsLabel = Regex("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")
    private val ipv4Part = Regex("^(0|[1-9][0-9]{0,2})$")

    fun wifiError(value: WifiProvisioning): String? {
        val ssid = value.ssid.toByteArray(Charsets.UTF_8)
        if (ssid.size !in 1..32 || ssid.any { it == 0.toByte() } ||
            ssid.toString(Charsets.UTF_8) != value.ssid
        ) return "invalid_ssid"
        val passphrase = value.password
        if (passphrase.length !in 8..63 || passphrase.any { it.code !in 0x20..0x7e }) {
            return "invalid_passphrase"
        }
        return null
    }

    /**
     * A signed `stored` receipt proves that the firmware accepted the write, but
     * it is not a read-back.  Confirm the immediately following authenticated
     * state snapshot before Android claims that credentials were persisted.
     */
    fun wifiVerificationError(
        receipt: ProvisioningReceipt,
        status: KitsuStatus,
    ): String? = when {
        !receipt.accepted || receipt.state != "stored" || receipt.errorCode != null ->
            receipt.errorCode ?: "wifi_configuration_rejected"
        status.lan.wifiConfigured != true -> "wifi_storage_not_confirmed"
        else -> null
    }

    fun gatewayError(value: GatewayConfiguration): String? {
        if (!isCanonicalLowercaseUuid(value.gatewayId)) return "invalid_gateway_id"
        if (!isValidHost(value.host)) return "invalid_host"
        if (!isDnsName(value.serverName)) return "invalid_server_name"
        if (value.bootstrapPort !in 1..65_535) return "invalid_bootstrap_port"
        if (value.port !in 1..65_535) return "invalid_port"
        if (value.bootstrapPort == value.port) return "gateway_ports_must_differ"
        val certificate = decodeCanonicalBase64Url(value.caCertificateDerB64)
            ?.takeIf { it.size in 1..8_192 }
            ?: return "invalid_ca_certificate"
        val parsed = runCatching {
            CertificateFactory.getInstance("X.509")
                .generateCertificate(certificate.inputStream()) as X509Certificate
        }.getOrNull()
        if (parsed == null || parsed.basicConstraints < 0) return "invalid_ca_certificate"
        val spki = decodeCanonicalBase64Url(value.spkiSha256B64)
        if (spki?.size != 32) return "invalid_spki"
        return null
    }

    private fun isCanonicalLowercaseUuid(value: String): Boolean = runCatching {
        value == value.lowercase() && UUID.fromString(value).toString() == value
    }.getOrDefault(false)

    private fun isValidHost(value: String): Boolean {
        if (value.length !in 1..253 || value.any { it.code !in 0x21..0x7e } ||
            "://" in value || '/' in value || '?' in value || '#' in value
        ) return false
        return isIpv4(value) || isIpv6(value) || isDnsName(value)
    }

    private fun isIpv4(value: String): Boolean {
        val parts = value.split('.')
        return parts.size == 4 && parts.all { part ->
            ipv4Part.matches(part) && (part.toIntOrNull() ?: 256) in 0..255
        }
    }

    private fun isIpv6(value: String): Boolean {
        if (':' !in value || value.any { !(it.isDigit() || it.lowercaseChar() in 'a'..'f' || it in ":.") }) {
            return false
        }
        return runCatching { InetAddress.getByName(value) is Inet6Address }.getOrDefault(false)
    }

    private fun isDnsName(value: String): Boolean {
        if (value.length !in 1..253 || value.any { it.code > 0x7f } || value.endsWith('.') ||
            ':' in value || isIpv4(value)
        ) return false
        return value.split('.').all(dnsLabel::matches)
    }

    private fun decodeCanonicalBase64Url(value: String): ByteArray? {
        if (value.isEmpty() || '=' in value || value.any { !it.isLetterOrDigit() && it != '-' && it != '_' }) {
            return null
        }
        return runCatching { Base64.getUrlDecoder().decode(value) }
            .getOrNull()
            ?.takeIf { Base64.getUrlEncoder().withoutPadding().encodeToString(it) == value }
    }
}
