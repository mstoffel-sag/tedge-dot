#!/bin/sh
# Cross-build the C implementation inside the image defined by ./Dockerfile.
#
#   ARCH      Debian architecture to build for: amd64 | arm64 | armhf
#   GLIBC_MIN Oldest glibc the binary must run against (default 2.17 = RHEL 7 /
#             Debian 8 era). zig pins the symbol versions; a native build
#             cannot, which is why an ubuntu-24.04 build needs glibc >= 2.39.
#   SRC       Repository root, read-only (default /src)
#   OUT       Build directory (default /out)
#   VERSION   Version `tedge-dot --version` reports (default: the CMake project version)
#
# Extra arguments are passed through to the configure step, e.g. -DTDOT_OPCUA=OFF.
set -eu

ARCH="${ARCH:-amd64}"
GLIBC_MIN="${GLIBC_MIN:-2.17}"
SRC="${SRC:-/src}"
OUT="${OUT:-/out}"

case "$ARCH" in
  amd64) ZIG_ARCH=x86_64;  DEB_MULTIARCH=x86_64-linux-gnu;     ABI=gnu;         PROC=x86_64 ;;
  arm64) ZIG_ARCH=aarch64; DEB_MULTIARCH=aarch64-linux-gnu;    ABI=gnu;         PROC=aarch64 ;;
  armhf) ZIG_ARCH=arm;     DEB_MULTIARCH=arm-linux-gnueabihf;  ABI=gnueabihf;   PROC=armv7-a ;;
  *) echo "build.sh: unsupported ARCH '$ARCH' (want amd64|arm64|armhf)" >&2; exit 2 ;;
esac

ZIG_TARGET="${ZIG_ARCH}-linux-${ABI}.${GLIBC_MIN}"
export ZIG_TARGET DEB_MULTIARCH
export TDOT_SYSTEM_PROCESSOR="$PROC"

# CMake wants a single executable per tool, so wrap zig's multi-call driver.
for tool in cc c++ ar ranlib; do
  case "$tool" in
    cc|c++) target_flag="-target $ZIG_TARGET" ;;
    *)      target_flag="" ;;
  esac
  name=$(echo "$tool" | tr '+' 'x')   # c++ -> cxx
  printf '#!/bin/sh\nexec zig %s %s "$@"\n' "$tool" "$target_flag" \
    > "/usr/local/bin/zig-$name"
  chmod +x "/usr/local/bin/zig-$name"
done

# Resolve the target architecture's .pc files, not the image's native ones.
export PKG_CONFIG_LIBDIR="/usr/lib/${DEB_MULTIARCH}/pkgconfig:/usr/share/pkgconfig"

echo "==> cross-building $ARCH (zig target $ZIG_TARGET)"
cmake -B "$OUT" -S "$SRC/impl/c" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/tdot-cross/toolchain.cmake \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DTDOT_OPCUA_VENDORED=ON \
  -DTDOT_NETSNMP_HOST="$DEB_MULTIARCH" \
  ${VERSION:+"-DTDOT_BUILD_VERSION=$VERSION"} \
  "$@" \
  -DTDOT_BENCH=OFF # last, so no argument above can put the bench module in a release
cmake --build "$OUT"

echo "==> built:"
ls -l "$OUT/tedge-dot" "$OUT/tedge-dot-golden"

# The glibc floor is the reason this build exists: fail if anything statically linked in
# (open62541, mbedTLS, net-snmp) pulled in a newer symbol version than GLIBC_MIN.
max_glibc=$("${DEB_MULTIARCH}-objdump" -T "$OUT/tedge-dot" \
  | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
echo "==> highest glibc symbol version: $max_glibc (floor $GLIBC_MIN)"
if [ "$(printf '%s\n%s\n' "$max_glibc" "$GLIBC_MIN" | sort -V | tail -1)" != "$GLIBC_MIN" ]; then
  echo "build.sh: tedge-dot needs glibc $max_glibc, above the $GLIBC_MIN floor" >&2
  exit 1
fi
