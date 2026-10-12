#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Invoked inside the pinned NickelTC container, with /work mounted.
set -eu
cd /work
. ./toolchain.env
stamp="$MBEDTLS_VERSION $CURL_VERSION $KOBO_TOOLCHAIN_IMAGE"
if [ -f build/network/prefix/lib/libcurl.a ] && [ "$(cat build/network/ready 2>/dev/null || true)" = "$stamp" ]; then
    exit 0
fi
mkdir -p build/network
flags='-O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -mthumb'
echo 'Building ARM HTTPS libraries (logs in build/network/)'
cmake -S "third_party/mbedtls-$MBEDTLS_VERSION" -B build/network/mbedtls \
    -DCMAKE_C_COMPILER=arm-nickel-linux-gnueabihf-gcc -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=arm -DCMAKE_C_FLAGS="$flags" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=/work/build/network/prefix > build/network/mbedtls-configure.log 2>&1
cmake --build build/network/mbedtls --target install -- -j4 > build/network/mbedtls-build.log 2>&1
mkdir -p build/network/curl
cd build/network/curl
"/work/third_party/curl-$CURL_VERSION/configure" --host=arm-nickel-linux-gnueabihf \
    --prefix=/work/build/network/prefix --disable-shared --enable-static \
    --with-mbedtls=/work/build/network/prefix --without-openssl \
    --without-libpsl --without-libidn2 --without-zlib --without-brotli --without-zstd \
    --without-nghttp2 --without-nghttp3 --without-ngtcp2 --without-librtmp \
    --without-libssh2 --without-gssapi --disable-ldap --disable-ldaps --disable-ftp \
    --disable-file --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smtp --disable-smb --disable-gopher \
    --disable-mqtt --disable-manual --disable-docs --disable-libcurl-option \
    --enable-threaded-resolver --with-ca-bundle=/mnt/onboard/.adds/kobo-spark/cacert.pem \
    CC=arm-nickel-linux-gnueabihf-gcc CFLAGS="$flags" > ../curl-configure.log 2>&1
make -j4 > ../curl-build.log 2>&1
make install >> ../curl-build.log 2>&1
printf '%s\n' "$stamp" > ../ready
