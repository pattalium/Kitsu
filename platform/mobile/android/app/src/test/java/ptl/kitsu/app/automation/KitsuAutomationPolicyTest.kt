package ptl.kitsu.app.automation

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class KitsuAutomationPolicyTest {
    private val token = "A".repeat(43)

    @Test
    fun exactSafeActionAndCapabilityAreAccepted() {
        val request = KitsuAutomationPolicy.parse(
            action = KitsuAutomationPolicy.ACTION_AUTOMATE,
            hasData = false,
            hasClipData = false,
            mimeType = null,
            categories = emptySet(),
            extras = exactExtras("focus_25"),
        )

        assertEquals(KitsuAutomationAction.FOCUS_25, request?.action)
        assertEquals(token, request?.capabilityToken)
    }

    @Test
    fun meshFirmwareAndExtraFieldRequestsFailClosed() {
        listOf("send_message", "advertise", "firmware", "pair", "codes", "pack").forEach { action ->
            assertNull(
                KitsuAutomationPolicy.parse(
                    action = KitsuAutomationPolicy.ACTION_AUTOMATE,
                    hasData = false,
                    hasClipData = false,
                    mimeType = null,
                    categories = emptySet(),
                    extras = exactExtras(action),
                ),
            )
        }
        assertNull(
            KitsuAutomationPolicy.parse(
                action = KitsuAutomationPolicy.ACTION_AUTOMATE,
                hasData = false,
                hasClipData = false,
                mimeType = null,
                categories = emptySet(),
                extras = exactExtras("pet") + ("unexpected" to true),
            ),
        )
    }

    @Test
    fun malformedTokenAndImplicitPayloadsFailClosed() {
        assertNull(
            KitsuAutomationPolicy.parse(
                action = KitsuAutomationPolicy.ACTION_AUTOMATE,
                hasData = false,
                hasClipData = false,
                mimeType = null,
                categories = emptySet(),
                extras = exactExtras("pet") +
                    (KitsuAutomationPolicy.EXTRA_CAPABILITY to "short"),
            ),
        )
        assertNull(
            KitsuAutomationPolicy.parse(
                action = KitsuAutomationPolicy.ACTION_AUTOMATE,
                hasData = false,
                hasClipData = false,
                mimeType = "text/plain",
                categories = emptySet(),
                extras = exactExtras("pet"),
            ),
        )
    }

    @Test
    fun taskerRecipeContainsTheExactExplicitComponentActionExtrasAndAllowlist() {
        val recipe = KitsuAutomationPolicy.setupRecipe("ptl.kitsu.app.debug", token)

        assertTrue(
            recipe.contains(
                "Component: ptl.kitsu.app.debug/ptl.kitsu.app.automation.KitsuAutomationActivity",
            ),
        )
        assertTrue(recipe.contains("Action: ${KitsuAutomationPolicy.ACTION_AUTOMATE}"))
        assertTrue(recipe.contains("${KitsuAutomationPolicy.EXTRA_ACTION}: <action>"))
        assertTrue(recipe.contains("${KitsuAutomationPolicy.EXTRA_CAPABILITY}: $token"))
        KitsuAutomationAction.entries.forEach { action ->
            assertTrue(recipe.contains(action.wireName))
        }
    }

    private fun exactExtras(action: String): Map<String, Any?> = mapOf(
        KitsuAutomationPolicy.EXTRA_ACTION to action,
        KitsuAutomationPolicy.EXTRA_CAPABILITY to token,
    )
}
