# JNI: ネイティブ側 (Java_org_usbmem_app_NativeBridge_*) が名前で解決するので、クラス名とメソッド名を変えない・消さない
-keep class org.usbmem.app.NativeBridge { *; }
# ネイティブから値を受け取る/渡すクラスがあれば、ここに追加する (現状、ネイティブから Java のクラス・メソッドを名前で探す処理はない)

# スタックトレースの行番号は残す (難読化したクラス名は mapping.txt で戻せる)
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile

# kfsd (docs/kfsd.md): ネイティブ (jni_kfsd.cpp) が KfsdNative.spawnChildFd を名前で探し、JNI が KfsdNative / KfsdSpawnNative のメソッドを名前で解決する
-keep class org.usbmem.app.kfsd.KfsdNative { *; }
-keep class org.usbmem.app.kfsd.KfsdSpawnNative { *; }

# SMB (docs/smb.md): SMBJ と依存ライブラリ。
#  - mbassador (SMBJ のイベントバス) は、@Handler の付いたメソッドをリフレクションで呼ぶ。呼び出し元が無いように見えるので R8 に消されないよう残す。
#  - Android に無い / 使わないクラス (JGSS = Kerberos、javax.el、slf4j のバインディング、BouncyCastle の任意のプロバイダ部分) への参照は警告を出さない
#    (認証は NTLM だけにしてあり、SPNEGO / Kerberos のクラスは読み込まれない。slf4j は実装が無いので何も出力しない)。
-keepattributes *Annotation*,Signature,InnerClasses,EnclosingMethod
-keep class net.engio.mbassador.** { *; }
-keepclassmembers class * { @net.engio.mbassador.listener.Handler <methods>; }
-keep @net.engio.mbassador.listener.Listener class * { *; }
-dontwarn javax.el.**
-dontwarn org.ietf.jgss.**
-dontwarn com.sun.security.jgss.**
-dontwarn javax.security.auth.kerberos.**
-dontwarn org.slf4j.impl.**
-dontwarn org.bouncycastle.**
-dontwarn sun.security.**
-dontwarn java.lang.management.**
