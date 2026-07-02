#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-build}"
build_type="${BUILD_TYPE:-Release}"
generator="${CMAKE_GENERATOR:-}"
run_tests="${RUN_TESTS:-1}"
clean_build="${CLEAN_BUILD:-1}"

cd "$repo_root"

log() {
    printf '[package-deb] %s\n' "$*"
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf '[package-deb] missing required command: %s\n' "$1" >&2
        exit 1
    fi
}

require_openssl_dev() {
    if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
        return
    fi

    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists openssl; then
        return
    fi

    if [[ -f /usr/include/openssl/sha.h ]] && \
        (compgen -G '/usr/lib/*/libcrypto.so' >/dev/null || [[ -f /usr/lib/libcrypto.so ]]); then
        return
    fi

    cat >&2 <<'EOF'
[package-deb] missing OpenSSL development files.
[package-deb] Debian/UOS/Ubuntu install command:
[package-deb]   sudo apt install libssl-dev
[package-deb] If OpenSSL is installed in a custom location, run with:
[package-deb]   OPENSSL_ROOT_DIR=/path/to/openssl ./scripts/package_deb.sh
EOF
    exit 1
}

require_command cmake
require_command cpack
require_command ctest
require_command dpkg-deb
require_openssl_dev

if [[ -z "$generator" ]]; then
    if command -v ninja >/dev/null 2>&1; then
        generator="Ninja"
    else
        generator="Unix Makefiles"
    fi
elif [[ "$generator" == "Ninja" ]]; then
    require_command ninja
fi

if [[ "$clean_build" == "1" ]]; then
    log "removing $build_dir"
    rm -rf "$build_dir"
fi

log "configuring $build_type build with $generator"
cmake_args=(-DCMAKE_BUILD_TYPE="$build_type")
if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
    cmake_args+=(-DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR")
fi
cmake -S . -B "$build_dir" -G "$generator" "${cmake_args[@]}"

log "building"
cmake --build "$build_dir"

if [[ "$run_tests" == "1" ]]; then
    log "running tests"
    ctest --test-dir "$build_dir" --output-on-failure
else
    log "skipping tests because RUN_TESTS=$run_tests"
fi

log "creating deb package"
cpack --config "$build_dir/CPackConfig.cmake"

package="$(find . -maxdepth 1 -type f -name 'lan-file-transfer_*.deb' -printf '%T@ %p\n' \
    | sort -nr \
    | awk 'NR == 1 {print $2}')"

if [[ -z "$package" ]]; then
    printf '[package-deb] package was not generated\n' >&2
    exit 1
fi

required_entries=(
    './usr/bin/sender'
    './usr/bin/receiver'
    './usr/bin/local-copy'
    './usr/bin/lan-gui'
    './usr/share/applications/lan-file-transfer.desktop'
    './usr/share/icons/hicolor/scalable/apps/lan-file-transfer.svg'
    './usr/share/lan-file-transfer/translations/lan-file-transfer_zh_CN.qm'
    './usr/share/doc/lan-file-transfer/CHANGELOG.md'
)

contents="$(dpkg-deb -c "$package")"

log "checking package contents"
for entry in "${required_entries[@]}"; do
    if ! grep -Fq "$entry" <<<"$contents"; then
        printf '[package-deb] missing package entry: %s\n' "$entry" >&2
        exit 1
    fi
    log "ok: $entry"
done

if [[ -x "$build_dir/src/lan-gui" ]]; then
    if ! grep -aFq '/usr/share/lan-file-transfer/translations' "$build_dir/src/lan-gui"; then
        printf '[package-deb] lan-gui does not contain the expected translation path\n' >&2
        exit 1
    fi
    log "ok: lan-gui translation path points to /usr/share"
fi

log "package ready: $package"
