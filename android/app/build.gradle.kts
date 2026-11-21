plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// エディション (docs/editions.md): personal (既定。すべての FS を含む自分用。配布しない) / public (GPL 系を含まない公開版。-PusbmemEdition=public)
val usbmemEdition = providers.gradleProperty("usbmemEdition").orNull ?: "personal"
require(usbmemEdition == "personal" || usbmemEdition == "public") { "usbmemEdition must be personal or public (got '$usbmemEdition')" }

// 個人版の FUSE 層 (別プロセスの wimlib。docs/fuse-layer.md 10 章): -PusbmemWimFuse=<tools/wimfuse/build_android.sh の出力ディレクトリ> のときだけ有効。
// libwimfuse.so (PIE の実行ファイル) を jniLibs として APK に入れ、nativeLibraryDir から exec する。exec には extractNativeLibs=true (useLegacyPackaging) が要る。
// 公開版 (-PusbmemEdition=public) では指定しても無視する (wimlib は LGPL / GPL。公開版のツリーに FUSE 層は無い。docs/editions.md 12 章)。
val wimFuseDir: String? = providers.gradleProperty("usbmemWimFuse").orNull.let { d ->
    if (d != null && usbmemEdition == "public") { logger.warn("usbmemWimFuse is ignored in the public edition"); null } else d
}

// 個人版の kfsd (GPL 系の FS ドライバを別プロセスの実行ファイルにしたもの。docs/kfsd.md): -PusbmemKfsd=true のとき libkfsd.so (PIE の実行ファイル) を作って APK に入れ、
// nativeLibraryDir から :kfsd サービスが exec する。exec には extractNativeLibs=true (useLegacyPackaging) が要る。公開版では指定しても無視する (kfsd は別 APK のプラグインで配る)。
val kfsdLocal: Boolean = (providers.gradleProperty("usbmemKfsd").orNull == "true").let { on ->
    if (on && usbmemEdition == "public") { logger.warn("usbmemKfsd is ignored in the public edition"); false } else on
}

android {
    namespace = "org.usbmem.app"
    compileSdk = 36
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "org.usbmem.app"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = if (usbmemEdition == "public") "0.1.0-phase1-public" else "0.1.0-phase1"
        // エディション名 (BuildConfig.EDITION)。UI が、公開版で未対応のファイルシステムを案内するのに使う (Edition.kt)
        buildConfigField("String", "EDITION", "\"$usbmemEdition\"")
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        // 64bit ABI のみ (Google Play 要件)。x86_64 はエミュレータ用。
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        // 書き込み経路を有効にしたビルドか (下の cmake の USBMEM_NTFS_WRITE と同じ条件)。false なら、画面で書き込みモードを選べない (BuildConfig.WRITE_SUPPORTED)
        buildConfigField("boolean", "WRITE_SUPPORTED", (providers.gradleProperty("usbmemNtfsWrite").orNull == "true").toString())
        buildConfigField("boolean", "WIMFUSE_SUPPORTED", (wimFuseDir != null).toString())
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
                if (usbmemEdition == "public") arguments += "-DUSBMEM_EDITION=public"
                // NTFS 書き込みは実験的: 既定では無効 (-PusbmemNtfsWrite=true で書き込み可能なビルドにする)。無効ビルドでは書き込みマウントが -EROFS で失敗する
                if (providers.gradleProperty("usbmemNtfsWrite").orNull == "true") arguments += "-DUSBMEM_NTFS_WRITE=ON"
                if (wimFuseDir != null) arguments += "-DUSBMEM_WIMFUSE=ON"
                if (kfsdLocal) arguments += "-DUSBMEM_KFSD_EXE=ON"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    // リリースビルドは R8 でコード縮小・難読化 + 未使用リソースの削除 (APK を小さくする)。
    // 署名鍵はリポジトリに置かない。~/.gradle/gradle.properties の usbmemKeystore / usbmemKeystorePassword / usbmemKeyAlias で指定する
    // (鍵が無い環境では署名なしの release APK になる)。手元の動作確認だけなら -PusbmemSignWithDebugKey=true で debug 鍵で署名できる (配布用ではない)。
    signingConfigs {
        val ks = providers.gradleProperty("usbmemKeystore").orNull
        if (providers.gradleProperty("usbmemSignWithDebugKey").orNull == "true") {
            create("localRelease") {
                storeFile = file(System.getProperty("user.home") + "/.android/debug.keystore")
                storePassword = "android"; keyAlias = "androiddebugkey"; keyPassword = "android"
            }
        } else if (ks != null) {
            create("localRelease") {
                storeFile = file(ks)
                storePassword = providers.gradleProperty("usbmemKeystorePassword").get()
                keyAlias = providers.gradleProperty("usbmemKeyAlias").get()
                keyPassword = providers.gradleProperty("usbmemKeystorePassword").get()
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfigs.findByName("localRelease")?.let { signingConfig = it }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { compose = true; buildConfig = true }
    // 16KB ページ整列のため非圧縮で格納 (extractNativeLibs=false)。ただし個人版の FUSE 層 (wimfuse) を入れるときは、libwimfuse.so を nativeLibraryDir から exec するので展開が要る
    packaging { jniLibs { useLegacyPackaging = wimFuseDir != null || kfsdLocal } }
    if (wimFuseDir != null) sourceSets.getByName("main").jniLibs.srcDir("$wimFuseDir/jniLibs")
    // kfsd を起こす共通部 (Kotlin: KfsdSpawner / KfsdProtocol / KfsdSpawnNative、JNI: kfsd_spawn.cpp)。別 APK のプラグイン (:kfsd-plugin) と同じソースを使う
    sourceSets.getByName("main").java.srcDir("../kfsd-common/src/main/java")
    testOptions { unitTests.isReturnDefaultValues = true }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.09.00")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
    // 暗号ボリュームの UI 試験 (CryptoUiTest): 実画面 (Compose) を操作する。ファイル選択は espresso-intents で差し替える
    androidTestImplementation(composeBom)
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    androidTestImplementation("androidx.test.espresso:espresso-intents:3.6.1")
}
