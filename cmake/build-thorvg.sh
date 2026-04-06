#!/usr/bin/env bash
set -euo pipefail

src_dir="$1"
build_dir="$2"
prefix_dir="$3"
meson_bin="${MESON_BIN:-}"
ninja_bin="${NINJA_BIN:-}"
lipo_bin="${LIPO_BIN:-}"

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

uname_s="$(uname -s)"
is_windows=0
case "$uname_s" in
  MINGW*|MSYS*|CYGWIN*)
    is_windows=1
    ;;
esac

find_existing_install() {
  if [ -f "$prefix_dir/lib/thorvg-1.lib" ]; then
    printf '%s\n' "$prefix_dir/lib/thorvg-1.lib"
    return 0
  fi

  if [ -f "$prefix_dir/lib/libthorvg-1.a" ]; then
    printf '%s\n' "$prefix_dir/lib/libthorvg-1.a"
    return 0
  fi

  local multiarch_lib
  multiarch_lib="$(find "$prefix_dir/lib" -mindepth 2 -maxdepth 2 -name 'libthorvg-1.a' -print -quit 2>/dev/null || true)"
  if [ -n "$multiarch_lib" ]; then
    printf '%s\n' "$multiarch_lib"
    return 0
  fi

  return 1
}

if [ -z "$meson_bin" ]; then
  meson_bin="$(command -v meson || true)"
fi

if [ -z "$ninja_bin" ]; then
  ninja_bin="$(command -v ninja || true)"
fi

if [ ! -x "$meson_bin" ] || [ ! -x "$ninja_bin" ]; then
  echo "meson or ninja not found" >&2
  exit 1
fi

if existing_install="$(find_existing_install)"; then
  echo "Using existing ThorVG install at $existing_install"
  exit 0
fi

setup_build() {
  local build_dir="$1"
  local prefix_dir="$2"
  shift 2

  local args=("${common_args[@]}")
  args[1]="$prefix_dir"

  local meson_setup_args=()
  if [ "$is_windows" -eq 1 ]; then
    meson_setup_args+=("--vsenv")
  fi

  if [ ! -d "$build_dir" ]; then
    "$@" "$meson_bin" setup "${meson_setup_args[@]}" "$build_dir" "$src_dir" "${args[@]}"
  else
    "$@" "$meson_bin" setup "${meson_setup_args[@]}" --wipe "$build_dir" "$src_dir" "${args[@]}"
  fi

  "$@" "$ninja_bin" -C "$build_dir" install
}

if [ "$uname_s" = "Darwin" ]; then
  if [ -z "$lipo_bin" ]; then
    lipo_bin="$(command -v lipo || true)"
  fi

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
  if [ "$is_windows" -eq 1 ]; then
    setup_build "$build_dir" "$prefix_dir" env CC=cl CXX=cl
  else
    setup_build "$build_dir" "$prefix_dir"
  fi
fi
