# JNI: ネイティブ側 (Java_org_usbmem_app_NativeBridge_*) が名前で解決するので、クラス名とメソッド名を変えない・消さない
-keep class org.usbmem.app.NativeBridge { *; }
# ネイティブから値を受け取る/渡すクラスがあれば、ここに追加する (現状、ネイティブから Java のクラス・メソッドを名前で探す処理はない)

# スタックトレースの行番号は残す (難読化したクラス名は mapping.txt で戻せる)
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
