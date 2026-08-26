package ptl.kitsu.app.security

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.EncounterCodePolicy
import ptl.kitsu.app.model.EncounterRarity
import ptl.kitsu.app.model.EncounterUnlockCode

class EncounterCodeVaultPolicyTest {
    @Test
    fun upsertIsMultiDeviceDeduplicatedAndStateIsMonotonic() {
        val first = code("KT12AF", "event:1", "K8-ABCDE-FGHJK-MNPQR", redeemed = true)
        val otherDevice = code("KTBEEF", "event:1", "K8-STVWZ-23456-789AB")
        val refreshed = first.copy(
            code = "K8-CDEFG-HJKMN-PQRST",
            acquiredAtEpoch = 20,
            redeemed = false,
            installed = true,
        )

        val merged = EncounterVaultPolicy.merge(listOf(first, otherDevice), listOf(refreshed))

        assertEquals(2, merged.size)
        val updated = merged.single { it.deviceId == "KT12AF" }
        assertEquals("K8-CDEFG-HJKMN-PQRST", updated.code)
        assertTrue(updated.redeemed)
        assertTrue(updated.installed)
        assertEquals(20, updated.acquiredAtEpoch)
    }

    @Test
    fun deletingOneDeviceNeverClearsAnotherDevice() {
        val records = listOf(
            code("KT12AF", "event:1", "K8-ABCDE-FGHJK-MNPQR"),
            code("KTBEEF", "event:2", "K8-STVWZ-23456-789AB"),
        )

        val retained = EncounterVaultPolicy.deleteForDevice(records, "KT12AF")

        assertEquals(listOf("KTBEEF"), retained.map { it.deviceId })
        assertFalse(retained.any { it.code == "K8-ABCDE-FGHJK-MNPQR" })
    }

    @Test
    fun encounterCodesRequireExactCanonicalCrockfordShape() {
        assertTrue(EncounterCodePolicy.validCode("K8-ABCDE-FGHJK-MNPQR"))
        assertTrue(EncounterCodePolicy.validCode("K8-STVWZ-23456-789AB"))

        listOf(
            "k8-ABCDE-FGHJK-MNPQR",
            "K8-abcde-FGHJK-MNPQR",
            "K8-ABCDE-FGHJK-MNPQ",
            "K8-ABCDE-FGHJK-MNPQRR",
            "K8-ABCDE-FGHJKM-NPQRS",
            "K8-ABCDE-FGHIJ-MNPQR",
            "K8-ABCDE-FGHJL-MNPQR",
            "K8-ABCDE-FGHJO-MNPQR",
            "K8-ABCDE-FGHJU-MNPQR",
            "K8ABCDE-FGHJK-MNPQR",
            "K8-ABCDE-FGHJK-MNPQR-",
        ).forEach { value ->
            assertFalse(value, EncounterCodePolicy.validCode(value))
        }
    }

    private fun code(
        deviceId: String,
        codeId: String,
        value: String,
        redeemed: Boolean = false,
    ) = EncounterUnlockCode(
        deviceId = deviceId,
        codeId = codeId,
        code = value,
        packId = 1,
        rarity = EncounterRarity.RARE,
        source = "mesh_message",
        acquiredAtEpoch = 10,
        redeemed = redeemed,
    )
}
