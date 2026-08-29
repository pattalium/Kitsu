package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.model.EncounterRarity
import ptl.kitsu.app.model.EncounterDiscoveryRecord
import ptl.kitsu.app.model.PUBLIC_ENCOUNTER_CATALOG
import ptl.kitsu.app.model.EncounterUnlockCode

class EncounterFieldGuideTest {
    @Test
    fun catalogContainsTheExactBalancedTwentyOneCreatureRoster() {
        val catalog = EncounterFieldGuidePolicy.catalog

        assertEquals(21, catalog.size)
        assertEquals(21, catalog.map { it.packId }.distinct().size)
        assertEquals(21, catalog.map { it.name }.distinct().size)
        EncounterRarity.entries.forEach { rarity ->
            assertEquals("$rarity roster count", 3, catalog.count { it.rarity == rarity })
        }
        assertTrue(catalog.any { it.packId == 0x91A2DE7BL && it.name == "Red Panda" })
        assertTrue(catalog.any { it.packId == 0xE59408E0L && it.name == "Ferret" })
    }

    @Test
    fun recordsBecomeSeenOrOwnedAndRetainCountAndLatestAvailableSource() {
        val records = listOf(
            encounter(
                codeId = "frog:first",
                packId = 0x5CAC86A3L,
                source = "mesh_repeater",
                acquiredAt = 10,
            ),
            encounter(
                codeId = "frog:second",
                packId = 0x5CAC86A3L,
                source = null,
                acquiredAt = 20,
            ),
            encounter(
                codeId = "red-panda",
                packId = 0x91A2DE7BL,
                source = "direct_encounter",
                acquiredAt = 30,
                redeemed = true,
            ),
            encounter(
                codeId = "ferret-by-name",
                packId = null,
                creatureName = " ferret ",
                source = "event_badge",
                acquiredAt = 40,
            ),
        )

        val guide = EncounterFieldGuidePolicy.build(records, activePackId = 0x29B4B2F7L)
        val frog = guide.named("Frog")
        val redPanda = guide.named("Red Panda")
        val ferret = guide.named("Ferret")
        val otter = guide.named("Otter")
        val turtle = guide.named("Turtle")

        assertEquals(FieldGuideDiscovery.SEEN, frog.discovery)
        assertEquals(2, frog.encounterCount)
        assertEquals(20L, frog.lastEncounterEpoch)
        assertEquals("mesh_repeater", frog.lastSource)
        assertEquals(FieldGuideDiscovery.OWNED, redPanda.discovery)
        assertEquals(FieldGuideDiscovery.SEEN, ferret.discovery)
        assertEquals(FieldGuideDiscovery.OWNED, otter.discovery)
        assertEquals(0, otter.encounterCount)
        assertEquals(FieldGuideDiscovery.UNSEEN, turtle.discovery)
        assertEquals(0, turtle.encounterCount)
        assertNull(turtle.lastSource)
    }

    @Test
    fun installedEncounterIsOwnedEvenWhenLegacyRecordWasNotMarkedRedeemed() {
        val guide = EncounterFieldGuidePolicy.build(
            records = listOf(
                encounter(
                    codeId = "installed",
                    packId = 0x86F3BB5DL,
                    acquiredAt = 50,
                    installed = true,
                ),
            ),
            activePackId = null,
        )

        assertEquals(FieldGuideDiscovery.OWNED, guide.named("Okapi").discovery)
    }

    @Test
    fun noCodeDiscoveryBecomesSeenAndFirmwareCountDoesNotDoubleCountCodeRecords() {
        val discovery = PUBLIC_ENCOUNTER_CATALOG.map { creature ->
            when (creature.name) {
                "Frog" -> EncounterDiscoveryRecord(creature.packId, 2, "mesh_peer")
                "Turtle" -> EncounterDiscoveryRecord(creature.packId, 1, "mesh_message_rx")
                else -> EncounterDiscoveryRecord(creature.packId, 0, null)
            }
        }
        val guide = EncounterFieldGuidePolicy.build(
            records = listOf(
                encounter(
                    codeId = "frog:code-for-one-of-two",
                    packId = 0x5CAC86A3L,
                    source = "mesh_repeater",
                    acquiredAt = 100,
                ),
            ),
            activePackId = null,
            discoveryRecords = discovery,
        )

        val frog = guide.named("Frog")
        assertEquals(FieldGuideDiscovery.SEEN, frog.discovery)
        assertEquals(2, frog.encounterCount)
        assertEquals("mesh_peer", frog.lastSource)

        val noCodeTurtle = guide.named("Turtle")
        assertEquals(FieldGuideDiscovery.SEEN, noCodeTurtle.discovery)
        assertEquals(1, noCodeTurtle.encounterCount)
        assertEquals("mesh_message_rx", noCodeTurtle.lastSource)
    }

    @Test
    fun codeCountIsOnlyTheFallbackWhenNoAuthoritativeDiscoveryPageExists() {
        val codes = listOf(
            encounter("ferret:1", 0xE59408E0L, acquiredAt = 1),
            encounter("ferret:2", 0xE59408E0L, acquiredAt = 2),
        )

        assertEquals(2, EncounterFieldGuidePolicy.build(codes, null).named("Ferret").encounterCount)

        val authoritativeZero = PUBLIC_ENCOUNTER_CATALOG.map { creature ->
            EncounterDiscoveryRecord(creature.packId, 0, null)
        }
        val withDeviceLedger = EncounterFieldGuidePolicy.build(
            records = codes,
            activePackId = null,
            discoveryRecords = authoritativeZero,
        ).named("Ferret")
        assertEquals(FieldGuideDiscovery.SEEN, withDeviceLedger.discovery)
        assertEquals(0, withDeviceLedger.encounterCount)
    }

    @Test
    fun discoveryRefreshErrorOnlyClaimsLastCountsWhenADevicePageExists() {
        assertEquals(
            "Live encounter notes could not refresh; no verified device counts are available yet.",
            encounterDiscoveryRefreshErrorCopy(hasVerifiedCounts = false),
        )
        assertEquals(
            "Live encounter notes could not refresh; showing the last verified device counts.",
            encounterDiscoveryRefreshErrorCopy(hasVerifiedCounts = true),
        )
    }

    private fun List<FieldGuideEntry>.named(name: String): FieldGuideEntry = single {
        it.creature.name == name
    }

    private fun encounter(
        codeId: String,
        packId: Long?,
        creatureName: String? = null,
        source: String? = null,
        acquiredAt: Long,
        redeemed: Boolean = false,
        installed: Boolean = false,
    ) = EncounterUnlockCode(
        deviceId = "KT0001",
        codeId = codeId,
        code = "K8-ABCDE-FGHJK-MNPQR",
        packId = packId,
        creatureName = creatureName,
        rarity = EncounterRarity.COMMON,
        source = source,
        acquiredAtEpoch = acquiredAt,
        redeemed = redeemed,
        installed = installed,
    )
}
