package ptl.kitsu.app.ui

import android.content.Context
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.shape.RoundedCornerShape

enum class KitsuThemePreference(val storageValue: String) {
    DARK("dark"),
    SYSTEM("system");

    internal fun useDarkColors(systemIsDark: Boolean): Boolean = this == DARK || systemIsDark

    companion object {
        fun fromStorage(value: String?): KitsuThemePreference =
            entries.firstOrNull { it.storageValue == value } ?: DARK
    }
}

class KitsuThemePreferences(context: Context) {
    private val preferences = context.applicationContext.getSharedPreferences(
        PREFERENCES_NAME,
        Context.MODE_PRIVATE,
    )

    fun current(): KitsuThemePreference =
        KitsuThemePreference.fromStorage(preferences.getString(KEY_THEME, null))

    fun set(preference: KitsuThemePreference) {
        preferences.edit().putString(KEY_THEME, preference.storageValue).apply()
    }

    private companion object {
        const val PREFERENCES_NAME = "kitsu_appearance"
        const val KEY_THEME = "theme_preference"
    }
}

private val KitsuDarkColors = darkColorScheme(
    primary = Color(0xFFF09A68),
    onPrimary = Color(0xFF321203),
    primaryContainer = Color(0xFF5A2B18),
    onPrimaryContainer = Color(0xFFFFDBCA),
    secondary = Color(0xFFD9C1D2),
    onSecondary = Color(0xFF2C1827),
    secondaryContainer = Color(0xFF422D3C),
    onSecondaryContainer = Color(0xFFF6DAEC),
    tertiary = Color(0xFFD9C58D),
    onTertiary = Color(0xFF292002),
    tertiaryContainer = Color(0xFF403711),
    onTertiaryContainer = Color(0xFFF6E2A6),
    background = Color(0xFF0B0C0F),
    onBackground = Color(0xFFF7F0E8),
    surface = Color(0xFF111217),
    onSurface = Color(0xFFF7F0E8),
    surfaceVariant = Color(0xFF282329),
    onSurfaceVariant = Color(0xFFD5C8C0),
    outline = Color(0xFF96877F),
    outlineVariant = Color(0xFF4D423D),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD6),
)

private val KitsuLightColors = lightColorScheme(
    primary = Color(0xFF94451F),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFFFDBCA),
    onPrimaryContainer = Color(0xFF351000),
    secondary = Color(0xFF755568),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFFFD8ED),
    onSecondaryContainer = Color(0xFF2B1423),
    tertiary = Color(0xFF6B5C22),
    onTertiary = Color.White,
    tertiaryContainer = Color(0xFFF4E19B),
    onTertiaryContainer = Color(0xFF211B00),
    background = Color(0xFFFFF8F3),
    onBackground = Color(0xFF221A16),
    surface = Color(0xFFFFF8F3),
    onSurface = Color(0xFF221A16),
    surfaceVariant = Color(0xFFF3E1D8),
    onSurfaceVariant = Color(0xFF53443D),
    outline = Color(0xFF85736A),
    outlineVariant = Color(0xFFD7C2B8),
)

private val KitsuTypography = Typography(
    displaySmall = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Bold,
        fontSize = 36.sp,
        lineHeight = 40.sp,
        letterSpacing = (-0.5).sp,
    ),
    headlineLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Bold,
        fontSize = 30.sp,
        lineHeight = 36.sp,
        letterSpacing = (-0.35).sp,
    ),
    headlineSmall = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Bold,
        fontSize = 24.sp,
        lineHeight = 30.sp,
    ),
    titleLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.SemiBold,
        fontSize = 20.sp,
        lineHeight = 26.sp,
    ),
    titleMedium = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.SemiBold,
        fontSize = 16.sp,
        lineHeight = 22.sp,
    ),
    bodyLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Normal,
        fontSize = 16.sp,
        lineHeight = 24.sp,
    ),
    bodyMedium = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.Normal,
        fontSize = 14.sp,
        lineHeight = 20.sp,
    ),
    labelLarge = TextStyle(
        fontFamily = FontFamily.SansSerif,
        fontWeight = FontWeight.SemiBold,
        fontSize = 14.sp,
        lineHeight = 20.sp,
        letterSpacing = 0.1.sp,
    ),
)

private val KitsuShapes = Shapes(
    extraSmall = RoundedCornerShape(10.dp),
    small = RoundedCornerShape(14.dp),
    medium = RoundedCornerShape(20.dp),
    large = RoundedCornerShape(28.dp),
    extraLarge = RoundedCornerShape(34.dp),
)

@Composable
fun KitsuTheme(
    preference: KitsuThemePreference,
    content: @Composable () -> Unit,
) {
    val dark = preference.useDarkColors(isSystemInDarkTheme())
    MaterialTheme(
        colorScheme = if (dark) KitsuDarkColors else KitsuLightColors,
        typography = KitsuTypography,
        shapes = KitsuShapes,
        content = content,
    )
}
