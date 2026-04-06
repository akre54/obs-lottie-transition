#!/usr/bin/env bash
set -euo pipefail

src_dir="$1"
build_dir="$2"
prefix_dir="$3"
meson_bin="${MESON_BIN:-$(command -v meson)}"
ninja_bin="${NINJA_BIN:-$(command -v ninja)}"
lipo_bin="${LIPO_BIN:-$(command -v lipo)}"

common_args=(
  "--prefix" "$prefix_dir"
  "-Ddefault_library=static"
  "-Dengines=sw"
  "-Dloaders=lottie"
  "-Dextra="
  "-Dtools="
  "-Dsavers="
  "-Dbindings=capi"
  "-Dtests=false"
  "-Dfile=true"
)

if [ ! -x "$meson_bin" ] || [ ! -x "$ninja_bin" ]; then
  echo "meson or ninja not found" >&2
  exit 1
fi

setup_build() {
  local build_dir="$1"
  local prefix_dir="$2"
  shift 2

  local args=("${common_args[@]}")
  args[1]="$prefix_dir"

  if [ ! -d "$build_dir" ]; then
    "$@" "$meson_bin" setup "$build_dir" "$src_dir" "${args[@]}"
  else
    "$@" "$meson_bin" setup --wipe "$build_dir" "$src_dir" "${args[@]}"
  fi

  "$@" "$ninja_bin" -C "$build_dir" install
}

if [ "$(uname -s)" = "Darwin" ]; then
  if [ ! -x "$lipo_bin" ]; then
    echo "lipo not found" >&2
    exit 1
  fi

  arm64_build="${build_dir}-arm64"
  x86_64_build="${build_dir}-x86_64"
  arm64_prefix="${prefix_dir}-arm64"
  x86_64_prefix="${prefix_dir}-x86_64"

  setup_build "$arm64_build" "$arm64_prefix" env CFLAGS="-arch arm64" CXXFLAGS="-arch arm64" LDFLAGS="-arch arm64"
  setup_build "$x86_64_build" "$x86_64_prefix" env CFLAGS="-arch x86_64" CXXFLAGS="-arch x86_64" LDFLAGS="-arch x86_64"

  mkdir -p "$prefix_dir/lib" "$prefix_dir/include"
  "$lipo_bin" -create \
    "$arm64_prefix/lib/libthorvg-1.a" \
    "$x86_64_prefix/lib/libthorvg-1.a" \
    -output "$prefix_dir/lib/libthorvg-1.a"
  cp -R "$arm64_prefix/include/." "$prefix_dir/include/"
else
  setup_build "$build_dir" "$prefix_dir"
fi
