package ptl.kitsu.app.security

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.EncounterRarity
import ptl.kitsu.app.model.EncounterUnlockCode

class EncounterCodeVaultPolicyTest {
    @Test
    fun upsertIsMultiDeviceDeduplicatedAndStateIsMonotonic() {
        val first = code("KT12AF", "event:1", "KITSU-ONE", redeemed = true)
        val otherDevice = code("KTBEEF", "event:1", "KITSU-TWO")
        val refreshed = first.copy(
            code = "KITSU-ONE-ROTATED",
            acquiredAtEpoch = 20,
            redeemed = false,
            installed = true,
        )

        val merged = EncounterVaultPolicy.merge(listOf(first, otherDevice), listOf(refreshed))

        assertEquals(2, merged.size)
        val updated = merged.single { it.deviceId == "KT12AF" }
        assertEquals("KITSU-ONE-ROTATED", updated.code)
        assertTrue(updated.redeemed)
        assertTrue(updated.installed)
        assertEquals(20, updated.acquiredAtEpoch)
    }

    @Test
    fun deletingOneDeviceNeverClearsAnotherDevice() {
        val records = listOf(
            code("KT12AF", "event:1", "KITSU-ONE"),
            code("KTBEEF", "event:2", "KITSU-TWO"),
        )

        val retained = EncounterVaultPolicy.deleteForDevice(records, "KT12AF")

        assertEquals(listOf("KTBEEF"), retained.map { it.deviceId })
        assertFalse(retained.any { it.code == "KITSU-ONE" })
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
