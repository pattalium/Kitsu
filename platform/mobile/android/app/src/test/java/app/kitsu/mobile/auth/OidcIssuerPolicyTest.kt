package app.kitsu.mobile.auth

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class OidcIssuerPolicyTest {
    @Test fun productionIssuerIsTheExactKeycloakRealm() {
        assertEquals("https://auth.k32.run/realms/kitsu", OidcIssuerPolicy.OFFICIAL_ISSUER)
        assertNull(OidcIssuerPolicy.validationError(OidcIssuerPolicy.OFFICIAL_ISSUER))
        assertEquals(
            "https://auth.k32.run/realms/kitsu/.well-known/openid-configuration",
            OidcIssuerPolicy.discoveryUrl(OidcIssuerPolicy.OFFICIAL_ISSUER),
        )
    }

    @Test fun issuerAndRevocationEndpointFailClosedAcrossOrigins() {
        assertEquals("oidc_issuer_requires_https", OidcIssuerPolicy.validationError("http://auth.k32.run/realms/kitsu"))
        assertEquals("invalid_oidc_issuer", OidcIssuerPolicy.validationError("https://user@auth.k32.run/realms/kitsu"))
        assertTrue(
            OidcIssuerPolicy.isTrustedRevocationEndpoint(
                OidcIssuerPolicy.OFFICIAL_ISSUER,
                "https://auth.k32.run/realms/kitsu/protocol/openid-connect/revoke",
            ),
        )
        assertFalse(
            OidcIssuerPolicy.isTrustedRevocationEndpoint(
                OidcIssuerPolicy.OFFICIAL_ISSUER,
                "https://evil.example/revoke",
            ),
        )
    }
}
