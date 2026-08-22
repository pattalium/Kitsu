-keepattributes Signature,*Annotation*
-dontwarn javax.annotation.**
# The pure-Java Ed25519 verifier has an optional Oracle-JVM key adapter. Android always passes
# its concrete EdDSAPublicKey, so that unreachable adapter is intentionally absent.
-dontwarn sun.security.x509.X509Key
