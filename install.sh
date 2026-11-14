#!/usr/bin/env sh
set -eu

REPO="Hephaestus-Zero/hslice"
PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="${PREFIX}/bin"

need_cmd() {
    command -v "$1" >/dev/null 2>&1
}

install_from_source() {
    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT
    echo "Building hslice from source..."
    if need_cmd git; then
        git clone --depth 1 "https://github.com/${REPO}.git" "${tmpdir}/hslice" >/dev/null
    elif need_cmd curl; then
        curl -fsSL "https://github.com/${REPO}/archive/refs/heads/main.tar.gz" | tar -xz -C "${tmpdir}"
        mv "${tmpdir}/hslice-main" "${tmpdir}/hslice"
    else
        echo "Error: need git or curl to fetch source" >&2
        exit 1
    fi
    make -C "${tmpdir}/hslice" install PREFIX="${PREFIX}"
}

OS="$(uname -s)"
ARCH="$(uname -m)"
TARBALL=""

if [ "${OS}" = "Linux" ] && [ "${ARCH}" = "x86_64" ]; then
    TARBALL="hslice-linux-x86_64.tar.gz"
elif [ "${OS}" = "Darwin" ] && [ "${ARCH}" = "arm64" ]; then
    TARBALL="hslice-macos-arm64.tar.gz"
fi

if [ "${HSLICE_FROM_SOURCE:-0}" != "1" ] && [ -n "${TARBALL}" ] && need_cmd curl; then
    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"' EXIT
    BASE_URL="https://github.com/${REPO}/releases/latest/download"
    if curl -fsSL "${BASE_URL}/${TARBALL}" -o "${tmpdir}/${TARBALL}" 2>/dev/null && \
       curl -fsSL "${BASE_URL}/SHA256SUMS" -o "${tmpdir}/SHA256SUMS" 2>/dev/null; then
        if need_cmd sha256sum; then
            (cd "${tmpdir}" && grep "${TARBALL}" SHA256SUMS | sha256sum -c - >/dev/null)
        elif need_cmd shasum; then
            (cd "${tmpdir}" && grep "${TARBALL}" SHA256SUMS | shasum -a 256 -c - >/dev/null)
        fi
        tar -xzf "${tmpdir}/${TARBALL}" -C "${tmpdir}"
        mkdir -p "${BINDIR}"
        install -m 0755 "${tmpdir}/hslice" "${BINDIR}/hslice"
        install -m 0755 "${tmpdir}/hfind" "${BINDIR}/hfind"
        ln -sf hfind "${BINDIR}/hsearch"
        echo "Installed hslice, hfind, and hsearch to ${BINDIR} (verified SHA-256)"
        exit 0
    fi
fi

install_from_source
echo "Installed hslice, hfind, and hsearch to ${BINDIR}"
