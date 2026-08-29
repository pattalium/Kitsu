package ptl.kitsu.app.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import java.util.UUID

const val ENCOUNTER_CODES_OPERATION = "encounter.codes.get.v1"
const val ENCOUNTER_CATALOG_OPERATION = "encounter.catalog.get.v1"
const val ENCOUNTER_DISCOVERY_OPERATION = "encounter.discovery.get.v1"
const val ENCOUNTER_NEIGHBORS_OPERATION = "encounter.neighbors.get.v1"
const val NEIGHBOR_ACTION_OPERATION = "encounter.neighbor.action.v1"
const val ENCOUNTER_CODES_SCHEMA = "kitsu.encounter-codes.v1"
const val ENCOUNTER_CATALOG_SCHEMA = "kitsu.encounter-catalog.v1"
const val ENCOUNTER_DISCOVERY_SCHEMA = "kitsu.encounter-discovery.v1"
const val ENCOUNTER_NEIGHBORS_SCHEMA = "kitsu.encounter-neighbors.v1"
const val NEIGHBOR_ACTION_RECEIPT_SCHEMA = "kitsu.neighbor-action-receipt.v1"

@Serializable
enum class EncounterRarity {
    @SerialName("common") COMMON,
    @SerialName("uncommon") UNCOMMON,
    @SerialName("rare") RARE,
    @SerialName("very_rare") VERY_RARE,
    @SerialName("epic") EPIC,
    @SerialName("legendary") LEGENDARY,
    @SerialName("mythical") MYTHICAL,
}

@Serializable
data class EncounterCatalogCreature(
    @SerialName("pack_id") val packId: Long,
    @SerialName("creature_name") val name: String,
    val rarity: EncounterRarity,
)

@Serializable
data class EncounterCatalogPage(
    val schema: String,
    val items: List<EncounterCatalogCreature> = emptyList(),
)

@Serializable
data class EncounterDiscoveryRecord(
    @SerialName("pack_id") val packId: Long,
    @SerialName("encounter_count") val encounterCount: Int,
    @SerialName("last_source") val lastSource: String? = null,
)

@Serializable
data class EncounterDiscoveryPage(
    val schema: String,
    val items: List<EncounterDiscoveryRecord> = emptyList(),
)

/** Public 21-creature roster; also provides an offline guide when Kitsu is disconnected. */
val PUBLIC_ENCOUNTER_CATALOG: List<EncounterCatalogCreature> = listOf(
    EncounterCatalogCreature(0x5CAC86A3L, "Frog", EncounterRarity.COMMON),
    EncounterCatalogCreature(0x13793DC7L, "Hamster", EncounterRarity.COMMON),
    EncounterCatalogCreature(0x7495DBFBL, "Turtle", EncounterRarity.COMMON),
    EncounterCatalogCreature(0x68D9554EL, "Rabbit", EncounterRarity.UNCOMMON),
    EncounterCatalogCreature(0x5DF6BE74L, "Hedgehog", EncounterRarity.UNCOMMON),
    EncounterCatalogCreature(0xE59408E0L, "Ferret", EncounterRarity.UNCOMMON),
    EncounterCatalogCreature(0x29B4B2F7L, "Otter", EncounterRarity.RARE),
    EncounterCatalogCreature(0x69276D0CL, "Axolotl", EncounterRarity.RARE),
    EncounterCatalogCreature(0x2DFB0797L, "Chinchilla", EncounterRarity.RARE),
    EncounterCatalogCreature(0xC163EFEDL, "Raccoon", EncounterRarity.VERY_RARE),
    EncounterCatalogCreature(0x374D2540L, "Capybara", EncounterRarity.VERY_RARE),
    EncounterCatalogCreature(0x39FC5B1AL, "Sugar Glider", EncounterRarity.VERY_RARE),
    EncounterCatalogCreature(0x91A2DE7BL, "Red Panda", EncounterRarity.EPIC),
    EncounterCatalogCreature(0xE04EC405L, "Pangolin", EncounterRarity.EPIC),
    EncounterCatalogCreature(0x8E0E1B03L, "Tasmanian Devil", EncounterRarity.EPIC),
    EncounterCatalogCreature(0x533B9B30L, "Snow Leopard", EncounterRarity.LEGENDARY),
    EncounterCatalogCreature(0x86F3BB5DL, "Okapi", EncounterRarity.LEGENDARY),
    EncounterCatalogCreature(0x2D1D89AFL, "Shoebill", EncounterRarity.LEGENDARY),
    EncounterCatalogCreature(0xA52160C5L, "Cat Girl", EncounterRarity.MYTHICAL),
    EncounterCatalogCreature(0xF0F750BDL, "Rabbit Girl", EncounterRarity.MYTHICAL),
    EncounterCatalogCreature(0x52A1C03AL, "Deer Girl", EncounterRarity.MYTHICAL),
)

object EncounterCatalogPolicy {
    const val ITEM_COUNT = 21

    fun isExactPublicCatalog(items: List<EncounterCatalogCreature>): Boolean =
        items.size == ITEM_COUNT && items.toSet() == PUBLIC_ENCOUNTER_CATALOG.toSet()
}

object EncounterDiscoveryPolicy {
    const val MAX_ENCOUNTER_COUNT = 65_535
    val LAST_SOURCES: Set<String> = setOf(
        "mesh_repeater",
        "mesh_peer",
        "mesh_message_tx",
        "mesh_message_rx",
        "mesh_advert_tx",
        "mesh_advert_rx",
        "mesh_other",
        "kitsu_neighbor",
    )

    fun isExactPublicDiscovery(items: List<EncounterDiscoveryRecord>): Boolean =
        items.size == EncounterCatalogPolicy.ITEM_COUNT &&
            items.map(EncounterDiscoveryRecord::packId) ==
            PUBLIC_ENCOUNTER_CATALOG.map(EncounterCatalogCreature::packId) &&
            items.all { item ->
                item.packId in 0L..EncounterCodePolicy.UINT32_MAX &&
                    item.encounterCount in 0..MAX_ENCOUNTER_COUNT &&
                    if (item.encounterCount == 0) {
                        item.lastSource == null
                    } else {
                        item.lastSource in LAST_SOURCES
                    }
            }
}

@Serializable
data class EncounterUnlockCode(
    @SerialName("device_id") val deviceId: String,
    @SerialName("code_id") val codeId: String,
    val code: String,
    @SerialName("pack_id") val packId: Long? = null,
    @SerialName("creature_name") val creatureName: String? = null,
    val rarity: EncounterRarity,
    val source: String? = null,
    @SerialName("acquired_at_epoch") val acquiredAtEpoch: Long,
    val redeemed: Boolean = false,
    val installed: Boolean = false,
) {
    /** Stable local identity. The raw unlock code is deliberately excluded. */
    val vaultKey: String get() = "$deviceId:$codeId"
}

@Serializable
data class EncounterCodePage(
    val schema: String,
    val items: List<EncounterUnlockCode> = emptyList(),
    val cursor: String? = null,
    @SerialName("has_more") val hasMore: Boolean = false,
)

/** A live owned Kitsu heard over the dedicated, non-MeshCore nearby protocol. */
@Serializable
data class NearbyKitsu(
    @SerialName("device_id") val deviceId: String,
    @SerialName("session_nonce") val sessionNonce: Long,
    @SerialName("pack_id") val packId: Long,
    val appearance: Int,
    @SerialName("evolution_stage") val evolutionStage: Int,
    val bond: Int,
    val mood: Int,
    val emote: Int,
    val rssi: Double,
    val snr: Double,
    @SerialName("last_seen_age_ms") val lastSeenAgeMs: Long,
    @SerialName("next_sequence") val nextSequence: Long,
) {
    /** Session-bound identity suitable for Compose list keys; not a MeshCore peer ID. */
    val sessionKey: String get() = "$deviceId:$sessionNonce"
}

@Serializable
data class NearbyKitsuPage(
    val schema: String,
    val items: List<NearbyKitsu> = emptyList(),
    /**
     * Actions the authenticated firmware accepts for this roster snapshot.
     *
     * The v1 firmware contract predates this capability field and only accepts Pet,
     * so omission deliberately keeps the safe legacy behavior. New actions remain
     * unavailable until firmware advertises them explicitly.
     */
    @SerialName("supported_actions")
    val supportedActions: List<NeighborInteractionKind> = listOf(NeighborInteractionKind.PET),
)

@Serializable
enum class NeighborInteractionKind {
    @SerialName("pet") PET,
    @SerialName("greet") GREET,
    @SerialName("play") PLAY,
    @SerialName("gift") GIFT,
}

@Serializable
data class NeighborInteractionCommand(
    @SerialName("action_id") val actionId: String,
    @SerialName("target_device_id") val targetDeviceId: String,
    @SerialName("target_session_nonce") val targetSessionNonce: Long,
    val sequence: Long,
    val kind: NeighborInteractionKind,
    @SerialName("expires_at_epoch") val expiresAtEpoch: Long,
)

@Serializable
data class NeighborInteractionReceipt(
    val schema: String,
    @SerialName("action_id") val actionId: String,
    val accepted: Boolean,
    val state: String,
    @SerialName("error_code") val errorCode: String? = null,
)

object EncounterCodePolicy {
    const val MAX_PAGE_SIZE = 100
    const val MAX_VAULT_RECORDS = 256
    const val MAX_VAULT_PLAINTEXT_BYTES = 256 * 1024
    const val UINT32_MAX = 4_294_967_295L

    private val deviceId = Regex("^KT[0-9A-F]{4}$")
    private val opaqueId = Regex("^[\\x21-\\x7E]{1,64}$")
    private val unlockCode =
        Regex("^K8-[0123456789ABCDEFGHJKMNPQRSTVWXYZ]{5}-[0123456789ABCDEFGHJKMNPQRSTVWXYZ]{5}-[0123456789ABCDEFGHJKMNPQRSTVWXYZ]{5}$")
    private val sourceToken = Regex("^[a-z0-9][a-z0-9_.:-]{0,63}$")

    fun validationError(value: EncounterUnlockCode): String? = when {
        !deviceId.matches(value.deviceId) -> "invalid_encounter_device_id"
        !opaqueId.matches(value.codeId) -> "invalid_encounter_code_id"
        !validCode(value.code) -> "invalid_encounter_code"
        value.packId?.let { it !in 0L..UINT32_MAX } == true -> "invalid_encounter_pack_id"
        value.creatureName?.let(::validCreatureName) == false -> "invalid_encounter_creature_name"
        value.packId == null && value.creatureName == null -> "encounter_identity_required"
        value.source?.let(sourceToken::matches) == false -> "invalid_encounter_source"
        value.acquiredAtEpoch !in 0L..UINT32_MAX -> "invalid_encounter_time"
        else -> null
    }

    fun validDeviceId(value: String): Boolean = deviceId.matches(value)
    fun validOpaqueCursor(value: String): Boolean = opaqueId.matches(value)
    fun validCode(value: String): Boolean = unlockCode.matches(value)

    private fun validCreatureName(value: String): Boolean {
        val bytes = value.toByteArray(Charsets.UTF_8)
        return value.isNotBlank() && bytes.size in 1..48 && value.none(Char::isISOControl)
    }
}

object NeighborInteractionPolicy {
    private val errorToken = Regex("^[a-z][a-z0-9_]{0,63}$")

    fun validationError(command: NeighborInteractionCommand): String? = when {
        runCatching { UUID.fromString(command.actionId) }.isFailure -> "invalid_neighbor_action_id"
        !EncounterCodePolicy.validDeviceId(command.targetDeviceId) -> "invalid_neighbor_target"
        command.targetSessionNonce !in 1L..EncounterCodePolicy.UINT32_MAX ->
            "invalid_neighbor_session_nonce"
        command.sequence !in 1L..NearbyKitsuPolicy.UINT16_MAX -> "invalid_neighbor_sequence"
        command.expiresAtEpoch !in 1L..EncounterCodePolicy.UINT32_MAX -> "invalid_neighbor_expiry"
        else -> null
    }

    fun validErrorCode(value: String): Boolean = errorToken.matches(value)
}

/** Exact bounds exported by the firmware's eight-entry, two-minute nearby roster. */
object NearbyKitsuPolicy {
    const val MAX_ITEMS = 8
    const val MAX_LAST_SEEN_AGE_MS = 120_000L
    const val UINT16_MAX = 65_535L

    fun validationError(value: NearbyKitsu): String? = when {
        !EncounterCodePolicy.validDeviceId(value.deviceId) -> "invalid_neighbor_device_id"
        value.sessionNonce !in 1L..EncounterCodePolicy.UINT32_MAX -> "invalid_neighbor_session_nonce"
        value.packId !in 1L..EncounterCodePolicy.UINT32_MAX -> "invalid_neighbor_pack_id"
        value.appearance !in 0..31 -> "invalid_neighbor_appearance"
        value.evolutionStage !in 0..7 -> "invalid_neighbor_evolution_stage"
        value.bond !in 0..100 -> "invalid_neighbor_bond"
        value.mood !in 0..15 -> "invalid_neighbor_mood"
        value.emote !in 0..15 -> "invalid_neighbor_emote"
        !value.rssi.isFinite() || !value.snr.isFinite() -> "invalid_neighbor_signal"
        value.lastSeenAgeMs !in 0L..MAX_LAST_SEEN_AGE_MS -> "stale_neighbor"
        value.nextSequence !in 1L..UINT16_MAX -> "invalid_neighbor_sequence"
        else -> null
    }
}
