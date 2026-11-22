pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "usbmem"
include(":app")
// kfsd のプラグイン APK (GPL-2.0。別 APK で配る。docs/kfsd.md 6 章)。公開版のツリーには無い
if (file("kfsd-plugin").isDirectory) include(":kfsd-plugin")
