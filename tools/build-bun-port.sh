#!/usr/bin/env bash
# PS5Term - build Bun as a native PS5 payload: bun.elf. Runs inside the
# ps5term-build container (tooling/bun-port/Dockerfile.build), from /work.
#
#   bash tools/build-bun-port.sh [STAGE...]        default: all stages in order
#   bash tools/build-bun-port.sh --stage DIR       copy bun.elf into DIR
#
# Stages: sources icu webkit. Every download is verified against tooling/bun-port/sources.json.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
port="$root/tooling/bun-port"
deps="$root/.deps/bun-port"
out="$root/build/bun-port"
sdk="$root/.deps/backend/ps5-payload-sdk"
jobs=${JOBS:-$(nproc)}
export PS5_PAYLOAD_SDK="$sdk"

die() { echo "build-bun-port: $*" >&2; exit 1; }
source_field() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]])' "$port/sources.json" "$1" "$2"; }

# fetch NAME FILE: download sources.json[NAME].url to $deps/FILE and verify its sha256.
fetch() {
    local name=$1 file=$deps/$2 url sha
    url=$(source_field "$name" url)
    sha=$(source_field "$name" sha256)
    if [[ ! -f "$file" ]]; then
        mkdir -p "$deps"
        curl -fsSL "$url" -o "$file.download"
        mv "$file.download" "$file"
    fi
    echo "$sha  $file" | sha256sum -c --quiet - || die "checksum mismatch for $file"
}

# git_pin NAME DIR [SPARSE...]: shallow checkout of sources.json[NAME].commit.
git_pin() {
    local name=$1 dir=$2 url commit
    shift 2
    url=$(source_field "$name" url)
    commit=$(source_field "$name" commit)
    if [[ ! -d "$dir/.git" ]]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    if (($#)); then
        git -C "$dir" sparse-checkout init --cone
        git -C "$dir" sparse-checkout set "$@"
    fi
    if [[ "$(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null)" != "$commit" ]]; then
        git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$commit"
        git -C "$dir" checkout -q --force FETCH_HEAD
    fi
}

# apply_patch DIR PATCH: apply once (reset by git_pin's forced checkout).
apply_patch() {
    if git -C "$1" apply --check "$2" 2>/dev/null; then
        git -C "$1" apply "$2"
    else
        git -C "$1" apply --reverse --check "$2" || die "patch $2 does not apply to $1"
    fi
}

# Compiler flags shared by every C/C++ component compiled for the payload. ps5compat/include
# is -I so its FreeBSD 14 header overlays are searched before the SDK's -isystem headers.
ps5_cflags="-DPS5TERM_PS5_PAYLOAD=1 -I$port/ps5compat/include -include ps5compat_prelude.h -fno-omit-frame-pointer -ffunction-sections -fdata-sections"
# Linker flags for every payload executable: ps5compat (whole archive, with its --wrap list),
# libexecinfo.a (backtrace over libunwind) from ps5compat, the empty FreeBSD base archives
# (tooling/bun-port/Makefile EMPTY_SYSLIBS), the SDK's libunwind, and libkernel_sys for
# elfldr_spawn.
ps5_ldflags="-L$out -L$out/syslibs -Wl,--whole-archive -lps5compat -Wl,--no-whole-archive $(tr '\n' ' ' <"$port/ps5compat/ps5compat.ldflags") -lexecinfo -lunwind -lkernel_sys"

# Rust for the payload: the custom target, std rebuilt for it, FreeBSD 11 libc ABI (the SDK's).
# In FreeBSD 11 mode the libc crate links stat/readdir/kevent/... as "name@FBSD_1.0" versioned
# symbols, which the SDK's unversioned libc/libkernel stubs cannot satisfy, so std and every
# crate use a copy of the libc crate with those version suffixes dropped (same functions, same
# FreeBSD 11 structures): $out/libc-ps5, patched into std's workspace and passed to cargo.
rust_toolchain=$(source_field rust toolchain)
rust_target="$port/x86_64-ps5-freebsd.json"
rust_libc_version=0.2.189
rust_env() {
    export RUSTUP_HOME="$deps/rustup" CARGO_HOME="$deps/cargo"
    local toolchain=$RUSTUP_HOME/toolchains/$rust_toolchain-x86_64-unknown-linux-gnu
    if [[ ! -x "$toolchain/bin/cargo" ]]; then
        rustup toolchain install "$rust_toolchain" --profile minimal -c rust-src >/dev/null
    fi
    export PATH="$toolchain/bin:$PATH"
    unset RUST_LIBC_UNSTABLE_FREEBSD_VERSION
    export CARGO_TARGET_X86_64_PS5_FREEBSD_LINKER="$sdk/bin/prospero-clang"
    local library=$toolchain/lib/rustlib/src/rust/library
    if [[ ! -f "$out/libc-ps5/.ps5" ]]; then
        cargo fetch -q --manifest-path "$library/Cargo.toml"
        local registry
        registry=$(echo "$CARGO_HOME"/registry/src/*/libc-$rust_libc_version)
        [[ -d "$registry" ]] || die "libc $rust_libc_version is not in $CARGO_HOME"
        rm -rf "$out/libc-ps5" && cp -r "$registry" "$out/libc-ps5"
        grep -rlE '@FBSD_1\.[01]"' "$out/libc-ps5/src" | xargs sed -i -E 's/@FBSD_1\.[01]"/"/g'
        touch "$out/libc-ps5/.ps5"
    fi
    if ! grep -q "^libc = { path = '$out/libc-ps5' }" "$library/Cargo.toml"; then
        sed -i "/^\[patch.crates-io\]/a libc = { path = '$out/libc-ps5' }" "$library/Cargo.toml"
        cargo update -q -p libc --manifest-path "$library/Cargo.toml"
    fi
    patch -s -p1 -N -r - -d "$library" <"$port/patches/rust-std-ps5.patch" >/dev/null || true
    grep -q 'd_fileno as u64' "$library/std/src/sys/fs/unix.rs" || die "rust-std-ps5.patch did not apply"
    # Payload link: ps5compat and friends, plus the SDK's libc stubs again after Rust's
    # -nodefaultlibs (which makes prospero-clang drop them).
    local flag
    # libc_unstable_freebsd_version=11: the libc crate's FreeBSD 11 structures (stat, dirent,
    # kevent, statfs) are the ones the SDK's libc fills in.
    RUSTFLAGS='--cfg libc_unstable_freebsd_version="11" --check-cfg cfg(libc_unstable_freebsd_version,values("11"))'
    for flag in $ps5_ldflags -Wl,-Bdynamic -lSceLibcInternal -lSceNet -lc; do
        RUSTFLAGS+=" -C link-arg=$flag"
    done
    export RUSTFLAGS
}
cargo_ps5() {
    cargo "$@" -Zbuild-std=std,panic_abort -Zjson-target-spec --target "$rust_target" \
        --config "patch.crates-io.libc.path='$out/libc-ps5'"
}

stage_sources() {
    git_pin webkit "$deps/WebKit" Source/JavaScriptCore Source/WTF Source/bmalloc Source/cmake \
        Source/ThirdParty/capstone Source/ThirdParty/unifdef Source/ThirdParty/gtest \
        Source/ThirdParty/gmock Tools/Scripts Tools/TestWebKitAPI Tools/jsc icu
    apply_patch "$deps/WebKit" "$port/patches/webkit-ps5-nojit.patch"
    fetch icu icu4c-sources.tgz
}

stage_compat() {
    make -C "$port" -j"$jobs" OUT="$out" "$out/libps5compat.a" "$out/libexecinfo.a" syslibs
}

stage_icu() {
    local host=$out/icu-host src=$out/icu-src major
    major=$(source_field icu version)
    major=${major%%.*}
    if [[ ! -x "$host/source/bin/icupkg" ]]; then
        rm -rf "$host" && mkdir -p "$host"
        tar -xf "$deps/icu4c-sources.tgz" -C "$host" --strip-components=1
        (cd "$host/source" && CC=clang-23 CXX=clang++-23 CFLAGS=-Os CXXFLAGS=-Os \
            ./configure --disable-shared --enable-static --disable-samples --disable-tests >/dev/null &&
            make -j"$jobs" >/dev/null)
    fi
    rm -rf "$src" && mkdir -p "$src"
    tar -xf "$deps/icu4c-sources.tgz" -C "$src" --strip-components=1
    cd "$src/source"
    # Same data filter as WebKit's Dockerfile.freebsd: Bun has no converter/transliterator users.
    "$host/source/bin/icupkg" -l "data/in/icudt${major}l.dat" |
        grep -E '\.(cnv|spp|cfu)$|^cnvalias\.icu$|^translit/|^rbnf/|^unames\.icu$' |
        grep -vE '^rbnf/(root|res_index|ja|zh|zh_Hant)\.res$' > data/in/rm.lst
    "$host/source/bin/icupkg" --auto_toc_prefix -r data/in/rm.lst "data/in/icudt${major}l.dat" data/in/filtered.dat
    mv -f data/in/filtered.dat "data/in/icudt${major}l.dat"
    CC="$sdk/bin/prospero-clang" CXX="$sdk/bin/prospero-clang++" AR="$sdk/bin/prospero-ar" \
        RANLIB="$sdk/bin/prospero-ranlib" \
        CFLAGS="-Os $ps5_cflags -DU_STATIC_IMPLEMENTATION=1 -std=c17" \
        CXXFLAGS="-Os $ps5_cflags -DU_STATIC_IMPLEMENTATION=1 -std=c++20 -fno-exceptions -fno-c++-static-destructors" \
        ./configure --host=x86_64-unknown-freebsd --with-cross-build="$host/source" \
        --enable-static --disable-shared --with-data-packaging=static --disable-samples \
        --disable-tests --disable-debug --disable-tools --disable-extras >/dev/null
    make -j"$jobs" GENCCODE_ASSEMBLY="-a gcc" >/dev/null
    rm -rf "$out/icu" && mkdir -p "$out/icu/lib" "$out/icu/include/unicode"
    cp lib/libicudata.a lib/libicui18n.a lib/libicuuc.a "$out/icu/lib/"
    cp -r i18n/unicode/* common/unicode/* "$out/icu/include/unicode/"
    cd "$root"
}

# JavaScriptCore runs as LLInt only (offlineasm x86_64): Bun's WebKit fork does not compile with
# ENABLE_JIT=OFF (its DOMJIT signatures need the DFG headers), so the JIT tiers are compiled in
# but webkit-ps5-nojit.patch forces Options::useJIT() off on PS5 payloads: no executable memory
# is ever allocated for generated code.
stage_webkit() {
    local build=$out/webkit-build flags
    # JS_NO_EXPORT: the PS5 target accepts __declspec, which JSBase.h would otherwise pick for a
    # static library that exports nothing.
    flags="$ps5_cflags -DU_STATIC_IMPLEMENTATION=1 -DJS_NO_EXPORT=1 -isystem $out/icu/include"
    mkdir -p "$build"
    cmake -S "$deps/WebKit" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DCMAKE_BUILD_TYPE=Release -DPORT=JSCOnly -DENABLE_STATIC_JSC=ON -DUSE_THIN_ARCHIVES=OFF \
        -DENABLE_BUN_SKIP_FAILING_ASSERTIONS=ON -DUSE_BUN_JSC_ADDITIONS=ON -DUSE_BUN_EVENT_LOOP=ON \
        -DUSE_MIMALLOC=OFF -DUSE_EXTERNAL_MIMALLOC=OFF -DUSE_SYSTEM_MALLOC=OFF \
        -DENABLE_C_LOOP=OFF -DENABLE_API_TESTS=OFF \
        -DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON -DENABLE_REMOTE_INSPECTOR=ON \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags -fno-c++-static-destructors" \
        -DICU_ROOT="$out/icu" -DICU_INCLUDE_DIR="$out/icu/include" \
        -DICU_UC_LIBRARY_RELEASE="$out/icu/lib/libicuuc.a" \
        -DICU_I18N_LIBRARY_RELEASE="$out/icu/lib/libicui18n.a" \
        -DICU_DATA_LIBRARY_RELEASE="$out/icu/lib/libicudata.a" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH -DCMAKE_EXE_LINKER_FLAGS="$ps5_ldflags" \
        >"$build/cmake.log"
    ninja -C "$build" -j"$jobs" jsc
    # Same layout as the bun-webkit-freebsd prebuilt (WebKit's Dockerfile.freebsd), which Bun's
    # build consumes from its cache directory.
    local dest=$out/webkit
    rm -rf "$dest" && mkdir -p "$dest/lib" "$dest/include/JavaScriptCore" "$dest/include/unicode" \
        "$dest/Source/JavaScriptCore" "$dest/bin"
    cp "$out"/icu/lib/*.a "$build"/lib/*.a "$dest/lib/"
    cp -r "$out"/icu/include/unicode/* "$dest/include/unicode/"
    cp "$build"/*.h "$dest/include/"
    cp "$build/bin/jsc" "$dest/bin/"
    find "$build/JavaScriptCore/DerivedSources/" -name '*.h' -exec cp {} "$dest/include/JavaScriptCore/" \;
    cp "$build/JavaScriptCore/Scripts/CombinedDomains.json" "$dest/"
    find "$build/JavaScriptCore/Headers/JavaScriptCore/" -name '*.h' -exec cp {} "$dest/include/JavaScriptCore/" \;
    find "$build/JavaScriptCore/PrivateHeaders/JavaScriptCore/" -name '*.h' -exec cp {} "$dest/include/JavaScriptCore/" \;
    cp -r "$build/WTF/Headers/wtf/" "$dest/include"
    cp -r "$build/bmalloc/Headers/bmalloc/" "$dest/include"
    cp -r "$deps/WebKit/Source/JavaScriptCore/Scripts" "$dest/Source/JavaScriptCore"
    cp "$deps/WebKit/Source/JavaScriptCore/create_hash_table" "$dest/Source/JavaScriptCore"
}

# Rust std smoke program for the payload target (tooling/bun-port/probes/rust_probe).
stage_rustprobe() {
    rust_env
    (cd "$port/probes/rust_probe" && CARGO_TARGET_DIR="$out/rust-probe-target" cargo_ps5 build --release)
    cp "$out/rust-probe-target/x86_64-ps5-freebsd/release/probe_rust" "$out/probe_rust.elf"
}

# --stage DIR: the app's assets/bun (copied to /data/ps5term/bun by PS5Term on first use).
stage_into() {
    local dest=$1 version
    [[ -f "$out/bun.elf" ]] || die "build bun.elf first"
    rm -rf "$dest" && mkdir -p "$dest/bin"
    cp "$out/bun.elf" "$dest/bin/bun.elf"
    version="bun-$(source_field bun version)+$(sha256sum "$out/bun.elf" | cut -c1-12)"
    echo "$version" >"$dest/version"
    echo "build-bun-port: staged $version in $dest"
}

if [[ "${1:-}" == --stage ]]; then
    [[ $# -eq 2 ]] || die "usage: --stage DIR"
    stage_into "$2"
    exit 0
fi

stages=("$@")
((${#stages[@]})) || stages=(sources compat icu webkit)
for stage in "${stages[@]}"; do
    declare -F "stage_$stage" >/dev/null || die "unknown stage: $stage"
    echo "build-bun-port: stage $stage"
    "stage_$stage"
done
