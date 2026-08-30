package ptl.kitsu.app.widget

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.view.View
import android.widget.RemoteViews
import ptl.kitsu.app.MainActivity
import ptl.kitsu.app.R
import ptl.kitsu.app.navigation.AppLaunchIntentPolicy
import ptl.kitsu.app.ui.nearbyCreaturePresentation

object KitsuStatusWidgetIntents {
    fun openHome(context: Context): Intent = Intent(context, MainActivity::class.java)
        .setAction(AppLaunchIntentPolicy.ACTION_OPEN_HOME)
        .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)

    fun openHomePendingIntent(context: Context): PendingIntent = PendingIntent.getActivity(
        context,
        OPEN_HOME_REQUEST_CODE,
        openHome(context),
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
    )

    private const val OPEN_HOME_REQUEST_CODE = 0x4B5357
}

object KitsuStatusWidgetRenderer {
    fun render(
        context: Context,
        presentation: KitsuStatusWidgetPresentation,
    ): RemoteViews = RemoteViews(context.packageName, R.layout.kitsu_status_widget).apply {
        val portrait = KitsuStatusWidgetPortraitRenderer.render(presentation.portraitPackId)
        setTextViewText(R.id.kitsu_widget_name, presentation.petName)
        setTextViewText(R.id.kitsu_widget_mood, presentation.moodText)
        setTextViewText(R.id.kitsu_widget_energy, presentation.energyText)
        setProgressBar(
            R.id.kitsu_widget_energy_progress,
            100,
            presentation.energyPercent.coerceIn(0, 100),
            false,
        )
        setTextViewText(R.id.kitsu_widget_battery, presentation.batteryText)
        setTextViewText(R.id.kitsu_widget_freshness, presentation.freshnessText)
        setOptionalText(R.id.kitsu_widget_request, presentation.requestText)
        setOptionalText(R.id.kitsu_widget_focus, presentation.focusText)
        setOptionalText(R.id.kitsu_widget_walk, presentation.walkText)
        setImageViewBitmap(
            R.id.kitsu_widget_portrait,
            portrait.bitmap,
        )
        setContentDescription(
            R.id.kitsu_widget_portrait,
            if (!portrait.known) {
                context.getString(R.string.kitsu_widget_default_portrait)
            } else {
                "${presentation.petName} portrait"
            },
        )
        setContentDescription(R.id.kitsu_widget_root, presentation.contentDescription)
        setOnClickPendingIntent(
            R.id.kitsu_widget_root,
            KitsuStatusWidgetIntents.openHomePendingIntent(context),
        )
    }

    private fun RemoteViews.setOptionalText(viewId: Int, text: String?) {
        setViewVisibility(viewId, if (text == null) View.GONE else View.VISIBLE)
        if (text != null) setTextViewText(viewId, text)
    }
}

internal data class KitsuStatusWidgetPortrait(
    val bitmap: Bitmap,
    val known: Boolean,
)

internal object KitsuStatusWidgetPortraitRenderer {
    fun render(packId: Long?): KitsuStatusWidgetPortrait {
        val creature = nearbyCreaturePresentation(packId ?: Long.MIN_VALUE)
        val bitmap = Bitmap.createBitmap(creature.width, creature.height, Bitmap.Config.ARGB_8888)
        val rowBytes = (creature.width + 7) / 8
        for (y in 0 until creature.height) {
            for (x in 0 until creature.width) {
                val byte = creature.bitmap[y * rowBytes + x / 8].toInt() and 0xff
                bitmap.setPixel(
                    x,
                    y,
                    if (byte and (1 shl (x and 7)) != 0) PORTRAIT_INK else Color.TRANSPARENT,
                )
            }
        }
        return KitsuStatusWidgetPortrait(bitmap = bitmap, known = creature.known)
    }

    private const val PORTRAIT_INK = 0xFFF09A68.toInt()
}
