package ptl.kitsu.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import java.util.Base64
import kotlin.math.min

internal data class NearbyCreaturePresentation(
    val name: String,
    val width: Int,
    val height: Int,
    val bitmap: ByteArray,
    val known: Boolean = true,
)

/** Static, monochrome portraits derived from the matching K868 packs/catalog entries. */
internal fun nearbyCreaturePresentation(packId: Long): NearbyCreaturePresentation =
    NEARBY_CREATURES[packId] ?: UNKNOWN_CREATURE

internal fun nearbyMoodLabel(value: Int): String = MOOD_LABELS.getOrNull(value) ?: "Unknown mood"

internal fun nearbyStageLabel(value: Int): String = STAGE_LABELS.getOrNull(value) ?: "Unknown stage"

internal fun nearbyLastSeenLabel(ageMs: Long): String = when {
    ageMs < 5_000L -> "just now"
    ageMs < 60_000L -> "${ageMs / 1_000L}s ago"
    else -> "${ageMs / 60_000L}m ago"
}

@Composable
internal fun NearbyKitsuPortrait(
    creature: NearbyCreaturePresentation,
    modifier: Modifier = Modifier,
) {
    val ink = MaterialTheme.colorScheme.onSecondaryContainer
    val pixelBackground = MaterialTheme.colorScheme.secondaryContainer
    Surface(
        modifier = modifier.semantics {
            contentDescription = if (creature.known) {
                "${creature.name} portrait"
            } else {
                "Unknown Kitsu portrait"
            }
        },
        color = pixelBackground,
        shape = MaterialTheme.shapes.medium,
    ) {
        PixelPortrait(
            bitmap = creature.bitmap,
            width = creature.width,
            height = creature.height,
            color = ink,
            modifier = Modifier.fillMaxSize().padding(8.dp),
        )
    }
}

@Composable
private fun PixelPortrait(
    bitmap: ByteArray,
    width: Int,
    height: Int,
    color: Color,
    modifier: Modifier,
) {
    Canvas(modifier) {
        val pixelSize = min(size.width / width, size.height / height)
        val left = (size.width - width * pixelSize) / 2f
        val top = (size.height - height * pixelSize) / 2f
        val rowBytes = (width + 7) / 8
        for (y in 0 until height) {
            for (x in 0 until width) {
                val byte = bitmap[y * rowBytes + x / 8].toInt() and 0xff
                if (byte and (1 shl (x and 7)) != 0) {
                    drawRect(
                        color = color,
                        topLeft = Offset(left + x * pixelSize, top + y * pixelSize),
                        size = Size(pixelSize + 0.1f, pixelSize + 0.1f),
                    )
                }
            }
        }
    }
}

private fun portrait(
    name: String,
    width: Int,
    height: Int,
    encoded: String,
    known: Boolean = true,
): NearbyCreaturePresentation {
    val decoded = Base64.getDecoder().decode(encoded)
    require(decoded.size == ((width + 7) / 8) * height) { "invalid_nearby_portrait" }
    return NearbyCreaturePresentation(name, width, height, decoded, known)
}

private val NEARBY_CREATURES = mapOf(
    0xFDC79D6FL to portrait(
        "Cat", 64, 64,
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAcAAAAA8AAADYAQDACQAAAAgDAMAIAAAADAIAIBgAAAAEBAAwEAAAAEQIAAgRAAAARBgAjBAAAAAE0X9GEAAAAATj8QMQAAAARIMxohAAAADEwjngEAAAAITBOMAQAAAAhMAYwBAAAABMgAjAEQAAACgAAAAJAAAAKAAAAAgAAAAYAAAADgAAABAAAAAGAAAAEAAAAAYAAAAYAAAABAAAAAgHgAMEAAAADA/AAwwAAAAMD8AHGAAAAAcPwAMYAAAABw/AAxgAAAAOBoADDAAAABwEAMEHAAAAB8ADABgAAAAsAAEgBgAAABgAAcADAAAANgAAwAMAAABgAADgAAAAAIADADgAAAAAABwAGAAfAAAABAAwgHEAAAAEAGDAwAAAAAIAgEGAAAAAAgAAQYABAAACAABCDAEAAAQAAMYLAQAABAAADAgBAAAUAAAICAEAABQACBgIAQAAHAAIEAwBAAA8CAwwhgEAAGgIDCCDAAAAyAgE4MEAAACIDwQgYAAAABgPBiBgAAAAEA4GIDAAAAAQDgYgHAAAABAOH+AHAAAAEAY/MAAAAAAIAmcQAAAAAAQDAxgAAAAAJIUXDAAAAAAsxxYGAAAAAPh//AMAAAAAAAAAAAAAAAAAAAAAAAA=",
    ),
    0x6C393E21L to portrait(
        "Fox", 64, 64,
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMAAAMAAAAABwAAA8AAAAAPgAAHwAAAAA2AMAbwAAAACYB4BnAAAAAAgPgEMAAAAASA/ASQAAAACIHuBFAAAAAIj890QAAAAASAUASgAAAACIAQBGAAAAAJgAAEQAAAAAkAAAZgAAAABQAAAkAAAAACAAACgAAAAAIAAAEAAAAAAQAAAQAAAAABAAAAAAAAAAEAAAAAAAAAAQAxggAAAAAAwHOGAAgAMABgc4wABgAgAEBzhgADACABgDGDAADAYABgAAwAAEBAAccABgAAIMADgwADgAAQgAMCAAHAABCACAEcADAAMAAAAHcP8ABBAAAAwAAIf4HQAABAAAhAAWAAACAABIABIAAAYAAHgAEAAABAAAAAAQAAAEAAAgABgAAAwAACAAAAAACAAAIAAIAAAYAAIgAAwAABAAAiAABgAAIAAiYAADAACAAD6gYQAAABABb2A/AAAAEI5HQAAAAAAwiIUBAAAAAPD5icMAAAAA8Pn54QAAAADw+fj5AAAAAPD9+PgAAAAA/H/8/AAAAAD8fnx8AAAAAHg8OHgAAAAAAAAAAAAAAAAAAAAAAAA=",
    ),
    0xE2B5E7BAL to portrait(
        "Dog", 64, 64,
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPADAAAAAAAADBwAAAAAAHgPPB8AAACA/wPwfwAAAIC/AXD/AAAA4N8B8P8BAADg3wDg/gMAAPDvAMD+BwAA8G8AwP4HAAD4bwCA/Q8AAPg/AID9DwAA+D8AgP0PAAD4Pwec/Q8AAPg3B5z7DwAA8DcHnPsHAADwOwKc/wcAAOA7AIj3AwAAwBgAgOcBAAAAGPgA5wAAAAAY8AADABwAABBgAAEAFAAAECAAAQAgAABgZIIAACIAAEAAwQMAIwAAgAHwfoAnAACAB3z/wScAAMD+v///HwAAQP7H/+8PAABAYMD/3wcAAEBggP+/AwAAYPCB//8AAABgYIH//wAAAGDwwf//AQAAYPDA//8BAADgAMD/+QEAAMABwP/4AwAAwAGA//gDAADAAYBv8AMAAMAFAgfgAwAAwBkCh8ADAADA8QLEgAMAAMAgA+QBAgAAQCACPgMCAABAIAImBgAAAEAAAiYEAAAAYCACNgAAAAAQAAISAAAAABAQAxIEAAAACBABCgIAAACIGCEGSgIAAIgIJQNOAwAA8Af+APgBAAAAAAAAAAAAAAAAAAAAAAA=",
    ),
    0x5CAC86A3L to portrait("Frog", 16, 18, "AAAAADgcVCr8Pzw8FCiGYQJA5CccOB54EkhySqQt/n8AAAAA"),
    0x13793DC7L to portrait("Hamster", 16, 18, "AAA8PO432jsOKAQwNiwybjJOhnGOcQZgIkZyTiZkBDD8PwAA"),
    0x7495DBFBL to portrait("Turtle", 16, 18, "AAAAAAAAAAA8AEIGwhl2GUovZnn8f/h/aC/4OQAAAAAAAAAA"),
    0x68D9554EL to portrait("Rabbit", 16, 18, "AAAYBygHuAbYBdgHGAMIApgCmAP4AhAHEA2wGfAY4B3wBwAA"),
    0x5DF6BE74L to portrait("Hedgehog", 16, 18, "AAAAAAAA4AfwH/g/+D/8f8R/lH8Sfw5+eDfgDgAAAAAAAAAA"),
    0xE59408E0L to portrait("Ferret", 16, 18, "AAAAAAAAAAAAAD4APAA+BewIFBA8Evh/PH8AAAAAAAAAAAAA"),
    0x29B4B2F7L to portrait("Otter", 16, 18, "AAD4Af4H/gf+A/4CMgOEA4QHxAfMD+wP/A/8P9x//H8AAAAA"),
    0x69276D0CL to portrait("Axolotl", 16, 18, "AAAAAAAAAAAAAAgD+h8OOwZC3mMAXfBvODEAAAAAAAAAAAAA"),
    0x2DFB0797L to portrait("Chinchilla", 16, 18, "AAAAAI4D8gQaDAYTBCPKQupGJEUERrxE/GRIJPwe/AcAAAAA"),
    0xC163EFEDL to portrait("Raccoon", 16, 18, "AAAAAAwH/gdsBvQE+gX+D74/LH4YTQh8EGlwWXA5+B8AAAAA"),
    0x374D2540L to portrait("Capybara", 16, 18, "AAAAAAAA+ADIAKYfJiAGQAZADEAIQHBPcDvwPgAAAAAAAAAA"),
    0x39FC5B1AL to portrait("Sugar Glider", 16, 18, "AAAAAAAAAAAAAMYB/gZ+GPZvrF7sV+ZaADwAAAAAAAAAAAAA"),
    0x91A2DE7BL to portrait("Red Panda", 16, 18, "AAAAAAAAAAAAAPYAxgC6A/4P9h/8f/h//HcAAAAAAAAAAAAA"),
    0xE04EC405L to portrait("Pangolin", 16, 18, "AAAAAAAAAAAAAAAA4AP4D/wf/j86ewAAAAAAAAAAAAAAAAAA"),
    0x8E0E1B03L to portrait("Tasmanian Devil", 16, 18, "AAAAAAAAAAAAAD4AfgD+B74P/g/oH/h/+C8AAAAAAAAAAAAA"),
    0x533B9B30L to portrait("Snow Leopard", 16, 18, "AAAAAAAA/AOuA/YDQgNOEhIrzH+Ed8h/uH2sf/wXAAAAAAAA"),
    0x86F3BB5DL to portrait("Okapi", 16, 18, "AADGAO4A/gD+AP4A/gfsH/4//H/wf+B/4H/APcA9YD3gNwAA"),
    0x2D1D89AFL to portrait("Shoebill", 16, 18, "AADgAxAGUALwAvAA2AFIA7gEeA9gC8APwA7AAcAC4APgAwAA"),
    0xA52160C5L to portrait("Cat Girl", 16, 18, "AAAOB4oF+gVWBgICmgaaBiYG/DEIKYw7DBsMF/gPaAH4AQAA"),
    0xF0F750BDL to portrait("Rabbit Girl", 16, 18, "AAAwBkgJeAvwB3AFEAQICGgLiAhwB/AHMA44DvAH4APgAwAA"),
    0x52A1C03AL to portrait("Deer Girl", 16, 18, "AAAwDHAOcA78P/wtHDhwDlAKYAbgB2AGUBowHOAfwAPAAwAA"),
)

private val UNKNOWN_CREATURE = portrait(
    name = "Unknown Kitsu",
    width = 16,
    height = 18,
    encoded = "CBAcOP5/AkDCRyJIIgBEAIgAgAAAAIAAAAAEIAgQ8A8AAAAA",
    known = false,
)

private val MOOD_LABELS = listOf(
    "Content", "Dreaming", "Listening", "Drowsy", "Lonely", "Curious", "Excited",
    "Devoted", "Impish", "Loved", "Satisfied", "Playful", "Proud", "Startled", "Awake",
)

private val STAGE_LABELS = listOf("New", "Familiar", "Trusted", "Resonant", "Ascended")
