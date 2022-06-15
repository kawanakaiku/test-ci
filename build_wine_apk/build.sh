#!/bin/sh
#
# Build Android packages
#
# Copyright 2018 Alexandre Julliard
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
#

set -e

basedir=$HOME/wine/packages/android
winetree=https://github.com/wine-mirror/wine.git
android_root=$HOME/android
android_api=26
default_path=/usr/lib/ccache:$HOME/bin:/usr/sbin:/sbin:/usr/bin:/bin:$android_root/tools:$android_root/platform-tools
downloads=$HOME/.cache/wine-packaging
winetools=$basedir/tools
silent=--silent
makeflags="--no-print-directory $silent -j$(nproc)"

android_ndk=android-ndk-r21d
llvm_mingw=llvm-mingw-20201020-ucrt-ubuntu-18.04
gradle=gradle-3.5.1
freetype=freetype-2.10.4
gmp=gmp-6.2.1
nettle=nettle-3.6
gnutls=gnutls-3.7.0
openldap=openldap-2.4.56
gecko=wine-gecko-2.47.2

sourcedir=$(cd $(dirname $0) && pwd)
release=$1

download ()
{
    test -d $downloads || mkdir -p $downloads
    test -f $downloads/$(basename $2) || wget -O $downloads/$(basename $2) ${3:-$2}
    rm -rf $1
    case $2 in
        *.zip)
            unzip -q $downloads/$(basename $2) ;;
        *)
            tar xf $downloads/$(basename $2) ;;
    esac
}

config_ndk ()
{
    download $android_ndk https://dl.google.com/android/repository/$android_ndk-linux-x86_64.zip
}

config_toolchain ()
{
    rm -rf toolchain
    ../$android_ndk/build/tools/make_standalone_toolchain.py --arch $arch --api $android_api --install-dir toolchain
}

config_gradle ()
{
    download $gradle https://services.gradle.org/distributions/$gradle-bin.zip
}

config_llvm_mingw ()
{
    download $llvm_mingw https://github.com/mstorsjo/llvm-mingw/releases/download/20201020/$llvm_mingw.tar.xz
}

config_freetype ()
{
    download $freetype http://download.savannah.gnu.org/releases/freetype/$freetype.tar.xz
    (cd $freetype && $run_configure --without-png && make $makeflags)
}

config_gmp ()
{
    download $gmp https://gmplib.org/download/gmp/$gmp.tar.xz
    (cd $gmp && $run_configure --disable-static && make $makeflags)
}

config_nettle ()
{
    test -d $gmp || config_gmp
    download $nettle https://ftp.gnu.org/gnu/nettle/$nettle.tar.gz
    gmpdir=$(pwd)/$gmp
    (cd $nettle && $run_configure --disable-shared --disable-documentation CPPFLAGS=-I$gmpdir LDFLAGS=-L$gmpdir/.libs && make $makeflags)
}

config_gnutls ()
{
    test -d $nettle || config_nettle
    version=v$(expr $gnutls : '.*-\([0-9]\+\.[0-9]\+\)')
    download $gnutls https://www.gnupg.org/ftp/gcrypt/gnutls/$version/$gnutls.tar.xz
    gmpdir=$(pwd)/$gmp
    nettledir=$(pwd)/$nettle
    test -f $nettle/nettle || ln -s . $nettle/nettle  # gnutls includes nettle files with nettle/ prefix
    (cd $gnutls && ./configure --host=$host --without-p11-kit --without-idn --with-included-libtasn1 --with-included-unistring -disable-cxx \
                               --disable-maintainer-mode --disable-static --disable-doc --disable-tools --disable-tests \
                               CC=$cc PKG_CONFIG=true \
                               GMP_CFLAGS="-I$gmpdir -L$gmpdir/.libs -lgmp" GMP_LIBS=$gmpdir/.libs/libgmp.so \
	                       NETTLE_CFLAGS=-I$nettledir NETTLE_LIBS="-L$nettledir -lnettle" \
	                       HOGWEED_CFLAGS=-I$nettledir HOGWEED_LIBS="-L$nettledir -lhogweed -lnettle $gmpdir/.libs/libgmp.so" \
         && make $makeflags)
}

config_openldap ()
{
    download $openldap ftp://ftp.openldap.org/pub/OpenLDAP/openldap-release/$openldap.tgz
    cp $gnutls/build-aux/ltmain.sh $gnutls/build-aux/config.guess $gnutls/build-aux/config.sub $openldap/build
    (cd $openldap && aclocal && autoconf && $run_configure --with-yielding_select=yes --disable-bdb --disable-hdb ac_cv_func_memcmp_working=yes CPPFLAGS=-DANDROID CC=$host-gcc && make $makeflags)
}


config_wine_tools ()
{
    mkdir tools
    (cd tools && ../wine/configure --without-x --enable-win64 $silent && make $makeflags __tooldeps__)
}

config_wine ()
{
    test -d toolchain || config_toolchain
    test -d $freetype || config_freetype
    test -d $gmp || config_gmp
    test -d $nettle || config_nettle
    test -d $gnutls || config_gnutls
    test -d $openldap || config_openldap
    rm -rf wine
    mkdir wine
    (cd wine && ../../wine/configure $silent --host=$host --with-wine-tools=../../tools CC=$cc \
                FREETYPE_CFLAGS="-I../$freetype/include" \
                FREETYPE_LIBS="-L../$freetype/objs/.libs -lfreetype" \
                GNUTLS_CFLAGS="-I../$gnutls/lib/includes" \
                GNUTLS_LIBS="-L../$gnutls/lib/.libs -lgnutls -L../$gmp/.libs -lgmp" \
                LDAP_CFLAGS="-I../$openldap/include" \
                LDAP_LIBS="-L../$openldap/libraries/liblber/.libs -L../$openldap/libraries/libldap_r/.libs -lldap_r-2.4 -llber-2.4" \
    )
}

install_all ()
{
    test -d wine || config_wine
    cd wine
    destdir=$(pwd)/dlls/wineandroid.drv
    srcdir=../../wine
    install=$srcdir/tools/install-sh
    rm -rf $destdir/assets assets-tmp $destdir/lib
    make $makeflags all install-lib DESTDIR=$(pwd) prefix=/assets-tmp

    for i in \
        ../$freetype/objs/.libs/libfreetype.so \
        ../$gmp/.libs/libgmp.so \
        ../$gnutls/lib/.libs/libgnutls.so \
        ../$openldap/libraries/liblber/.libs/liblber-2.4.so \
        ../$openldap/libraries/libldap_r/.libs/libldap_r-2.4.so
    do
        $install -s $i $destdir/lib/$exec_prefix/$(basename $i)
    done

    $install -m 644 $sourcedir/LICENSE.txt assets-tmp/LICENSE.txt
    mv assets-tmp $destdir/assets

    (cd $destdir && gradle -q -Psrcdir=$srcdir -Dorg.gradle.jvmargs="-Xmx2048m -XX:MaxPermSize=512m" assembleDebug)
    mv $destdir/build/outputs/apk/wine-debug.apk ../../wine-${release:-debug}-$arch.apk
}

test -z "$release" || rm -rf $basedir
test -d $basedir || mkdir $basedir
cd $basedir

test -d $android_ndk || config_ndk
test -d $gradle || config_gradle
test -d $llvm_mingw || config_llvm_mingw

test -d wine || git clone $winetree
test -z "$release" || (cd wine && git checkout wine-$release)
test -d $winetools || config_wine_tools

for arch in x86
do
    test -d $arch || mkdir $arch

    case $arch in
        x86)
            host=i686-linux-android
            exec_prefix=x86
            ;;
        arm)
            host=arm-linux-androideabi
            exec_prefix=armeabi-v7a
            ;;
    esac

    (cd $arch
    PATH=$default_path:$basedir/$llvm_mingw/bin:$basedir/$gradle/bin:$basedir/$arch/toolchain/bin
    run_configure="./configure $silent --host=$host PKG_CONFIG=false"
    install_all)
done
