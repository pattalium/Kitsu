package ptl.kitsu.app.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import ptl.kitsu.app.R

class FieldGuidePortraitsTest {
    @Test
    fun everyPublicWildPackUsesItsMatchingIdlePortrait() {
        val expected = linkedMapOf(
            0x5CAC86A3L to R.drawable.guide_frog_idle,
            0x13793DC7L to R.drawable.guide_hamster_idle,
            0x7495DBFBL to R.drawable.guide_turtle_idle,
            0x68D9554EL to R.drawable.guide_rabbit_idle,
            0x5DF6BE74L to R.drawable.guide_hedgehog_idle,
            0xE59408E0L to R.drawable.guide_ferret_idle,
            0x29B4B2F7L to R.drawable.guide_otter_idle,
            0x69276D0CL to R.drawable.guide_axolotl_idle,
            0x2DFB0797L to R.drawable.guide_chinchilla_idle,
            0xC163EFEDL to R.drawable.guide_raccoon_idle,
            0x374D2540L to R.drawable.guide_capybara_idle,
            0x39FC5B1AL to R.drawable.guide_sugar_glider_idle,
            0x91A2DE7BL to R.drawable.guide_red_panda_idle,
            0xE04EC405L to R.drawable.guide_pangolin_idle,
            0x8E0E1B03L to R.drawable.guide_tasmanian_devil_idle,
            0x533B9B30L to R.drawable.guide_snow_leopard_idle,
            0x86F3BB5DL to R.drawable.guide_okapi_idle,
            0x2D1D89AFL to R.drawable.guide_shoebill_idle,
            0xA52160C5L to R.drawable.guide_cat_girl_idle,
            0xF0F750BDL to R.drawable.guide_rabbit_girl_idle,
            0x52A1C03AL to R.drawable.guide_deer_girl_idle,
        )

        assertEquals(expected, fieldGuidePortraitResources)
        assertEquals(21, fieldGuidePortraitResources.values.distinct().size)
    }

    @Test
    fun unknownPackFallsBackToTheCompactCatalogPortrait() {
        assertNull(fieldGuidePortraitResource(0x12345678L))
    }
}
