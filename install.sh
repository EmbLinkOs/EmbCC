#!/bin/sh
# Installs EmbCC: the compiler, the Emb tools, and their headers and
# libraries for every target.
#
#   curl -fsSL https://raw.githubusercontent.com/EmbLinkOs/EmbCC/main/install.sh | sh
#
# Options go after `sh -s --`:
#
#   curl -fsSL .../install.sh | sh -s -- --prefix /opt/embcc --version v1.0.0
#
#   --prefix DIR    install into DIR/bin, DIR/lib/embcc and DIR/share.
#                   The default is $HOME/.local, or /usr/local when run
#                   as root. The script never runs sudo.
#   --version TAG   install the release TAG (v1.0.0, or 1.0.0). The
#                   default is the latest release.
#   --from-source   build the release from its source code instead of
#                   downloading a binary. This needs a C compiler and GNU
#                   make. With --from-source, `--version main` builds the
#                   main branch.
#   --help          print this text.
#
# A binary release is one tarball per OS and processor on GitHub
# Releases, with a SHA-256 file beside it. The tarball is checked before
# anything is written into the prefix, and a mismatch stops the install.
# Where there is no tarball for this machine, or the one there is does
# not run here, the release is built from source instead.
#
# An installed EmbCC finds its own files relative to bin/embcc, so the
# prefix can be anywhere and can be moved afterwards. Installing a version
# again replaces it.
#
# Environment:
#
#   EMBCC_INSTALL_BASE_URL    where releases are downloaded from, in the
#       layout of https://github.com/EmbLinkOs/EmbCC/releases (the
#       default): BASE/latest/download/embcc-version.txt holds the latest
#       release's tag, and BASE/download/TAG/ holds that release's files.
#       A local directory, or a file:// URL, works the same way.
#   EMBCC_INSTALL_SOURCE_URL  where source tarballs are downloaded from:
#       SOURCE/tags/TAG.tar.gz and SOURCE/heads/main.tar.gz. The default
#       is https://github.com/EmbLinkOs/EmbCC/archive/refs.
#   EMBCC_INSTALL_PLATFORM    the tarball to install, as OS-ARCH
#       (linux-x86_64, linux-aarch64, macos-x86_64, macos-aarch64),
#       instead of the one for this machine.
#
# Everything below is in functions, and the last line calls main, so a
# download of this script that was cut short runs nothing.

set -eu

# Run by zsh (`curl ... | zsh`), behave as sh does: split words, leave an
# unmatched pattern as it is, and run the EXIT trap at exit from the
# script rather than from the function that set it.
if [ -n "${ZSH_VERSION:-}" ]; then
    emulate sh
fi

REPO=EmbLinkOs/EmbCC

say() {
    printf 'embcc-install: %s\n' "$*"
}

die() {
    printf 'embcc-install: error: %s\n' "$*" >&2
    exit 1
}

have() {
    command -v "$1" > /dev/null 2>&1
}

usage() {
    cat << 'EOF'
Installs EmbCC, the compiler and the Emb tools, from a release.

usage: install.sh [--prefix DIR] [--version TAG] [--from-source]

  --prefix DIR    install into DIR/bin, DIR/lib/embcc and DIR/share
                  (default: $HOME/.local, or /usr/local when run as root)
  --version TAG   install the release TAG, such as v1.0.0
                  (default: the latest release)
  --from-source   build the release from source with cc and GNU make;
                  with it, --version main builds the main branch
  --help          print this text

Through curl:  curl -fsSL .../install.sh | sh -s -- --prefix DIR
EOF
}

# fetch URL FILE: download URL into FILE. A local path or a file:// URL is
# copied, so the installer can be tested without a network.
fetch() {
    case $1 in
        file://*)
            cp "${1#file://}" "$2" 2> /dev/null ;;
        /*)
            cp "$1" "$2" 2> /dev/null ;;
        https://*)
            if have curl; then
                curl -fsSL --proto '=https' --tlsv1.2 --retry 3 -o "$2" "$1"
            else
                wget -q -O "$2" "$1"
            fi ;;
        *)
            if have curl; then
                curl -fsSL --retry 3 -o "$2" "$1"
            else
                wget -q -O "$2" "$1"
            fi ;;
    esac
}

# sha256 FILE: the file's SHA-256, in lower-case hex.
sha256() {
    if have sha256sum; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

# The OS-ARCH name of this machine's tarball, or nothing when no tarball
# is built for it.
detect_platform() {
    if [ -n "${EMBCC_INSTALL_PLATFORM:-}" ]; then
        printf '%s\n' "$EMBCC_INSTALL_PLATFORM"
        return
    fi
    sys=
    cpu=
    case $(uname -s) in
        Linux) sys=linux ;;
        Darwin) sys=macos ;;
    esac
    case $(uname -m) in
        x86_64 | amd64) cpu=x86_64 ;;
        aarch64 | arm64) cpu=aarch64 ;;
    esac
    # An x86-64 shell under Rosetta 2 runs on Apple silicon, where the
    # native build is the one to install.
    if [ "$sys" = macos ] && [ "$cpu" = x86_64 ] &&
       [ "$(sysctl -n sysctl.proc_translated 2> /dev/null)" = 1 ]; then
        cpu=aarch64
    fi
    if [ -n "$sys" ] && [ -n "$cpu" ]; then
        printf '%s-%s\n' "$sys" "$cpu"
    fi
}

# The startup file a PATH line belongs in, for the user's login shell.
shell_rc() {
    case ${SHELL:-} in
        */zsh) printf '%s\n' '~/.zshrc' ;;
        */bash)
            if [ "$(uname -s)" = Darwin ]; then
                printf '%s\n' '~/.bash_profile'
            else
                printf '%s\n' '~/.bashrc'
            fi ;;
        *) printf '%s\n' '~/.profile' ;;
    esac
}

# install_tree STAGE PREFIX: copy an installed tree (bin, lib, share) into
# PREFIX. Each lib/embcc/VERSION directory the tree brings replaces the one
# in PREFIX, so files a reinstalled version no longer has do not survive.
# Every other file is removed before it is copied, so a symbolic link such
# as bin/embcc-ar is replaced rather than written through.
install_tree() {
    if [ -d "$1/lib/embcc" ]; then
        ls "$1/lib/embcc" | while IFS= read -r v; do
            rm -rf "${2:?}/lib/embcc/${v:?}"
        done
    fi
    (cd "$1" && find . ! -type d) | while IFS= read -r f; do
        rm -f "$2/${f#./}"
    done
    mkdir -p "$2" && cp -RP "$1/." "$2/"
}

# build_from_source TAG STAGE: build TAG with make and install it into
# STAGE, from where install_tree copies it like a binary release.
build_from_source() {
    tag=$1
    stage=$2
    if have gmake; then
        mk=gmake
    elif have make && make --version 2> /dev/null | grep -q 'GNU Make'; then
        mk=make
    else
        die "building from source needs GNU make, as make or gmake"
    fi
    have "${CC:-cc}" ||
        die "building from source needs a C compiler; install one, or set CC"
    case $tmpdir in
        *[[:space:]]*)
            die "the temporary directory $tmpdir has a space in its name," \
                "which make cannot handle; set TMPDIR to another directory" ;;
    esac
    if [ "$tag" = main ]; then
        src_url=$source_url/heads/main.tar.gz
    else
        src_url=$source_url/tags/$tag.tar.gz
    fi
    say "downloading the source of $tag from $src_url"
    fetch "$src_url" "$tmpdir/source.tar.gz" ||
        die "cannot download the source of $tag from $src_url"
    mkdir "$tmpdir/source"
    (cd "$tmpdir/source" && tar -xzf ../source.tar.gz) ||
        die "cannot unpack the source of $tag"
    # GitHub's tarball has one top directory, EmbCC-VERSION.
    srcdir=
    for d in $(ls "$tmpdir/source"); do
        if [ -f "$tmpdir/source/$d/Makefile" ]; then
            srcdir=$tmpdir/source/$d
        fi
    done
    [ -n "$srcdir" ] || die "the source of $tag has no Makefile"
    njobs=$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 2)
    say "building EmbCC $tag with $mk; this takes a few minutes"
    log=$tmpdir/build.log
    # The tools in parallel; then the install rule, which builds the
    # libraries and copies everything, in one job, because its
    # prerequisites are not ordered before its copying step.
    if ! (cd "$srcdir" && "$mk" -j"$njobs" all embdbg &&
          "$mk" -j1 install DESTDIR="$stage" PREFIX=/embcc) \
            > "$log" 2>&1 < /dev/null; then
        tail -n 30 "$log" >&2
        die "building EmbCC $tag failed; the end of the build log is above"
    fi
}

main() {
    opt_prefix=
    opt_version=
    opt_source=0
    while [ $# -gt 0 ]; do
        case $1 in
            --prefix)
                [ $# -ge 2 ] || die "--prefix needs a directory"
                opt_prefix=$2
                shift 2 ;;
            --prefix=*)
                opt_prefix=${1#--prefix=}
                shift ;;
            --version)
                [ $# -ge 2 ] || die "--version needs a release, such as v1.0.0"
                opt_version=$2
                shift 2 ;;
            --version=*)
                opt_version=${1#--version=}
                shift ;;
            --from-source)
                opt_source=1
                shift ;;
            -h | --help)
                usage
                exit 0 ;;
            *)
                die "unknown option '$1'; --help lists the options" ;;
        esac
    done

    base_url=${EMBCC_INSTALL_BASE_URL:-https://github.com/$REPO/releases}
    source_url=${EMBCC_INSTALL_SOURCE_URL:-https://github.com/$REPO/archive/refs}
    base_url=${base_url%/}
    source_url=${source_url%/}

    # ---- where, and which version --------------------------------------
    if [ -z "$opt_prefix" ]; then
        if [ "$(id -u)" = 0 ]; then
            opt_prefix=/usr/local
        else
            [ -n "${HOME:-}" ] ||
                die "HOME is not set; name a directory with --prefix"
            opt_prefix=$HOME/.local
        fi
    fi
    case $opt_prefix in
        /*) ;;
        *) opt_prefix=$(pwd)/$opt_prefix ;;
    esac
    while [ "$opt_prefix" != / ] && [ "${opt_prefix%/}" != "$opt_prefix" ]; do
        opt_prefix=${opt_prefix%/}
    done

    case $opt_version in
        '' | v* | main) ;;
        [0-9]*) opt_version=v$opt_version ;;
        *) die "--version takes a release tag, such as v1.0.0, not '$opt_version'" ;;
    esac
    case $opt_version in
        *[!A-Za-z0-9._+-]*)
            die "--version takes a release tag, such as v1.0.0, not '$opt_version'" ;;
    esac
    if [ "$opt_version" = main ] && [ "$opt_source" = 0 ]; then
        die "there is no binary release of the main branch; add --from-source to build it"
    fi

    # Refuse before downloading anything: the nearest directory that
    # exists has to be writable.
    d=$opt_prefix
    while [ ! -d "$d" ]; do
        d=$(dirname "$d")
    done
    [ -w "$d" ] || die "cannot write to $opt_prefix as $(id -un);" \
        "choose another directory with --prefix, or run this script as" \
        "a user who can write there"

    case "$base_url $source_url" in
        *https://* | *http://*)
            have curl || have wget ||
                die "downloading needs curl or wget; install one of them" ;;
    esac
    have tar || die "unpacking needs tar"
    have sha256sum || have shasum ||
        die "checking the download needs sha256sum or shasum"

    tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/embcc-install.XXXXXX") ||
        die "cannot create a temporary directory"
    trap 'rm -rf "${tmpdir:?}"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ -z "$opt_version" ]; then
        fetch "$base_url/latest/download/embcc-version.txt" "$tmpdir/latest" ||
            die "cannot find the latest release at $base_url/latest;" \
                "name one with --version, or build the main branch with" \
                "--from-source --version main"
        opt_version=$(head -n 1 "$tmpdir/latest" | tr -d '[:space:]')
        case $opt_version in
            v[0-9]*) ;;
            *) die "the latest release's embcc-version.txt names no tag" ;;
        esac
        case $opt_version in
            *[!A-Za-z0-9._+-]*)
                die "the latest release's embcc-version.txt names no tag" ;;
        esac
    fi
    tag=$opt_version
    version=${tag#v}

    # ---- a binary release ----------------------------------------------
    platform=$(detect_platform)
    stage=
    if [ "$opt_source" = 0 ] && [ -z "$platform" ]; then
        say "there is no binary release for $(uname -s) on $(uname -m);" \
            "building from source instead"
        opt_source=1
    fi
    if [ "$opt_source" = 0 ]; then
        name=embcc-$version-$platform
        asset=$name.tar.gz
        url=$base_url/download/$tag/$asset
        say "downloading $url"
        if ! fetch "$url" "$tmpdir/$asset"; then
            say "there is no binary release of EmbCC $tag for $platform;" \
                "building from source instead"
            opt_source=1
        fi
    fi
    if [ "$opt_source" = 0 ]; then
        fetch "$url.sha256" "$tmpdir/$asset.sha256" ||
            die "cannot download $url.sha256 to check $asset against;" \
                "nothing was installed"
        want=$(awk 'NR == 1 { print $1 }' "$tmpdir/$asset.sha256" |
               tr 'ABCDEF' 'abcdef')
        got=$(sha256 "$tmpdir/$asset")
        case $want in
            *[!0-9a-f]* | '') die "$asset.sha256 holds no SHA-256;" \
                                  "nothing was installed" ;;
        esac
        [ "${#want}" -eq 64 ] ||
            die "$asset.sha256 holds no SHA-256; nothing was installed"
        if [ "$got" != "$want" ]; then
            die "$asset does not match its checksum; nothing was installed" \
                "(expected $want, downloaded $got)"
        fi
        say "checksum verified: $got"

        mkdir "$tmpdir/unpack"
        (cd "$tmpdir/unpack" && tar -xzf "../$asset") ||
            die "cannot unpack $asset; nothing was installed"
        stage=$tmpdir/unpack/$name
        [ -f "$stage/bin/embcc" ] ||
            die "$asset holds no $name/bin/embcc; nothing was installed"
        # A tarball for this OS and processor can still fail to run: an
        # older C library than the one it was built against, or musl.
        if ! "$stage/bin/embcc" --version > "$tmpdir/run.txt" 2>&1; then
            say "the $platform build of EmbCC $tag does not run on this machine:"
            sed 's/^/    /' "$tmpdir/run.txt"
            say "building from source instead"
            opt_source=1
        fi
    fi

    # ---- or the source -------------------------------------------------
    if [ "$opt_source" = 1 ]; then
        build_from_source "$tag" "$tmpdir/stage"
        stage=$tmpdir/stage/embcc
        [ -f "$stage/bin/embcc" ] ||
            die "the build of $tag installed no bin/embcc"
    fi

    # ---- into the prefix -----------------------------------------------
    install_tree "$stage" "$opt_prefix" ||
        die "copying into $opt_prefix failed; the installation there may" \
            "be incomplete, and running this script again replaces it"
    "$opt_prefix/bin/embcc" --version > "$tmpdir/installed.txt" 2>&1 ||
        die "the installed $opt_prefix/bin/embcc does not run"
    say "installed $(awk 'NR == 1 { print $1, $2 }' "$tmpdir/installed.txt")" \
        "into $opt_prefix"

    # The versioned library directories of earlier installs stay, because
    # another copy of embcc may still use them. Say where they are.
    ls "$stage/lib/embcc" > "$tmpdir/ours"
    ls "$opt_prefix/lib/embcc" | while IFS= read -r v; do
        grep -qxF "$v" "$tmpdir/ours" ||
            say "an earlier version's files remain in $opt_prefix/lib/embcc/$v"
    done

    case ":${PATH:-}:" in
        *":$opt_prefix/bin:"*)
            found=$(command -v embcc 2> /dev/null || true)
            if [ -n "$found" ] && [ "$found" != "$opt_prefix/bin/embcc" ]; then
                say "note: $found comes first on PATH, before this embcc"
            fi ;;
        *)
            shown=$opt_prefix/bin
            case $shown in
                "${HOME:-/nonexistent}"/*) shown=\$HOME${shown#"$HOME"} ;;
            esac
            say "$opt_prefix/bin is not on your PATH. To add it, put this" \
                "line in $(shell_rc):"
            printf '\n    export PATH="%s:$PATH"\n\n' "$shown" ;;
    esac
}

main "$@"
