package ptl.kitsu.app.security

import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test

class ControllerCredentialRoleTest {
    private val json = Json { ignoreUnknownKeys = true }

    @Test fun legacyCredentialWithoutRoleRemainsOwner() {
        val legacy =
            """{"deviceAddress":"AA:BB:CC:DD:EE:FF","displayName":"Kitsu","controllerIdB64":"id","controllerRootB64":"root"}"""

        val decoded = json.decodeFromString<BondedCompanion>(legacy)

        assertEquals(ControllerRole.OWNER, decoded.role)
    }

    @Test fun caretakerRoleIsPersistedExplicitlyAndRoundTrips() {
        val caretaker = BondedCompanion(
            deviceAddress = "AA:BB:CC:DD:EE:FF",
            displayName = "Kitsu",
            controllerIdB64 = "id",
            controllerRootB64 = "root",
            role = ControllerRole.CARETAKER,
        )

        val encoded = json.encodeToString(caretaker)

        assertTrue(encoded.contains("\"role\":\"caretaker\""))
        assertEquals(caretaker, json.decodeFromString<BondedCompanion>(encoded))
    }

    @Test fun ownerDefaultKeepsLegacyStorageShape() {
        val owner = BondedCompanion(
            deviceAddress = "AA:BB:CC:DD:EE:FF",
            displayName = "Kitsu",
            controllerIdB64 = "id",
            controllerRootB64 = "root",
        )

        assertFalse(json.encodeToString(owner).contains("\"role\""))
    }

    @Test fun unknownPersistedRoleFailsClosedInsteadOfBecomingOwner() {
        val malformed =
            """{"deviceAddress":"AA:BB:CC:DD:EE:FF","displayName":"Kitsu","controllerIdB64":"id","controllerRootB64":"root","role":"admin"}"""

        assertThrows(Throwable::class.java) {
            json.decodeFromString<BondedCompanion>(malformed)
        }
    }
}
