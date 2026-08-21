from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
ESP_TLS = (ROOT / "src" / "kitsu_esp32_gateway_tls.cpp").read_text(
    encoding="utf-8"
)
ESP_TLS_HEADER = (ROOT / "src" / "kitsu_esp32_gateway_tls.h").read_text(
    encoding="utf-8"
)
BOOTSTRAP = (ROOT / "src" / "kitsu_gateway_bootstrap.cpp").read_text(
    encoding="utf-8"
)
BOOTSTRAP_HEADER = (ROOT / "src" / "kitsu_gateway_bootstrap.h").read_text(
    encoding="utf-8"
)
LAN_RUNTIME = (ROOT / "src" / "kitsu_gateway_lan_runtime.cpp").read_text(
    encoding="utf-8"
)
LAN_HEADER = (ROOT / "src" / "kitsu_gateway_lan_runtime.h").read_text(
    encoding="utf-8"
)


class GatewayTlsSourceContractTests(unittest.TestCase):
    def test_direct_tls_has_no_insecure_or_plaintext_api(self):
        forbidden = (
            "setInsecure(",
            "MBEDTLS_SSL_VERIFY_NONE",
            "skip_common_name",
            "is_plain_tcp",
            "esp_tls_plain_tcp_connect",
            "HTTPClient",
            "followRedirect",
        )
        for token in forbidden:
            self.assertNotIn(token, ESP_TLS)

    def test_tls12_sni_chain_spki_and_alpn_are_all_enforced(self):
        minimum = ESP_TLS.index("mbedtls_ssl_conf_min_version")
        handshake = ESP_TLS.index("mbedtls_ssl_handshake")
        self.assertLess(minimum, handshake)
        self.assertIn("MBEDTLS_SSL_MINOR_VERSION_3", ESP_TLS)
        self.assertIn("MBEDTLS_SSL_VERIFY_REQUIRED", ESP_TLS)
        self.assertIn("mbedtls_ssl_set_hostname(&ssl_, serverName)", ESP_TLS)
        self.assertIn("startTcp(endpointHost, port)", ESP_TLS)
        self.assertIn("mbedtls_ssl_get_verify_result", ESP_TLS)
        self.assertIn("mbedtls_ssl_conf_alpn_protocols", ESP_TLS)
        self.assertIn("mbedtls_ssl_get_alpn_protocol", ESP_TLS)
        self.assertIn("mbedtls_pk_write_pubkey_der", ESP_TLS)
        self.assertIn("mbedtls_sha256_ret", ESP_TLS)
        self.assertIn("constantTimeEqual(actual, expected", ESP_TLS)

    def test_endpoint_route_accepts_ip_but_sni_stays_dns_only(self):
        self.assertIn("validEndpointHost(endpointHost)", ESP_TLS)
        self.assertIn("validFqdn(serverName)", ESP_TLS)
        self.assertIn("AF_UNSPEC", ESP_TLS)
        self.assertIn("lwip_getaddrinfo", ESP_TLS)
        self.assertIn("lwip_inet_pton(AF_INET, endpointHost", ESP_TLS)
        self.assertIn("lwip_inet_pton(AF_INET6, endpointHost", ESP_TLS)
        self.assertIn("address->ai_family", ESP_TLS)
        self.assertIn("hasNonNumericLabelByte", ESP_TLS)
        self.assertIn("validEndpointHost(view.host)", LAN_RUNTIME)
        self.assertIn("validFqdn(view.serverName)", LAN_RUNTIME)

        resolver = ESP_TLS.split("bool startTcp", 1)[1].split(
            "bool startConnect", 1
        )[0]
        self.assertLess(
            resolver.index("lwip_inet_pton(AF_INET, endpointHost"),
            resolver.index("lwip_getaddrinfo(endpointHost"),
        )
        self.assertLess(
            resolver.index("lwip_inet_pton(AF_INET6, endpointHost"),
            resolver.index("lwip_getaddrinfo(endpointHost"),
        )

    def test_mutual_tls_proves_key_san_and_certificate_request(self):
        self.assertIn("certificateBindsKeyAndCompanion", ESP_TLS)
        self.assertIn("mbedtls_x509_crt_verify(&clientCertificate_", ESP_TLS)
        self.assertIn("mbedtls_ssl_conf_own_cert", ESP_TLS)
        self.assertIn("ssl_.client_auth != 0", ESP_TLS)
        self.assertIn("clientCertificateBindsCompanion", ESP_TLS)
        self.assertIn("clientCredentialPresented", ESP_TLS)

    def test_handshake_socket_stays_nonblocking_for_overall_deadline(self):
        self.assertIn("oldFlags | O_NONBLOCK", ESP_TLS)
        self.assertIn("millis() - connectStartedAt_", ESP_TLS)
        self.assertIn("timeval wait{}", ESP_TLS)
        self.assertIn("lwip_select", ESP_TLS)
        self.assertIn("MBEDTLS_ERR_SSL_WANT_READ", ESP_TLS)
        self.assertNotIn("oldFlags & ~O_NONBLOCK", ESP_TLS)
        self.assertNotIn("bool writeAll", ESP_TLS)
        self.assertNotIn("bool readExact", ESP_TLS)
        self.assertNotIn("delay(", ESP_TLS)
        self.assertIn("Exactly one nonblocking TLS read", ESP_TLS)
        self.assertIn("beginConnect", LAN_HEADER)
        self.assertIn("pollConnect", LAN_HEADER)
        self.assertIn("transport_->beginConnect", LAN_RUNTIME)
        self.assertIn("transport_->pollConnect", LAN_RUNTIME)
        self.assertNotIn("transport_->connect(", LAN_RUNTIME)
        pending = LAN_RUNTIME.split("if (connectPending_)", 1)[1].split(
            "} else {", 1
        )[0]
        self.assertNotIn("credentials_->acquire", pending)

    def test_certificate_time_validation_fails_before_socket(self):
        bootstrap_exchange = ESP_TLS.split(
            "Esp32GatewayBootstrapTransport::exchangeOneFramedRequest", 1
        )[1].split("void Esp32GatewayBootstrapTransport::close", 1)[0]
        self.assertLess(
            bootstrap_exchange.index("if (!evidence.systemTimeValid)"),
            bootstrap_exchange.index("new (std::nothrow) Implementation"),
        )
        self.assertIn("trustedWallClock(now)", ESP_TLS)
        self.assertIn("1704067200LL", ESP_TLS)
        self.assertIn("GatewayLanIoResult::TimeUnavailable", ESP_TLS)
        self.assertNotIn("MBEDTLS_X509_BADCERT_EXPIRED", ESP_TLS)
        self.assertNotIn("MBEDTLS_X509_BADCERT_FUTURE", ESP_TLS)

    def test_bootstrap_has_no_client_credential_and_exact_frame_bounds(self):
        self.assertIn("evidence.clientCredentialPresented = false", ESP_TLS)
        self.assertIn("kGatewayBootstrapAlpn", ESP_TLS)
        self.assertIn("kGatewayBootstrapMaximumProxyResponseBytes", ESP_TLS)
        self.assertIn("putU32Be", ESP_TLS)
        self.assertIn("getU32Be", ESP_TLS)
        self.assertIn("pendingBytes() == 0U", ESP_TLS)
        self.assertIn("state.transport->close();", BOOTSTRAP)
        self.assertIn("GatewayBootstrapIoResult::WouldBlock", ESP_TLS)

    def test_bootstrap_workspace_and_transient_ram_are_bounded(self):
        workspace = BOOTSTRAP_HEADER.split(
            "struct GatewayBootstrapWorkspace", 1
        )[1].split("constexpr size_t kGatewayBootstrapPermanent", 1)[0]
        self.assertIn("leafCertificate", workspace)
        self.assertIn("chainCertificates", workspace)
        self.assertNotIn("proxyResponse", workspace)
        self.assertNotIn("backendResponse", workspace)
        self.assertIn("20U * 1024U", BOOTSTRAP_HEADER)
        self.assertIn("80U * 1024U", BOOTSTRAP_HEADER)
        self.assertIn(
            "ScopedZeroBuffer backendRequest(kEnrollmentMaximumRequestBytes)",
            BOOTSTRAP,
        )
        self.assertIn("ScopedZeroBuffer proxyRequest", BOOTSTRAP)
        self.assertIn("ScopedZeroBuffer proxyResponse", BOOTSTRAP)
        self.assertIn("beginExchangeAndInstall", BOOTSTRAP)
        self.assertIn("pollExchangeAndInstall", BOOTSTRAP)
        self.assertIn("state.proxyResponse.data(), state.proxyResponseBytes", BOOTSTRAP)

    def test_heap_guards_and_secret_silence_are_source_enforced(self):
        self.assertIn("heap_caps_get_free_size", ESP_TLS)
        self.assertIn("heap_caps_get_largest_free_block", ESP_TLS)
        self.assertIn("heap_caps_get_minimum_free_size", ESP_TLS)
        self.assertIn("kEsp32GatewayTlsMinimumFreeHeapBytes", ESP_TLS_HEADER)
        self.assertIn("secureZero", ESP_TLS)
        for token in ("Serial.", "log_", "\nprintf(", "payloadJson"):
            self.assertNotIn(token, ESP_TLS)

    def test_steady_runtime_is_bounded_persistent_and_byte_exact(self):
        self.assertIn("kGatewayLanQueueDepth = 4U", LAN_HEADER)
        self.assertIn("kGatewayLanMaximumQueuedBytes = 32U * 1024U", LAN_HEADER)
        self.assertIn("reserveLanTxSequenceBlock", LAN_RUNTIME)
        self.assertIn("acceptLanRxSequence", LAN_RUNTIME)
        self.assertIn("frame, frameBytes, decoded, params", LAN_RUNTIME)
        self.assertIn("kGatewayLanMaximumBackoffMs", LAN_RUNTIME)
        self.assertIn("kGatewayLanAckTimeoutMs", LAN_RUNTIME)
        self.assertIn("RemoteConnectivityUnavailable", LAN_RUNTIME)


if __name__ == "__main__":
    unittest.main()
