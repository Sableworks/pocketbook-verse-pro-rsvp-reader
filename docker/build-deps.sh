#!/bin/bash
# Buduje libxml2 + libzip pod toolchain PocketBook (ARM).
# Celowo NIE używamy CMAKE_TOOLCHAIN_FILE z SDK — psuje budowę zależności.
set -euxo pipefail

unset CMAKE_TOOLCHAIN_FILE || true

SDK_BASE="${SDK_BASE:-/SDK}"
PB_DEPS="${PB_DEPS:-/pb-deps}"
SYSROOT="${SDK_BASE}/usr/arm-obreey-linux-gnueabi/sysroot"

export PATH="${SDK_BASE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${SDK_BASE}/usr/lib:${SDK_BASE}/lib:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

if [ -x "${SDK_BASE}/usr/bin/arm-obreey-linux-gnueabi-gcc" ]; then
  CC=arm-obreey-linux-gnueabi-gcc
elif [ -x "${SDK_BASE}/usr/bin/arm-obreey-linux-gnueabi-clang" ]; then
  CC=arm-obreey-linux-gnueabi-clang
else
  echo "Brak kompilatora ARM w ${SDK_BASE}/usr/bin" >&2
  ls -la "${SDK_BASE}/usr/bin" | head -80 >&2
  exit 1
fi

AR="${SDK_BASE}/usr/bin/arm-obreey-linux-gnueabi-ar"
RANLIB="${SDK_BASE}/usr/bin/arm-obreey-linux-gnueabi-ranlib"

# Flagi typowe dla PB + sysroot. -Wno-error: wrapper PB bywa ostry.
CFLAGS="-O2 -fPIC -Wno-error --sysroot=${SYSROOT} -I${SYSROOT}/usr/include"
LDFLAGS="--sysroot=${SYSROOT} -L${SYSROOT}/usr/lib -L${SYSROOT}/lib"

export CC AR RANLIB CFLAGS LDFLAGS
export CXXFLAGS="${CFLAGS}"

mkdir -p "${PB_DEPS}/include" "${PB_DEPS}/lib" "${PB_DEPS}/lib/pkgconfig" /tmp/pb-build
cd /tmp/pb-build

# --- szybki test: kompilator musi umieć zrobić plik .o ---
cat > /tmp/pb-build/smoke.c <<'EOF'
int pb_smoke_ok(void) { return 1; }
EOF
${CC} ${CFLAGS} -c /tmp/pb-build/smoke.c -o /tmp/pb-build/smoke.o
${AR} rcs /tmp/pb-build/libsmoke.a /tmp/pb-build/smoke.o
echo "Compiler smoke test OK (${CC})"

# --- zlib z SDK (prawie zawsze jest) ---
ZLIB_H=""
ZLIB_LIB=""
for d in "${SYSROOT}/usr/include" "${SYSROOT}/include"; do
  if [ -f "${d}/zlib.h" ]; then ZLIB_H="${d}"; break; fi
done
for d in "${SYSROOT}/usr/lib" "${SYSROOT}/lib" "${SYSROOT}/usr/lib/arm-linux-gnueabi"; do
  if [ -f "${d}/libz.a" ]; then ZLIB_LIB="${d}/libz.a"; break; fi
  if [ -f "${d}/libz.so" ]; then ZLIB_LIB="${d}/libz.so"; break; fi
done

if [ -z "${ZLIB_H}" ] || [ -z "${ZLIB_LIB}" ]; then
  echo "Brak zlib w SDK — buduję własny (static)"
  curl -L --fail -o zlib.tar.gz \
    https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
  rm -rf zlib-src && mkdir zlib-src
  tar xz -C zlib-src --strip-components=1 -f zlib.tar.gz
  make -C zlib-src -j"$(nproc)" CC="${CC}" CFLAGS="${CFLAGS}" AR="${AR}" RANLIB="${RANLIB}" static
  cp zlib-src/libz.a "${PB_DEPS}/lib/"
  cp zlib-src/zlib.h zlib-src/zconf.h "${PB_DEPS}/include/"
  ZLIB_LIB="${PB_DEPS}/lib/libz.a"
  ZLIB_H="${PB_DEPS}/include"
else
  echo "Używam zlib z SDK: ${ZLIB_LIB}"
  ln -sfn "${ZLIB_H}/zlib.h" "${PB_DEPS}/include/zlib.h"
  [ -f "${ZLIB_H}/zconf.h" ] && ln -sfn "${ZLIB_H}/zconf.h" "${PB_DEPS}/include/zconf.h"
  cp -a "${ZLIB_LIB}" "${PB_DEPS}/lib/"
  ZLIB_LIB="${PB_DEPS}/lib/$(basename "${ZLIB_LIB}")"
fi

# Wspólne argumenty CMake dla cross + „nie linkuj testowych programów”
CMAKE_CROSS=(
  -DCMAKE_SYSTEM_NAME=Linux
  -DCMAKE_SYSTEM_PROCESSOR=arm
  -DCMAKE_C_COMPILER="${CC}"
  -DCMAKE_C_FLAGS="${CFLAGS}"
  -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS}"
  -DCMAKE_AR="${AR}"
  -DCMAKE_RANLIB="${RANLIB}"
  -DCMAKE_SYSROOT="${SYSROOT}"
  -DCMAKE_FIND_ROOT_PATH="${PB_DEPS};${SYSROOT}"
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
  -DCMAKE_INSTALL_PREFIX="${PB_DEPS}"
  -DBUILD_SHARED_LIBS=OFF
)

# --- libxml2 przez CMake (omija kapryśny ./configure) ---
curl -L --fail -o libxml2.tar.xz \
  https://download.gnome.org/sources/libxml2/2.12/libxml2-2.12.7.tar.xz
rm -rf libxml2-src libxml2-build
mkdir libxml2-src
tar xJ -C libxml2-src --strip-components=1 -f libxml2.tar.xz

cmake -S libxml2-src -B libxml2-build \
  "${CMAKE_CROSS[@]}" \
  -DLIBXML2_WITH_PYTHON=OFF \
  -DLIBXML2_WITH_LZMA=OFF \
  -DLIBXML2_WITH_ICONV=OFF \
  -DLIBXML2_WITH_ICU=OFF \
  -DLIBXML2_WITH_PROGRAMS=OFF \
  -DLIBXML2_WITH_TESTS=OFF \
  -DLIBXML2_WITH_ZLIB=ON \
  -DZLIB_INCLUDE_DIR="${PB_DEPS}/include" \
  -DZLIB_LIBRARY="${ZLIB_LIB}"

cmake --build libxml2-build -j"$(nproc)"
cmake --install libxml2-build

# --- libzip przez CMake ---
# Ważne: bez -Werror=implicit-function-declaration CMake „znajduje”
# funkcje z macOS/BSD (clonefile, memcpy_s…), których nie ma na Linux/ARM PB.
curl -L --fail -o libzip.tar.gz \
  https://github.com/nih-at/libzip/releases/download/v1.10.1/libzip-1.10.1.tar.gz
rm -rf libzip-src libzip-build
mkdir libzip-src
tar xz -C libzip-src --strip-components=1 -f libzip.tar.gz

LIBZIP_CFLAGS="${CFLAGS} -Werror=implicit-function-declaration"

cmake -S libzip-src -B libzip-build \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_C_COMPILER="${CC}" \
  -DCMAKE_C_FLAGS="${LIBZIP_CFLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS}" \
  -DCMAKE_AR="${AR}" \
  -DCMAKE_RANLIB="${RANLIB}" \
  -DCMAKE_SYSROOT="${SYSROOT}" \
  -DCMAKE_FIND_ROOT_PATH="${PB_DEPS};${SYSROOT}" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_INSTALL_PREFIX="${PB_DEPS}" \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TOOLS=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_DOC=OFF \
  -DBUILD_REGRESS=OFF \
  -DENABLE_COMMONCRYPTO=OFF \
  -DENABLE_GNUTLS=OFF \
  -DENABLE_MBEDTLS=OFF \
  -DENABLE_OPENSSL=OFF \
  -DENABLE_BZIP2=OFF \
  -DENABLE_LZMA=OFF \
  -DENABLE_ZSTD=OFF \
  -DHAVE_CLONEFILE=FALSE \
  -DHAVE_FICLONERANGE=FALSE \
  -DHAVE_MEMCPY_S=FALSE \
  -DHAVE_STRNCPY_S=FALSE \
  -DHAVE_STRERROR_S=FALSE \
  -DHAVE_STRERRORLEN_S=FALSE \
  -DHAVE_ARC4RANDOM=FALSE \
  -DHAVE_ARC4RANDOM_BUF=FALSE \
  -DHAVE_GETPROGNAME=FALSE \
  -DHAVE_SETMODE=FALSE \
  -DHAVE_EXPLICIT_MEMSET=FALSE \
  -DZLIB_INCLUDE_DIR="${PB_DEPS}/include" \
  -DZLIB_LIBRARY="${ZLIB_LIB}"

cmake --build libzip-build -j"$(nproc)"
cmake --install libzip-build

# Weryfikacja
ls -la "${PB_DEPS}/lib" "${PB_DEPS}/include" || true
test -f "${PB_DEPS}/lib/libxml2.a"
# libzip bywa jako libzip.a
test -f "${PB_DEPS}/lib/libzip.a"

echo "PB deps OK"
