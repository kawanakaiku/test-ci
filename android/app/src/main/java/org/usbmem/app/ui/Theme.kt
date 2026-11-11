package org.usbmem.app.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.unit.dp

/** 状態の色 (バナー・チップ用)。Material の colorScheme に無い「警告」「成功」を足す。 */
@Immutable
data class StatusColors(
    val warningContainer: Color, val onWarningContainer: Color,
    val successContainer: Color, val onSuccessContainer: Color,
)

private val LightStatus = StatusColors(Color(0xFFFFEFC7), Color(0xFF4A3700), Color(0xFFD7F2DC), Color(0xFF0B3D1B))
private val DarkStatus = StatusColors(Color(0xFF4A3B00), Color(0xFFFFE08A), Color(0xFF14432A), Color(0xFFB7EBC4))
val LocalStatusColors = staticCompositionLocalOf { LightStatus }

private val LightColors = lightColorScheme(
    primary = Color(0xFF1B5FA8), onPrimary = Color.White, primaryContainer = Color(0xFFD5E3FF), onPrimaryContainer = Color(0xFF001B3C),
    secondaryContainer = Color(0xFFE1E8F5), onSecondaryContainer = Color(0xFF131C2B),
    surface = Color(0xFFF8F9FF), background = Color(0xFFF8F9FF),
)
private val DarkColors = darkColorScheme(
    primary = Color(0xFFA7C8FF), onPrimary = Color(0xFF00315F), primaryContainer = Color(0xFF00468A), onPrimaryContainer = Color(0xFFD5E3FF),
    secondaryContainer = Color(0xFF3F4757), onSecondaryContainer = Color(0xFFDBE3F3),
    surface = Color(0xFF111318), background = Color(0xFF111318),
)

/** アプリ全体のテーマ。Android 12 以降は壁紙に合わせた動的カラー、それ以前は固定配色。ダークテーマに追従。 */
@Composable
fun AppTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val ctx = LocalContext.current
    val scheme = when {
        Build.VERSION.SDK_INT >= 31 -> if (dark) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)
        dark -> DarkColors
        else -> LightColors
    }
    androidx.compose.runtime.CompositionLocalProvider(LocalStatusColors provides if (dark) DarkStatus else LightStatus) {
        MaterialTheme(
            colorScheme = scheme,
            shapes = Shapes(small = RoundedCornerShape(8.dp), medium = RoundedCornerShape(14.dp), large = RoundedCornerShape(20.dp)),
            content = content,
        )
    }
}
