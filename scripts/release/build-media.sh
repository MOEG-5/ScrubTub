#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Build private media tools. Never installs into a system prefix.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
root=${SCRUBTUB_MEDIA_ROOT:-"$repo/out/media"}
mkdir -p "$root"
root=$(cd "$root" && pwd)
prefix="$root/prefix"
mkdir -p "$root/sources" "$prefix" "$root/manifests"
fetch() {
    local file=$1 url=$2 hash=$3
    if [[ ! -f "$root/sources/$file" ]]; then
        curl --fail --location "$url" -o "$root/sources/$file.part"
        mv "$root/sources/$file.part" "$root/sources/$file"
    fi
    echo "$hash  $root/sources/$file" | sha256sum --check
}
fetch ffmpeg-9.0.1.tar.xz https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635
fetch mpv-0.41.0.tar.gz https://codeload.github.com/mpv-player/mpv/tar.gz/refs/tags/v0.41.0 ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209
[[ -d "$root/ffmpeg-9.0.1" ]] || tar -xf "$root/sources/ffmpeg-9.0.1.tar.xz" -C "$root"
[[ -d "$root/mpv-0.41.0" ]] || tar -xf "$root/sources/mpv-0.41.0.tar.gz" -C "$root"
jobs=${SCRUBTUB_BUILD_JOBS:-8}
fetch dav1d-1.5.4.tar.gz https://code.videolan.org/videolan/dav1d/-/archive/1.5.4/dav1d-1.5.4.tar.gz a1d5b63d2d38ec9bd03acf643caa51fa22edd1e89c5a109c4807717216bbec07
fetch libplacebo-7.360.1.tar.gz https://codeload.github.com/haasn/libplacebo/tar.gz/refs/tags/v7.360.1 d05fdf90bea2f629eaa2d115e909fd356388ac639e54f77b87a018a6d76224bd
[[ -d "$root/dav1d-1.5.4" ]] || tar -xf "$root/sources/dav1d-1.5.4.tar.gz" -C "$root"
[[ -d "$root/libplacebo-7.360.1" ]] || tar -xf "$root/sources/libplacebo-7.360.1.tar.gz" -C "$root"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Windows DLLs live in bin; this change is confined to this build process.
export PATH="$prefix/bin:$PATH"
meson_dependency() {
    local name=$1 source=$2
    shift 2
    local reconfigure=()
    [[ ! -f "$root/$name-build/build.ninja" ]] || reconfigure=(--reconfigure)
    local options=(--prefix="$prefix" --libdir=lib --buildtype=release --strip "$@")
    printf '%q ' meson setup "$root/$name-build" "$root/$source" "${options[@]}" > "$root/manifests/$name-configure.sh"
    printf '\n' >> "$root/manifests/$name-configure.sh"
    meson setup "${reconfigure[@]}" "$root/$name-build" "$root/$source" "${options[@]}"
    meson compile -C "$root/$name-build" -j "$jobs"
    meson install -C "$root/$name-build"
}
meson_dependency dav1d dav1d-1.5.4 -Denable_tools=false -Denable_tests=false
meson_dependency placebo libplacebo-7.360.1 -Dauto_features=disabled -Ddemos=false
mkdir -p "$root/ffmpeg-build"
cd "$root/ffmpeg-build"
# Keep built-in decoders, demuxers and parsers for broad input/probe coverage.
# Disable automatic external dependencies to make feature selection predictable.
args=(--prefix="$prefix" --enable-shared --disable-static --enable-rpath
    --enable-gpl --enable-version3 --disable-autodetect --disable-debug --disable-doc
    --disable-ffplay --disable-avdevice --disable-network --disable-hwaccels
    --disable-encoders --enable-encoder=mjpeg,png
    --disable-muxers --enable-muxer=image2,image2pipe
    --disable-protocols --enable-protocol=file,pipe
    --disable-filters --enable-filter=scale,format,showinfo,null,buffer,buffersink,abuffer,abuffersink,aformat,aresample
    --enable-zlib --enable-libdav1d)
printf '%q ' "$root/ffmpeg-9.0.1/configure" "${args[@]}" > "$root/manifests/ffmpeg-configure.sh"
printf '\n' >> "$root/manifests/ffmpeg-configure.sh"
"$root/ffmpeg-9.0.1/configure" "${args[@]}"
make -j"$jobs"
make install
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$repo"
mpv_args=(--prefix="$prefix" --libdir=lib --buildtype=release --strip
    -Dauto_features=disabled -Dlibmpv=false -Dcplayer=true -Dlua=luajit
    -Dgl=disabled -Dbuild-date=false)
# Native Windows threading is required even with optional features disabled.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) mpv_args+=(-Dwin32-threads=enabled) ;;
esac
printf '%q ' meson setup "$root/mpv-build" "$root/mpv-0.41.0" "${mpv_args[@]}" > "$root/manifests/mpv-configure.sh"
printf '\n' >> "$root/manifests/mpv-configure.sh"
if [[ -f "$root/mpv-build/build.ninja" ]]; then
    meson setup --reconfigure "$root/mpv-build" "$root/mpv-0.41.0" "${mpv_args[@]}"
else
    meson setup "$root/mpv-build" "$root/mpv-0.41.0" "${mpv_args[@]}"
fi
meson compile -C "$root/mpv-build" -j "$jobs"
meson install -C "$root/mpv-build"
"$prefix/bin/ffmpeg" -version > "$root/manifests/ffmpeg-version.txt"
"$prefix/bin/ffmpeg" -decoders > "$root/manifests/decoders.txt" 2>&1
"$prefix/bin/ffmpeg" -encoders > "$root/manifests/encoders.txt" 2>&1
"$prefix/bin/mpv" --no-config --version > "$root/manifests/mpv-version.txt"
for dependency in libass luajit zlib; do
    printf '%s ' "$dependency"
    pkg-config --modversion "$dependency"
done > "$root/manifests/external-dependencies.txt"
cp "$repo/scripts/release/build-media.sh" "$root/manifests/"
printf '\nPrivate media build installed in %s\n' "$prefix"
