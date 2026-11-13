plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
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
        versionName = "0.1.0-phase1"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        // 64bit ABI のみ (Google Play 要件)。x86_64 はエミュレータ用。
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        // 書き込み経路を有効にしたビルドか (下の cmake の USBMEM_NTFS_WRITE と同じ条件)。false なら、画面で書き込みモードを選べない (BuildConfig.WRITE_SUPPORTED)
        buildConfigField("boolean", "WRITE_SUPPORTED", (providers.gradleProperty("usbmemNtfsWrite").orNull == "true").toString())
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
                // NTFS 書き込みは実験的: 既定では無効 (-PusbmemNtfsWrite=true で書き込み可能なビルドにする)。無効ビルドでは書き込みマウントが -EROFS で失敗する
                if (providers.gradleProperty("usbmemNtfsWrite").orNull == "true") arguments += "-DUSBMEM_NTFS_WRITE=ON"
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
    packaging { jniLibs { useLegacyPackaging = false } }  // 16KB ページ整列のため非圧縮で格納
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
}
