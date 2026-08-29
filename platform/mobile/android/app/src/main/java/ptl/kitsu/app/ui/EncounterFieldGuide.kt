package ptl.kitsu.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ptl.kitsu.app.EncounterUnlockUiState
import ptl.kitsu.app.model.EncounterCatalogCreature
import ptl.kitsu.app.model.EncounterRarity
import ptl.kitsu.app.model.EncounterUnlockCode
import ptl.kitsu.app.model.PUBLIC_ENCOUNTER_CATALOG
import ptl.kitsu.app.repository.OwnerState

internal enum class FieldGuideDiscovery {
    UNSEEN,
    SEEN,
    OWNED,
}

internal data class FieldGuideEntry(
    val creature: EncounterCatalogCreature,
    val discovery: FieldGuideDiscovery,
    val encounterCount: Int,
    val lastSource: String?,
    val lastEncounterEpoch: Long?,
)

/** Deterministic local projection of authenticated encounter records onto the public 21-pack roster. */
internal object EncounterFieldGuidePolicy {
    val catalog: List<EncounterCatalogCreature> = PUBLIC_ENCOUNTER_CATALOG

    fun build(
        records: List<EncounterUnlockCode>,
        activePackId: Long?,
        catalog: List<EncounterCatalogCreature> = PUBLIC_ENCOUNTER_CATALOG,
    ): List<FieldGuideEntry> = catalog.map { creature ->
        val matching = records.filter { record ->
            record.packId == creature.packId ||
                (record.packId == null && record.creatureName.normalizedName() == creature.name.normalizedName())
        }
        val owned = activePackId == creature.packId || matching.any { it.redeemed || it.installed }
        val lastEncounter = matching.maxByOrNull(EncounterUnlockCode::acquiredAtEpoch)
        val lastSource = matching.asSequence()
            .filter { !it.source.isNullOrBlank() }
            .maxByOrNull(EncounterUnlockCode::acquiredAtEpoch)
            ?.source
        FieldGuideEntry(
            creature = creature,
            discovery = when {
                owned -> FieldGuideDiscovery.OWNED
                matching.isNotEmpty() -> FieldGuideDiscovery.SEEN
                else -> FieldGuideDiscovery.UNSEEN
            },
            encounterCount = matching.size,
            lastSource = lastSource,
            lastEncounterEpoch = lastEncounter?.acquiredAtEpoch,
        )
    }

    private fun String?.normalizedName(): String = this.orEmpty()
        .trim()
        .lowercase()
        .filter(Char::isLetterOrDigit)
}

@Composable
internal fun KitsuFieldGuideScreen(
    owner: OwnerState,
    encounterUnlocks: EncounterUnlockUiState,
    onSync: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val activePackId = owner.status?.takeIf { it.packReady }?.packId?.toLongOrNull()
    val catalog = owner.encounterCatalog.ifEmpty { PUBLIC_ENCOUNTER_CATALOG }
    val entries = remember(encounterUnlocks.records, activePackId, catalog) {
        EncounterFieldGuidePolicy.build(encounterUnlocks.records, activePackId, catalog)
    }
    val owned = entries.count { it.discovery == FieldGuideDiscovery.OWNED }
    val seen = entries.count { it.discovery == FieldGuideDiscovery.SEEN }
    val unseen = entries.size - owned - seen

    LazyColumn(
        modifier = modifier.fillMaxSize().testTag("screen-field-guide"),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(18.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item {
            SectionHeading(
                title = "Encounter field guide",
                supporting = "All 21 catalog creatures, built from saved authenticated encounters and your connected Kitsu.",
            )
        }
        item {
            KitsuCard(modifier = Modifier.testTag("field-guide-summary")) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(
                        modifier = Modifier.weight(1f),
                        verticalArrangement = Arrangement.spacedBy(2.dp),
                    ) {
                        Text("$owned of ${entries.size} owned", style = MaterialTheme.typography.titleLarge)
                        Text(
                            "$seen seen · $unseen still waiting",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Spacer(Modifier.size(10.dp))
                    OutlinedButton(
                        onClick = onSync,
                        enabled = owner.connection.connected && !encounterUnlocks.syncing,
                        modifier = Modifier.testTag("field-guide-sync"),
                    ) {
                        Icon(Icons.Default.Refresh, contentDescription = null, modifier = Modifier.size(18.dp))
                        Spacer(Modifier.size(7.dp))
                        Text(if (encounterUnlocks.syncing) "Syncing…" else "Sync")
                    }
                }
                LinearProgressIndicator(
                    progress = { owned.toFloat() / entries.size.coerceAtLeast(1) },
                    modifier = Modifier.fillMaxWidth().height(8.dp),
                    color = MaterialTheme.colorScheme.primary,
                    trackColor = MaterialTheme.colorScheme.surfaceVariant,
                )
                when {
                    encounterUnlocks.loading -> Text(
                        "Loading saved field notes…",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    encounterUnlocks.errorCode != null -> Text(
                        "Saved encounters need attention: ${encounterUnlocks.errorCode.humanized()}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                    owner.encounterCatalogErrorCode != null -> Text(
                        "Live catalog unavailable; showing the built-in field guide.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    owner.encounterCatalog.isNotEmpty() -> Text(
                        "Live catalog verified by your connected Kitsu.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.primary,
                    )
                    !owner.connection.connected -> Text(
                        "Offline notes are ready. Connect Kitsu to sync new encounters.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        items(entries, key = { it.creature.packId }) { entry ->
            FieldGuideCreatureCard(entry)
        }
        item { Spacer(Modifier.height(4.dp)) }
    }
}

@Composable
private fun FieldGuideCreatureCard(entry: FieldGuideEntry) {
    val creature = entry.creature
    KitsuCard(
        modifier = Modifier.testTag(
            "field-guide-creature-${creature.packId.toString(16).lowercase().padStart(8, '0')}",
        ),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FieldGuideCreaturePortrait(
                packId = creature.packId,
                name = creature.name,
                modifier = Modifier
                    .size(72.dp)
                    .alpha(if (entry.discovery == FieldGuideDiscovery.UNSEEN) 0.42f else 1f),
            )
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        creature.name,
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.titleLarge,
                        fontWeight = FontWeight.SemiBold,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(Modifier.size(8.dp))
                    StatusPill(entry.discovery.label, entry.discovery.tone)
                }
                Text(
                    "${creature.rarity.label} · Pack ${creature.packId.hexPackId()}",
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.primary,
                )
                Text(
                    when (entry.encounterCount) {
                        0 -> if (entry.discovery == FieldGuideDiscovery.OWNED) {
                            "Current Kitsu pack · no saved encounters yet"
                        } else {
                            "No encounters recorded"
                        }
                        1 -> "1 encounter recorded"
                        else -> "${entry.encounterCount} encounters recorded"
                    },
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                entry.lastSource?.let { source ->
                    Text(
                        "Latest source · ${source.humanized()}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

private val EncounterRarity.label: String
    get() = when (this) {
        EncounterRarity.COMMON -> "Common"
        EncounterRarity.UNCOMMON -> "Uncommon"
        EncounterRarity.RARE -> "Rare"
        EncounterRarity.VERY_RARE -> "Very rare"
        EncounterRarity.EPIC -> "Epic"
        EncounterRarity.LEGENDARY -> "Legendary"
        EncounterRarity.MYTHICAL -> "Mythical"
    }

private val FieldGuideDiscovery.label: String
    get() = when (this) {
        FieldGuideDiscovery.UNSEEN -> "Unseen"
        FieldGuideDiscovery.SEEN -> "Seen"
        FieldGuideDiscovery.OWNED -> "Owned"
    }

private val FieldGuideDiscovery.tone: StatusTone
    get() = when (this) {
        FieldGuideDiscovery.UNSEEN -> StatusTone.NEUTRAL
        FieldGuideDiscovery.SEEN -> StatusTone.ACTIVE
        FieldGuideDiscovery.OWNED -> StatusTone.POSITIVE
    }

private fun Long.hexPackId(): String = "0x${toString(16).uppercase().padStart(8, '0')}"
