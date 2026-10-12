#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
cd "$(dirname "$0")/.."
. ./toolchain.env
python3 scripts/fetch-network.py
bundled_source=false
if [ -f third_party/FBInk/.kobo-spark-commit ]; then
    test "$(cat third_party/FBInk/.kobo-spark-commit)" = "$FBINK_COMMIT"
    bundled_source=true
elif [ ! -d third_party/FBInk/.git ]; then
    mkdir -p third_party
    git clone https://github.com/NiLuJe/FBInk.git third_party/FBInk
fi
if [ "$bundled_source" = false ] && [ "$(git -C third_party/FBInk rev-parse HEAD)" != "$FBINK_COMMIT" ]; then
    git -C third_party/FBInk fetch origin "$FBINK_COMMIT"
    git -C third_party/FBInk checkout --detach "$FBINK_COMMIT"
fi
if [ "$bundled_source" = false ]; then git -C third_party/FBInk submodule update --init --recursive; fi
mkdir -p build dist/kobo-spark
docker run --rm --platform=linux/amd64 --network=none \
    --user "$(id -u):$(id -g)" -v "$PWD:/work" -w /work \
    --entrypoint sh "$KOBO_TOOLCHAIN_IMAGE" -eu -c '
    . ./toolchain.env
    prefix=arm-nickel-linux-gnueabihf
    flags="-O2 -std=gnu11 -march=armv7-a -mfpu=neon -mfloat-abi=hard -mthumb"
    sh scripts/build-network.sh
    if [ ! -f build/fbink-opentype-image.ready ]; then make -C third_party/FBInk clean; fi
    # NickelTC uses GCC 4.9; override FBInk flags requiring a newer compiler.
    make -C third_party/FBInk -j4 CROSS_TC="$prefix" \
        FBINK_VERSION="$(printf %.7s "$FBINK_COMMIT")" \
        EXTRA_CFLAGS="-std=gnu11 -fno-common -Wno-pragmas" \
        CFLAGS="$flags" MINIMAL=1 OPENTYPE=1 IMAGE=1 LIBS=-lm staticlib
    touch build/fbink-opentype-image.ready
    "$prefix-gcc" $flags -Wall -Wextra -Werror -Isrc -Ithird_party/FBInk -Ibuild/network/prefix/include \
        src/dashboard.c src/dashboard_data.c third_party/FBInk/Release/libfbink.a \
        build/network/prefix/lib/libcurl.a build/network/prefix/lib/libmbedtls.a \
        build/network/prefix/lib/libmbedx509.a build/network/prefix/lib/libmbedcrypto.a -lpthread -lm -lrt \
        -o build/kobo-spark
    "$prefix-strip" -o dist/kobo-spark/kobo-spark build/kobo-spark
    "$prefix-readelf" -h -A -d --version-info dist/kobo-spark/kobo-spark > build/elf-report.txt
    '
cp device/launch.sh device/restore-reader.sh device/cacert.pem device/nickelmenu.conf dist/kobo-spark/
if [ -f device/dashboard.conf ]; then
    cp device/dashboard.conf dist/kobo-spark/dashboard.conf
else
    cp device/dashboard.conf.example dist/kobo-spark/dashboard.conf
fi
mkdir -p dist/kobo-spark/fonts dist/kobo-spark/licenses dist/kobo-spark/source
cp device/fonts/*.ttf device/fonts/OFL.txt device/fonts/SOURCE.txt dist/kobo-spark/fonts/
cp LICENSE COPYRIGHT dist/kobo-spark/
cp THIRD_PARTY_NOTICES.md dist/kobo-spark/
cp "third_party/curl-$CURL_VERSION/COPYING" dist/kobo-spark/licenses/curl.txt
cp "third_party/mbedtls-$MBEDTLS_VERSION/LICENSE" dist/kobo-spark/licenses/mbedtls.txt
cp third_party/FBInk/LICENSE dist/kobo-spark/licenses/fbink.txt
cp third_party/FBInk/CREDITS dist/kobo-spark/licenses/fbink-credits.txt
cp third_party/FBInk/libunibreak/LICENCE dist/kobo-spark/licenses/libunibreak.txt
cp third_party/FBInk/stb/LICENSE dist/kobo-spark/licenses/stb.txt
cp third_party/FBInk/qimagescale/QIMAGETRANSFORM_LICENSE.txt dist/kobo-spark/licenses/qimagescale.txt
mkdir -p dist/kobo-spark/source/qimagescale
cp third_party/FBInk/qimagescale/*.c third_party/FBInk/qimagescale/*.h dist/kobo-spark/source/qimagescale/
cp third_party/FBInk/i2c-tools/COPYING.LGPL dist/kobo-spark/licenses/libi2c.txt
cp LICENSES/MPL-2.0.txt dist/kobo-spark/licenses/mozilla-ca-MPL-2.0.txt
cp src/dashboard.c src/dashboard_data.c src/dashboard_data.h toolchain.env dist/kobo-spark/source/
chmod +x dist/kobo-spark/kobo-spark dist/kobo-spark/launch.sh dist/kobo-spark/restore-reader.sh
printf '%s\n' 'Built dist/kobo-spark/kobo-spark (ARMv7 hard-float)'
