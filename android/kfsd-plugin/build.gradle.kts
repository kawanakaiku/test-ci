// kfsd のプラグイン APK (docs/kfsd.md 6 章 (b))。GPL-2.0 の FS ドライバを載せた kfsd の実行ファイル (libkfsd.so) を入れ、bound service (PluginService) で、
// 本体 (org.usbmem.app) から渡された socketpair の端を子の fd 3 にして起こす。本体 (Apache-2.0) とは Binder (起動の依頼) と socketpair (RPC) だけでつながる。
// 本体と同じ署名のアプリだけがつなげる (PluginService が呼び出し元の署名を確かめる)。これは試作: 配布用の署名・説明文・ソースの入手先の表示は docs/kfsd.md 6 章。
// -PusbmemPluginAbis=x86_64 のように、ABI を絞れる (CI の AVD は x86_64 だけ。ビルド時間を減らす)。
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val abis: List<String> = (providers.gradleProperty("usbmemPluginAbis").orNull ?: "arm64-v8a,x86_64").split(",").map { it.trim() }.filter { it.isNotEmpty() }

android {
    namespace = "org.usbmem.kfsd"
    compileSdk = 36
    defaultConfig {
        applicationId = "org.usbmem.kfsd"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-prototype"
        ndk { abiFilters += abis }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static", "-DUSBMEM_NTFS_WRITE=ON")
            }
        }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    // libkfsd.so (PIE の実行ファイル) を nativeLibraryDir から exec するので、展開が要る (extractNativeLibs=true)
    packaging { jniLibs { useLegacyPackaging = true } }
    // 本体と同じ起こす側の共通部 (KfsdSpawner / KfsdProtocol / KfsdSpawnNative)
    sourceSets.getByName("main").java.srcDir("../kfsd-common/src/main/java")
}
