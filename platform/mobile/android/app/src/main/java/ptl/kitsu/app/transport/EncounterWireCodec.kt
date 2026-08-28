package ptl.kitsu.app.transport

import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.encodeToJsonElement
import kotlinx.serialization.json.jsonObject
import ptl.kitsu.app.model.ENCOUNTER_CODES_SCHEMA
import ptl.kitsu.app.model.ENCOUNTER_CATALOG_SCHEMA
import ptl.kitsu.app.model.ENCOUNTER_NEIGHBORS_SCHEMA
import ptl.kitsu.app.model.EncounterCatalogPage
import ptl.kitsu.app.model.EncounterCatalogPolicy
import ptl.kitsu.app.model.EncounterCodePage
import ptl.kitsu.app.model.EncounterCodePolicy
import ptl.kitsu.app.model.NearbyKitsuPage
import ptl.kitsu.app.model.NearbyKitsuPolicy
import ptl.kitsu.app.model.NeighborInteractionCommand
import ptl.kitsu.app.model.NeighborInteractionKind
import ptl.kitsu.app.model.NeighborInteractionPolicy
import ptl.kitsu.app.model.NeighborInteractionReceipt
import ptl.kitsu.app.model.NEIGHBOR_ACTION_RECEIPT_SCHEMA
import ptl.kitsu.app.model.PUBLIC_ENCOUNTER_CATALOG

/** Forward-compatible mapper for the authenticated encounter operations. */
internal object EncounterWireCodec {
    private val json = Json {
        ignoreUnknownKeys = true
        explicitNulls = false
    }

    fun codePage(payload: ByteArray): EncounterCodePage {
        val page = try {
            json.decodeFromString<EncounterCodePage>(payload.toString(Charsets.UTF_8))
        } catch (failure: Throwable) {
            throw TransportException("malformed_encounter_codes", failure)
        }
        if (page.schema != ENCOUNTER_CODES_SCHEMA || page.items.size > EncounterCodePolicy.MAX_PAGE_SIZE ||
            page.items.any { EncounterCodePolicy.validationError(it) != null } ||
            page.items.map { it.vaultKey }.distinct().size != page.items.size ||
            page.cursor?.let(EncounterCodePolicy::validOpaqueCursor) == false ||
            (page.hasMore && page.cursor == null)
        ) {
            throw TransportException("malformed_encounter_codes")
        }
        return page.copy(
            items = page.items.map { item ->
                if (item.installed && !item.redeemed) item.copy(redeemed = true) else item
            },
        )
    }

    fun catalog(payload: ByteArray): EncounterCatalogPage {
        val page = try {
            json.decodeFromString<EncounterCatalogPage>(payload.toString(Charsets.UTF_8))
        } catch (failure: Throwable) {
            throw TransportException("malformed_encounter_catalog", failure)
        }
        if (
            page.schema != ENCOUNTER_CATALOG_SCHEMA ||
            !EncounterCatalogPolicy.isExactPublicCatalog(page.items)
        ) {
            throw TransportException("malformed_encounter_catalog")
        }
        // Firmware order is not UI state; retain the canonical rarity progression.
        return page.copy(items = PUBLIC_ENCOUNTER_CATALOG)
    }

    fun neighborActionBody(command: NeighborInteractionCommand): JsonObject {
        NeighborInteractionPolicy.validationError(command)?.let { code ->
            throw TransportException(code)
        }
        return json.encodeToJsonElement(command).jsonObject
    }

    fun nearbyKitsu(payload: ByteArray): NearbyKitsuPage {
        val page = try {
            json.decodeFromString<NearbyKitsuPage>(payload.toString(Charsets.UTF_8))
        } catch (failure: Throwable) {
            throw TransportException("malformed_encounter_neighbors", failure)
        }
        if (
            page.schema != ENCOUNTER_NEIGHBORS_SCHEMA ||
            page.items.size > NearbyKitsuPolicy.MAX_ITEMS ||
            page.items.any { NearbyKitsuPolicy.validationError(it) != null } ||
            page.items.map { it.deviceId }.distinct().size != page.items.size ||
            page.supportedActions.isEmpty() ||
            page.supportedActions.distinct().size != page.supportedActions.size ||
            NeighborInteractionKind.PET !in page.supportedActions
        ) {
            throw TransportException("malformed_encounter_neighbors")
        }
        return page.copy(items = page.items.sortedBy { it.lastSeenAgeMs })
    }

    fun neighborActionReceipt(
        payload: ByteArray,
        command: NeighborInteractionCommand,
    ): NeighborInteractionReceipt {
        val receipt = try {
            json.decodeFromString<NeighborInteractionReceipt>(payload.toString(Charsets.UTF_8))
        } catch (failure: Throwable) {
            throw TransportException("malformed_neighbor_action_receipt", failure)
        }
        val validState = if (receipt.accepted) {
            receipt.state in setOf("queued", "delivered", "applied") && receipt.errorCode == null
        } else {
            receipt.state == "rejected" &&
                receipt.errorCode?.let(NeighborInteractionPolicy::validErrorCode) == true
        }
        if (receipt.schema != NEIGHBOR_ACTION_RECEIPT_SCHEMA || receipt.actionId != command.actionId ||
            !validState
        ) {
            throw TransportException("malformed_neighbor_action_receipt")
        }
        if (!receipt.accepted) throw TransportException(requireNotNull(receipt.errorCode))
        return receipt
    }
}
