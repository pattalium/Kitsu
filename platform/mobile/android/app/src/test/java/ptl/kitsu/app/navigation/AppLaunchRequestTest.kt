package ptl.kitsu.app.navigation

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import ptl.kitsu.app.automation.KitsuAutomationAction
import ptl.kitsu.app.security.ControllerRole

class AppLaunchRequestTest {
    @Test fun shareTargetAcceptsOnlyNonBlankPlainTextAndNeverCreatesAnAction() {
        assertTrue(
            AppLaunchIntentPolicy.parse("android.intent.action.VIEW", "text/plain", "hello")
                is AppLaunchIntentResult.Ignored,
        )
        assertTrue(
            AppLaunchIntentPolicy.parse(AppLaunchIntentPolicy.ACTION_SEND, "image/png", "hello")
                is AppLaunchIntentResult.Rejected,
        )
        assertTrue(
            AppLaunchIntentPolicy.parse(AppLaunchIntentPolicy.ACTION_SEND, "text/plain", " \n\t")
                is AppLaunchIntentResult.Rejected,
        )

        val accepted = AppLaunchIntentPolicy.parse(
            AppLaunchIntentPolicy.ACTION_SEND,
            "TEXT/PLAIN; charset=utf-8",
            "hello from another app",
        ) as AppLaunchIntentResult.Accepted
        assertEquals(AppRoute.Messages(), accepted.spec.route)
        assertEquals("hello from another app", accepted.spec.messageDraft)
        assertFalse(accepted.sharedTextShortened)
    }

    @Test fun importedTextIsUtf8BoundedWithoutSplittingAnEmoji() {
        val value = "🦊".repeat(32) + "discarded"
        val accepted = AppLaunchIntentPolicy.parse(
            AppLaunchIntentPolicy.ACTION_SEND,
            "text/plain",
            value,
        ) as AppLaunchIntentResult.Accepted

        assertEquals("🦊".repeat(32), accepted.spec.messageDraft)
        assertEquals(128, accepted.spec.messageDraft!!.toByteArray(Charsets.UTF_8).size)
        assertTrue(accepted.sharedTextShortened)
    }

    @Test fun hostileControlCharactersAreRemovedButNewlinesRemainEditable() {
        val draft = IncomingTextSharePolicy.prepare("one\u0000\u0007\ntwo")!!
        assertEquals("one\ntwo", draft.text)
    }

    @Test fun consumeUsesExactRequestIdentityAndCannotClearANewerIntent() {
        val coordinator = AppLaunchRequestCoordinator()
        val first = coordinator.submit(AppLaunchSpec(AppRoute.Home))
        val second = coordinator.submit(AppLaunchSpec(AppRoute.Messages(), "draft"))

        assertFalse(coordinator.consume(first.id))
        assertEquals(second, coordinator.pending.value)
        assertTrue(coordinator.consume(second.id))
        assertNull(coordinator.pending.value)
        assertFalse(coordinator.consume(second.id))
    }

    @Test fun widgetOpenActionSelectsOnlyTheVisibleHomeRoute() {
        val accepted = AppLaunchIntentPolicy.parse(
            action = AppLaunchIntentPolicy.ACTION_OPEN_HOME,
            mimeType = "application/octet-stream",
            sharedText = "must not become a draft",
        ) as AppLaunchIntentResult.Accepted

        assertEquals(AppRoute.Home, accepted.spec.route)
        assertNull(accepted.spec.messageDraft)
        assertFalse(accepted.sharedTextShortened)
    }

    @Test fun notificationThreadActionRequiresACanonicalThreadAndNeverCarriesADraft() {
        val peer = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        val accepted = AppLaunchIntentPolicy.parse(
            action = AppLaunchIntentPolicy.ACTION_OPEN_MESSAGES,
            mimeType = null,
            sharedText = "ignored",
            routeThreadKey = "direct:$peer",
        ) as AppLaunchIntentResult.Accepted
        assertEquals(AppRoute.Messages("direct:$peer"), accepted.spec.route)
        assertNull(accepted.spec.messageDraft)

        assertTrue(
            AppLaunchIntentPolicy.parse(
                action = AppLaunchIntentPolicy.ACTION_OPEN_MESSAGES,
                mimeType = null,
                sharedText = null,
                routeThreadKey = "direct:not-a-key",
            ) is AppLaunchIntentResult.Rejected,
        )
    }

    @Test fun companionAndAutomationRoutesOnlySelectVisibleDestinations() {
        val studio = AppLaunchIntentPolicy.parse(
            action = AppLaunchIntentPolicy.ACTION_OPEN_COMPANION,
            mimeType = null,
            sharedText = null,
            companionDestination = "studio",
        ) as AppLaunchIntentResult.Accepted
        assertEquals(
            AppRoute.Companion(CompanionDestination.STUDIO),
            studio.spec.route,
        )

        val pet = AppLaunchIntentPolicy.parse(
            action = AppLaunchIntentPolicy.ACTION_CONFIRM_AUTOMATION,
            mimeType = null,
            sharedText = "must be ignored",
            automationAction = "pet",
        ) as AppLaunchIntentResult.Accepted
        assertEquals(AppRoute.Automation(KitsuAutomationAction.PET), pet.spec.route)
        assertNull(pet.spec.messageDraft)

        assertTrue(
            AppLaunchIntentPolicy.parse(
                action = AppLaunchIntentPolicy.ACTION_CONFIRM_AUTOMATION,
                mimeType = null,
                sharedText = null,
                automationAction = "send_message",
            ) is AppLaunchIntentResult.Rejected,
        )
    }

    @Test fun caretakerGuideRoutesResolveToTheSafeCompanionOverview() {
        assertEquals(
            CompanionDestination.OVERVIEW,
            CompanionDestinationPolicy.resolve(
                ControllerRole.CARETAKER,
                CompanionDestination.GUIDE,
            ),
        )
        assertFalse(
            CompanionDestinationPolicy.allows(
                ControllerRole.CARETAKER,
                CompanionDestination.GUIDE,
            ),
        )
        listOf(
            CompanionDestination.OVERVIEW,
            CompanionDestination.ACCESSIBILITY,
            CompanionDestination.STUDIO,
        ).forEach { destination ->
            assertEquals(
                destination,
                CompanionDestinationPolicy.resolve(ControllerRole.CARETAKER, destination),
            )
        }
        assertEquals(
            CompanionDestination.GUIDE,
            CompanionDestinationPolicy.resolve(ControllerRole.OWNER, CompanionDestination.GUIDE),
        )
    }
}
