import com.android.build.api.dsl.ManagedVirtualDevice
import java.security.MessageDigest
import java.time.Instant
import java.awt.image.BufferedImage
import javax.imageio.ImageIO

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.kotlin.plugin.serialization")
}

fun configured(name: String, fallback: String): String =
    providers.gradleProperty(name).orNull ?: fallback

fun quoted(value: String): String = "\"${value.replace("\\", "\\\\").replace("\"", "\\\"")}\""

val redirectUri = configured("KITSU_OIDC_REDIRECT_URI", "app.kitsu.mobile:/oauth2redirect")
val redirectScheme = redirectUri.substringBefore(':')
val oidcIssuerFallback = "https://auth.k32.run/realms/kitsu"
val configuredOidcIssuer = configured("KITSU_OIDC_ISSUER", oidcIssuerFallback)
val appIconSource = rootProject.layout.projectDirectory.file("../../../assets/brand/kitsu-app-icon.png")
val generatedAppIconResources = layout.buildDirectory.dir("generated/kitsuAppIcon/res")
val releaseStorePath = providers.environmentVariable("KITSU_ANDROID_KEYSTORE_PATH")
val releaseKeyAlias = providers.environmentVariable("KITSU_ANDROID_KEY_ALIAS")
val releaseStorePassword = providers.environmentVariable("KITSU_ANDROID_STORE_PASSWORD")
val releaseKeyPassword = providers.environmentVariable("KITSU_ANDROID_KEY_PASSWORD")
val sourceArchiveSha256 = providers.gradleProperty("KITSU_SOURCE_ARCHIVE_SHA256").orNull
val releaseSigningConfigured = listOf(
    releaseStorePath, releaseKeyAlias, releaseStorePassword, releaseKeyPassword,
).all { it.orNull?.isNotBlank() == true }
val generateKitsuLauncherAssets = tasks.register("generateKitsuLauncherAssets") {
    val generatedMaster = generatedAppIconResources.map {
        it.file("drawable-nodpi/kitsu_app_icon.png")
    }
    val generatedMonochrome = generatedAppIconResources.map {
        it.file("drawable-nodpi/kitsu_app_icon_monochrome.png")
    }
    inputs.file(appIconSource)
    outputs.files(generatedMaster, generatedMonochrome)
    doLast {
        val source = appIconSource.asFile
        require(source.isFile) { "missing Kitsu app icon master asset" }
        val actual = MessageDigest.getInstance("SHA-256")
            .digest(source.readBytes())
            .joinToString("") { "%02X".format(it.toInt() and 0xff) }
        require(actual == "4F850B551E8FC242B0B31577AB76407CF1ADE0E1A59BFAAF21EDDE3653B0EF42") {
            "unexpected Kitsu app icon digest"
        }

        // Keep the launcher layer byte-for-byte identical to the checked app icon master.
        val masterOutput = generatedMaster.get().asFile
        masterOutput.parentFile.mkdirs()
        source.copyTo(masterOutput, overwrite = true)

        // Android 13 themed icons are alpha masks. Derive that mask from the same checked master
        // instead of maintaining a second, visually divergent fox drawing.
        val appIcon = requireNotNull(ImageIO.read(source)) { "Kitsu app icon is not a readable PNG" }
        require(appIcon.width == 1254 && appIcon.height == 1254) {
            "unexpected Kitsu app icon dimensions: ${appIcon.width}x${appIcon.height}"
        }
        val monochrome = BufferedImage(appIcon.width, appIcon.height, BufferedImage.TYPE_INT_ARGB)
        var inkPixelCount = 0
        var maxSafeZoneRadiusSquared = 0.0
        val adaptiveLayerSizeDp = 108.0
        val adaptiveInsetDp = 19.0
        val adaptiveScale = (adaptiveLayerSizeDp - adaptiveInsetDp * 2.0) / appIcon.width
        for (y in 0 until appIcon.height) {
            for (x in 0 until appIcon.width) {
                val sourcePixel = appIcon.getRGB(x, y)
                val red = sourcePixel ushr 16 and 0xff
                val green = sourcePixel ushr 8 and 0xff
                val blue = sourcePixel and 0xff
                val luma = (red * 2126 + green * 7152 + blue * 722 + 5000) / 10000
                val alpha = if (luma >= 240) 0 else ((240 - luma) * 255 / 240).coerceIn(0, 255)
                monochrome.setRGB(x, y, alpha shl 24)
                if (alpha > 0) {
                    inkPixelCount += 1
                    val mappedX = adaptiveInsetDp + x * adaptiveScale
                    val mappedY = adaptiveInsetDp + y * adaptiveScale
                    val deltaX = mappedX - adaptiveLayerSizeDp / 2.0
                    val deltaY = mappedY - adaptiveLayerSizeDp / 2.0
                    maxSafeZoneRadiusSquared = maxOf(
                        maxSafeZoneRadiusSquared,
                        deltaX * deltaX + deltaY * deltaY,
                    )
                }
            }
        }
        require(inkPixelCount > 200_000) { "Kitsu monochrome launcher layer lost its artwork" }
        require(maxSafeZoneRadiusSquared <= 33.0 * 33.0) {
            "Kitsu app icon ink exceeds Android's guaranteed 66dp adaptive-icon safe circle"
        }
        val monochromeOutput = generatedMonochrome.get().asFile
        monochromeOutput.parentFile.mkdirs()
        require(ImageIO.write(monochrome, "png", monochromeOutput)) {
            "failed to encode Kitsu monochrome launcher layer"
        }
    }
}

fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
    .digest(bytes)
    .joinToString("") { "%02x".format(it.toInt() and 0xff) }

val sourceProvenanceOutput = layout.buildDirectory.file("reports/kitsu/source-provenance.json")
val generateSourceProvenance = tasks.register("generateSourceProvenance") {
    group = "verification"
    description = "Writes a file-by-file Android source manifest and deterministic tree digest."
    inputs.files(
        rootProject.fileTree(rootProject.layout.projectDirectory) {
            exclude("**/build/**", "**/.gradle/**", "**/.idea/**", "local.properties")
        },
    )
    inputs.file(appIconSource)
    inputs.property("sourceArchiveSha256", sourceArchiveSha256 ?: "unbound")
    outputs.file(sourceProvenanceOutput)
    doLast {
        val projectRoot = rootProject.layout.projectDirectory.asFile.canonicalFile
        val excludedSegments = setOf("build", ".gradle", ".idea")
        val projectFiles = projectRoot.walkTopDown()
            .filter(File::isFile)
            .filter { file ->
                val relative = file.relativeTo(projectRoot).invariantSeparatorsPath
                relative != "local.properties" &&
                    relative.split('/').none(excludedSegments::contains) &&
                    file.extension.lowercase() !in setOf("jks", "keystore")
            }
            .map { file -> file.relativeTo(projectRoot).invariantSeparatorsPath to file }
            .toList()
        val allFiles = (projectFiles + listOf(
            "@project/assets/brand/kitsu-app-icon.png" to appIconSource.asFile,
        )).sortedBy { it.first }
        val entries = allFiles.map { (path, file) -> Triple(path, file.length(), sha256(file.readBytes())) }
        val treePayload = entries.joinToString(separator = "\n", postfix = "\n") {
            (path, size, digest) -> "$digest $size $path"
        }
        fun escaped(value: String): String = buildString {
            value.forEach { character ->
                when (character) {
                    '\\' -> append("\\\\")
                    '"' -> append("\\\"")
                    else -> append(character)
                }
            }
        }
        val output = sourceProvenanceOutput.get().asFile
        output.parentFile.mkdirs()
        output.writeText(
            buildString {
                appendLine("{")
                appendLine("  \"schema\": \"kitsu.android-source-provenance.v1\",")
                appendLine("  \"generated_at_utc\": \"${Instant.now()}\",")
                appendLine("  \"application_id\": \"app.kitsu.mobile\",")
                appendLine("  \"version_code\": 6,")
                appendLine("  \"version_name\": \"1.1.0\",")
                appendLine("  \"backend_url\": \"${escaped(configured("KITSU_BACKEND_URL", "https://api.k32.run"))}\",")
                appendLine("  \"oidc_issuer\": \"${escaped(configuredOidcIssuer)}\",")
                appendLine(
                    "  \"source_archive_sha256\": " +
                        (sourceArchiveSha256?.let { "\"${escaped(it)}\"" } ?: "null") + ",",
                )
                appendLine("  \"vcs_revision\": null,")
                appendLine("  \"tree_algorithm\": \"sha256(lines: sha256-lowerhex SP size-decimal SP path LF)\",")
                appendLine("  \"tree_sha256\": \"${sha256(treePayload.toByteArray(Charsets.UTF_8))}\",")
                appendLine("  \"files\": [")
                entries.forEachIndexed { index, (path, size, digest) ->
                    append("    {\"path\":\"${escaped(path)}\",\"size\":$size,\"sha256\":\"$digest\"}")
                    appendLine(if (index == entries.lastIndex) "" else ",")
                }
                appendLine("  ]")
                appendLine("}")
            },
            Charsets.UTF_8,
        )
    }
}

android {
    namespace = "app.kitsu.mobile"
    compileSdk = 35

    defaultConfig {
        applicationId = "app.kitsu.mobile"
        minSdk = 26
        targetSdk = 35
        versionCode = 6
        versionName = "1.1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        manifestPlaceholders["appAuthRedirectScheme"] = redirectScheme
        manifestPlaceholders["kitsuSourceArchiveSha256"] = sourceArchiveSha256 ?: "unbound"

        buildConfigField("String", "KITSU_BACKEND_URL", quoted(configured("KITSU_BACKEND_URL", "https://api.k32.run")))
        buildConfigField("String", "KITSU_OIDC_ISSUER", quoted(configuredOidcIssuer))
        buildConfigField("String", "KITSU_OIDC_CLIENT_ID", quoted(configured("KITSU_OIDC_CLIENT_ID", "kitsu-native")))
        buildConfigField("String", "KITSU_OIDC_REDIRECT_URI", quoted(redirectUri))
        buildConfigField(
            "String",
            "KITSU_SOURCE_ARCHIVE_SHA256",
            quoted(sourceArchiveSha256 ?: "unbound"),
        )
        buildConfigField("String", "KITSU_BLE_SERVICE_UUID", quoted(configured("KITSU_BLE_SERVICE_UUID", "7f820001-735b-4b57-9a48-5f5f4b495453")))
        buildConfigField("String", "KITSU_BLE_WRITE_UUID", quoted(configured("KITSU_BLE_WRITE_UUID", "7f820002-735b-4b57-9a48-5f5f4b495453")))
        buildConfigField("String", "KITSU_BLE_NOTIFY_UUID", quoted(configured("KITSU_BLE_NOTIFY_UUID", "7f820003-735b-4b57-9a48-5f5f4b495453")))
    }

    signingConfigs {
        create("release") {
            if (releaseSigningConfigured) {
                storeFile = file(releaseStorePath.get())
                storePassword = releaseStorePassword.get()
                keyAlias = releaseKeyAlias.get()
                keyPassword = releaseKeyPassword.get()
                enableV1Signing = true
                enableV2Signing = true
                enableV3Signing = true
                enableV4Signing = true
            }
        }
    }

    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
        release {
            isMinifyEnabled = true
            if (releaseSigningConfigured) signingConfig = signingConfigs.getByName("release")
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }

    testOptions {
        animationsDisabled = true
        managedDevices {
            devices {
                create<ManagedVirtualDevice>("kitsuCiApi35") {
                    device = "Pixel 2"
                    apiLevel = 35
                    systemImageSource = "aosp-atd"
                }
            }
        }
    }

    sourceSets.getByName("main").res.srcDir(generatedAppIconResources)
}

tasks.named("preBuild").configure { dependsOn(generateKitsuLauncherAssets) }
tasks.matching { it.name == "assembleRelease" }.configureEach {
    dependsOn(generateSourceProvenance)
}

tasks.matching { task ->
    task.name in setOf("assembleRelease", "bundleRelease", "packageRelease")
}.configureEach {
    doFirst {
        require(releaseSigningConfigured) {
            "Release signing requires KITSU_ANDROID_KEYSTORE_PATH, KITSU_ANDROID_KEY_ALIAS, " +
                "KITSU_ANDROID_STORE_PASSWORD, and KITSU_ANDROID_KEY_PASSWORD"
        }
        require(file(releaseStorePath.get()).isFile) { "Configured Android release keystore is missing" }
        require(sourceArchiveSha256?.matches(Regex("^[0-9a-f]{64}$")) == true) {
            "Release provenance requires -PKITSU_SOURCE_ARCHIVE_SHA256=<64 lowercase hex>"
        }
        require(configuredOidcIssuer == oidcIssuerFallback) {
            "Official release requires OIDC issuer $oidcIssuerFallback"
        }
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")

    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-compose:1.10.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    debugImplementation("androidx.compose.ui:ui-tooling")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("net.openid:appauth:0.11.1")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.7.0")
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    androidTestImplementation(composeBom)
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
