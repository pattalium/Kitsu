import com.android.build.api.dsl.ManagedVirtualDevice
import java.security.MessageDigest
import java.time.Instant
import java.awt.Color
import java.awt.RenderingHints
import java.awt.image.BufferedImage
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.ZipFile
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

val kitsuVersionCode = 33
val kitsuVersionName = "2.2.12"
val kitsuBuildToolsVersion = "36.0.0"
val appIconSource = rootProject.layout.projectDirectory.file("../../../assets/brand/kitsu-app-icon.png")
val generatedAppIconResources = layout.buildDirectory.dir("generated/kitsuAppIcon/res")
val generatedPlayStoreIcon = layout.buildDirectory.file("reports/kitsu/play-store-icon-512.png")
val generatedPlayStoreIconDigest = layout.buildDirectory.file("reports/kitsu/play-store-icon-512.sha256")
val bundletool by configurations.creating
val releaseStorePath = providers.environmentVariable("KITSU_ANDROID_KEYSTORE_PATH")
val releaseKeyAlias = providers.environmentVariable("KITSU_ANDROID_KEY_ALIAS")
val releaseStorePassword = providers.environmentVariable("KITSU_ANDROID_STORE_PASSWORD")
val releaseKeyPassword = providers.environmentVariable("KITSU_ANDROID_KEY_PASSWORD")
val sourceArchiveSha256 = providers.gradleProperty("KITSU_SOURCE_ARCHIVE_SHA256").orNull
val allowUnsignedBundleValidation = providers.gradleProperty("KITSU_ALLOW_UNSIGNED_BUNDLE_VALIDATION")
    .orNull == "true"
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
    outputs.files(
        generatedMaster,
        generatedMonochrome,
        generatedPlayStoreIcon,
        generatedPlayStoreIconDigest,
    )
    doLast {
        val source = appIconSource.asFile
        require(source.isFile) { "missing Kitsu app icon master asset" }
        val actual = MessageDigest.getInstance("SHA-256")
            .digest(source.readBytes())
            .joinToString("") { "%02X".format(it.toInt() and 0xff) }
        require(actual == "4F850B551E8FC242B0B31577AB76407CF1ADE0E1A59BFAAF21EDDE3653B0EF42") {
            "unexpected Kitsu app icon digest"
        }

        // The checked master has a white artboard. Derive both transparent adaptive layers from
        // its ink, so cold launch and the launcher never show a white square.
        val appIcon = requireNotNull(ImageIO.read(source)) { "Kitsu app icon is not a readable PNG" }
        require(appIcon.width == 1254 && appIcon.height == 1254) {
            "unexpected Kitsu app icon dimensions: ${appIcon.width}x${appIcon.height}"
        }
        val foreground = BufferedImage(appIcon.width, appIcon.height, BufferedImage.TYPE_INT_ARGB)
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
                foreground.setRGB(x, y, (alpha shl 24) or 0x00F09A68)
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
        val masterOutput = generatedMaster.get().asFile
        masterOutput.parentFile.mkdirs()
        require(ImageIO.write(foreground, "png", masterOutput)) {
            "failed to encode Kitsu transparent adaptive foreground"
        }
        val monochromeOutput = generatedMonochrome.get().asFile
        monochromeOutput.parentFile.mkdirs()
        require(ImageIO.write(monochrome, "png", monochromeOutput)) {
            "failed to encode Kitsu monochrome launcher layer"
        }

        // Generate the Play listing icon from the same checked geometry and palette. This output
        // is evidence only; it is never read back into the packaged app.
        val playIcon = BufferedImage(512, 512, BufferedImage.TYPE_INT_ARGB)
        val graphics = playIcon.createGraphics()
        try {
            graphics.color = Color(0x0B, 0x0C, 0x0F)
            graphics.fillRect(0, 0, 512, 512)
            graphics.setRenderingHint(
                RenderingHints.KEY_INTERPOLATION,
                RenderingHints.VALUE_INTERPOLATION_BICUBIC,
            )
            graphics.setRenderingHint(
                RenderingHints.KEY_RENDERING,
                RenderingHints.VALUE_RENDER_QUALITY,
            )
            graphics.drawImage(foreground, 0, 0, 512, 512, null)
        } finally {
            graphics.dispose()
        }
        val playOutput = generatedPlayStoreIcon.get().asFile
        playOutput.parentFile.mkdirs()
        require(ImageIO.write(playIcon, "png", playOutput)) { "failed to encode Play store icon" }
        val playDigest = MessageDigest.getInstance("SHA-256")
            .digest(playOutput.readBytes())
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        generatedPlayStoreIconDigest.get().asFile.writeText(
            "$playDigest  play-store-icon-512.png\n",
            Charsets.UTF_8,
        )
    }
}

fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
    .digest(bytes)
    .joinToString("") { "%02x".format(it.toInt() and 0xff) }

fun elfLoadAlignments(bytes: ByteArray): List<Long> {
    require(bytes.size >= 64 && bytes[0] == 0x7f.toByte() && bytes.copyOfRange(1, 4).contentEquals("ELF".toByteArray())) {
        "native library is not ELF"
    }
    require(bytes[5].toInt() == 1) { "only little-endian Android ELF is supported" }
    val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
    val is64Bit = bytes[4].toInt() == 2
    val programOffset = if (is64Bit) buffer.getLong(32) else Integer.toUnsignedLong(buffer.getInt(28))
    val entrySize = buffer.getShort(if (is64Bit) 54 else 42).toInt() and 0xffff
    val entryCount = buffer.getShort(if (is64Bit) 56 else 44).toInt() and 0xffff
    require(programOffset >= 0 && entrySize > 0 && entryCount > 0) { "ELF has no program headers" }
    return buildList {
        repeat(entryCount) { index ->
            val offset = Math.toIntExact(programOffset + index.toLong() * entrySize)
            require(offset >= 0 && offset + entrySize <= bytes.size) { "ELF program header is truncated" }
            if (buffer.getInt(offset) == 1) {
                add(
                    if (is64Bit) buffer.getLong(offset + 48)
                    else Integer.toUnsignedLong(buffer.getInt(offset + 28)),
                )
            }
        }
    }.also { require(it.isNotEmpty()) { "ELF has no PT_LOAD segments" } }
}

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
                appendLine("  \"application_id\": \"ptl.kitsu.app\",")
                appendLine("  \"version_code\": $kitsuVersionCode,")
                appendLine("  \"version_name\": \"$kitsuVersionName\",")
                appendLine("  \"transport\": \"authenticated_ble_only\",")
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
    namespace = "ptl.kitsu.app"
    compileSdk = 36
    buildToolsVersion = kitsuBuildToolsVersion

    defaultConfig {
        applicationId = "ptl.kitsu.app"
        minSdk = 26
        targetSdk = 36
        versionCode = kitsuVersionCode
        versionName = kitsuVersionName

        testInstrumentationRunner = "ptl.kitsu.app.qa.KitsuTestRunner"
        manifestPlaceholders["kitsuSourceArchiveSha256"] = sourceArchiveSha256 ?: "unbound"

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
                create<ManagedVirtualDevice>("kitsuCiApi36") {
                    device = "Pixel 2"
                    apiLevel = 36
                    systemImageSource = "aosp-atd"
                }
            }
        }
    }

    sourceSets.getByName("main").res.srcDir(generatedAppIconResources)
}

tasks.named("preBuild").configure { dependsOn(generateKitsuLauncherAssets) }
tasks.matching { it.name in setOf("assembleRelease", "bundleRelease") }.configureEach {
    dependsOn(generateSourceProvenance)
}

tasks.matching { task ->
    task.name in setOf("assembleRelease", "bundleRelease", "packageRelease")
}.configureEach {
    doFirst {
        val explicitUnsignedBundleValidation =
            name == "bundleRelease" && allowUnsignedBundleValidation && !releaseSigningConfigured
        require(releaseSigningConfigured || explicitUnsignedBundleValidation) {
            "Release signing requires KITSU_ANDROID_KEYSTORE_PATH, KITSU_ANDROID_KEY_ALIAS, " +
                "KITSU_ANDROID_STORE_PASSWORD, and KITSU_ANDROID_KEY_PASSWORD. For an explicitly " +
                "non-publishable AAB structure check only, use " +
                "-PKITSU_ALLOW_UNSIGNED_BUNDLE_VALIDATION=true."
        }
        if (releaseSigningConfigured) {
            require(file(releaseStorePath.get()).isFile) { "Configured Android release keystore is missing" }
        }
        require(sourceArchiveSha256?.matches(Regex("^[0-9a-f]{64}$")) == true) {
            "Release provenance requires -PKITSU_SOURCE_ARCHIVE_SHA256=<64 lowercase hex>"
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
    implementation("net.i2p.crypto:eddsa:0.3.0")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.7.0")
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    androidTestImplementation(composeBom)
    debugImplementation("androidx.compose.ui:ui-test-manifest")
    bundletool("com.android.tools.build:bundletool:1.18.1")
}

val releaseBundle = layout.buildDirectory.file("outputs/bundle/release/app-release.aab")
val releaseBundleValidation = layout.buildDirectory.file("reports/kitsu/release-bundle-validation.json")
val debugApk = layout.buildDirectory.file("outputs/apk/debug/app-debug.apk")
val debugPageSizeValidation = layout.buildDirectory.file("reports/kitsu/debug-16kb-page-validation.json")

tasks.register("verifyDebug16KbPageCompatibility") {
    group = "verification"
    description = "Checks APK ZIP alignment and every packaged ELF PT_LOAD segment for 16 KB devices."
    dependsOn("assembleDebug")
    inputs.file(debugApk)
    outputs.file(debugPageSizeValidation)
    doLast {
        val apk = debugApk.get().asFile
        require(apk.isFile) { "debug APK is missing" }
        val executableSuffix = if (System.getProperty("os.name").startsWith("Windows", true)) ".exe" else ""
        val zipalign = android.sdkDirectory.resolve(
            "build-tools/$kitsuBuildToolsVersion/zipalign$executableSuffix",
        )
        require(zipalign.isFile) { "Build Tools $kitsuBuildToolsVersion zipalign is missing" }
        providers.exec {
            commandLine(zipalign.absolutePath, "-c", "-P", "16", "-v", "4", apk.absolutePath)
        }.result.get().assertNormalExitValue()

        val nativeAlignments = ZipFile(apk).use { zip ->
            zip.entries().asSequence()
                .filter { !it.isDirectory && it.name.startsWith("lib/") && it.name.endsWith(".so") }
                .associate { entry -> entry.name to elfLoadAlignments(zip.getInputStream(entry).use { it.readBytes() }) }
        }
        require(nativeAlignments.isNotEmpty()) { "APK has no native libraries to audit" }
        nativeAlignments.forEach { (path, alignments) ->
            require(alignments.all { it >= 16_384L }) { "$path is not 16 KB ELF compatible: $alignments" }
        }
        val output = debugPageSizeValidation.get().asFile
        output.parentFile.mkdirs()
        output.writeText(
            buildString {
                appendLine("{")
                appendLine("  \"schema\": \"kitsu.android-16kb-page-validation.v1\",")
                appendLine("  \"apk_sha256\": \"${sha256(apk.readBytes())}\",")
                appendLine("  \"zipalign_16kb\": true,")
                appendLine("  \"native_libraries\": {")
                nativeAlignments.entries.sortedBy { it.key }.forEachIndexed { index, (path, alignments) ->
                    append("    \"$path\": [${alignments.joinToString()}]")
                    appendLine(if (index == nativeAlignments.size - 1) "" else ",")
                }
                appendLine("  }")
                appendLine("}")
            },
            Charsets.UTF_8,
        )
    }
}

tasks.register<JavaExec>("verifyReleaseBundle") {
    group = "verification"
    description = "Runs bundletool validation and records the release AAB structure and SHA-256."
    dependsOn("bundleRelease")
    classpath = bundletool
    mainClass.set("com.android.tools.build.bundletool.BundleToolMain")
    args("validate", "--bundle=${releaseBundle.get().asFile.absolutePath}")
    inputs.file(releaseBundle)
    outputs.file(releaseBundleValidation)
    doFirst {
        require(releaseBundle.get().asFile.isFile) { "release AAB is missing" }
    }
    doLast {
        val bundle = releaseBundle.get().asFile
        val entries = ZipFile(bundle).use { zip -> zip.entries().asSequence().map { it.name }.toSet() }
        val requiredEntries = setOf(
            "BundleConfig.pb",
            "base/manifest/AndroidManifest.xml",
            "base/resources.pb",
        )
        require(entries.containsAll(requiredEntries)) { "release AAB is missing required base entries" }
        require(entries.any { it.startsWith("base/dex/classes") && it.endsWith(".dex") }) {
            "release AAB has no base DEX"
        }
        val nativeAlignments = ZipFile(bundle).use { zip ->
            zip.entries().asSequence()
                .filter { !it.isDirectory && it.name.startsWith("base/lib/") && it.name.endsWith(".so") }
                .associate { entry -> entry.name to elfLoadAlignments(zip.getInputStream(entry).use { it.readBytes() }) }
        }
        require(nativeAlignments.isNotEmpty()) { "release AAB has no native libraries to audit" }
        nativeAlignments.forEach { (path, alignments) ->
            require(alignments.all { it >= 16_384L }) { "$path is not 16 KB ELF compatible: $alignments" }
        }
        val bundleSigned = entries.any { entry ->
            entry.startsWith("META-INF/") &&
                (entry.endsWith(".RSA", true) || entry.endsWith(".DSA", true) ||
                    entry.endsWith(".EC", true) || entry.endsWith(".SF", true))
        }
        val output = releaseBundleValidation.get().asFile
        output.parentFile.mkdirs()
        output.writeText(
            buildString {
                appendLine("{")
                appendLine("  \"schema\": \"kitsu.android-bundle-validation.v1\",")
                appendLine("  \"application_id\": \"ptl.kitsu.app\",")
                appendLine("  \"version_code\": $kitsuVersionCode,")
                appendLine("  \"version_name\": \"$kitsuVersionName\",")
                appendLine("  \"bundle_bytes\": ${bundle.length()},")
                appendLine("  \"bundle_sha256\": \"${sha256(bundle.readBytes())}\",")
                appendLine("  \"bundletool_validated\": true,")
                appendLine("  \"native_libraries_16kb_compatible\": true,")
                appendLine("  \"bundle_signed\": $bundleSigned,")
                appendLine("  \"publishable\": $bundleSigned")
                appendLine("}")
            },
            Charsets.UTF_8,
        )
    }
}
