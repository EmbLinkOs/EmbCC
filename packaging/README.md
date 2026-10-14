# Packaging

This directory holds what turns a release of EmbCC into packages: the
scripts the release workflow runs, and the recipes for Homebrew, RPM and
Arch Linux. The installer, `install.sh`, is at the top of the repository,
because its URL is what the documentation gives users.

| File | What it is |
|---|---|
| `tarball.sh` | Packs an installed tree into `embcc-VERSION-OS-ARCH.tar.gz` and its `.sha256` file |
| `deb.sh` | Packs an installed tree into a Debian package, `embcc_VERSION_ARCH.deb`, and its `.sha256` file |
| `homebrew/embcc.rb` | The Homebrew formula, published in the tap `EmbLinkOs/homebrew-embcc` |
| `rpm/embcc.spec` | The RPM spec, for Fedora, RHEL and openSUSE |
| `arch/PKGBUILD` | The Arch Linux package, for the AUR |
| `set-version.sh` | Points the three recipes above at a release |

Every package is the tree `make install` writes: `bin/` with `embcc` and
the other tools, `lib/embcc/VERSION/` with the headers and the libraries
for each target, and `share/` with the man page and the CMake toolchain
file. The compiler finds its headers and libraries relative to its own
binary, so the same tree works under `/usr`, under `/usr/local`, in a
Homebrew cellar, or unpacked anywhere from a tarball.

Every recipe runs `make install` as one job (`-j1`), after building the
tools in parallel. The `install` rule's prerequisites, which build the
libraries, are not ordered before its copying step (`install-files`), so
under `make -j` it can copy before the libraries exist and install a tree
whose `lib/embcc` is mostly empty.

## Making a release

1. Set `EMBCC_VERSION` in `src/driver/version.h` to the new version, and
   merge that to `main`. The version is the tag without its `v`:
   `1.2.0` for `v1.2.0`, `1.2.0-rc1` for `v1.2.0-rc1`.
2. Optionally, try the build first: run the `release` workflow by hand
   (Actions, release, Run workflow) on `main` with no tag. It builds and
   tests every package and keeps them as the run's artifacts, and
   publishes nothing.
3. Tag the commit and push the tag:

   ```sh
   git tag v1.2.0
   git push origin v1.2.0
   ```

   `.github/workflows/release.yml` then builds EmbCC on four runners
   (Linux and macOS, x86-64 and arm64), installs each into a staging
   directory, tests it there, and uploads to the tag's GitHub Release:

   | File | Built on |
   |---|---|
   | `embcc-1.2.0-linux-x86_64.tar.gz` | Ubuntu 22.04, x86-64 |
   | `embcc-1.2.0-linux-aarch64.tar.gz` | Ubuntu 22.04, arm64 |
   | `embcc-1.2.0-macos-x86_64.tar.gz` | macOS 15, Intel |
   | `embcc-1.2.0-macos-aarch64.tar.gz` | macOS 15, Apple silicon |
   | `embcc_1.2.0_amd64.deb`, `embcc_1.2.0_arm64.deb` | the two Linux runners |
   | a `.sha256` file for each of the above | |
   | `embcc-version.txt` | the tag, which `install.sh` reads to find the latest release |

   The workflow creates the release when the tag has none, and replaces
   files of the same name when it has one. A tag that does not match
   `src/driver/version.h` stops the workflow before anything is built.
4. Update the Homebrew, RPM and Arch recipes, as described below.

`install.sh` installs the release GitHub marks as the latest one, which
is never a draft or a pre-release. Marking a release as a pre-release
keeps it from users who do not ask for it with `--version`.

The Linux programs are built on Ubuntu 22.04, so they run where its
glibc (2.35) or a newer one is installed, such as Ubuntu 22.04 and
Debian 12 and later; the `.deb`'s dependency on `libc6` names the exact
version they need. The macOS programs run on macOS 11 and newer. On a system the binaries do not run on, `install.sh` builds the
release from source.

## The installer

`install.sh` is served from the `main` branch:

```sh
curl -fsSL https://raw.githubusercontent.com/EmbLinkOs/EmbCC/main/install.sh | sh
```

It finds the latest release's tag in
`https://github.com/EmbLinkOs/EmbCC/releases/latest/download/embcc-version.txt`,
downloads the tarball for the machine and its `.sha256` file from
`.../releases/download/TAG/`, checks one against the other, runs the
unpacked `embcc --version`, and copies the tree into the prefix. The
comment at its top lists its options and environment variables.

`EMBCC_INSTALL_BASE_URL` replaces `https://github.com/EmbLinkOs/EmbCC/releases`
with a URL or a local directory in the same layout. That is how
`tests/golden/install-script.sh` tests the installer without a network,
against a tarball `tarball.sh` makes, and how the release workflow tests
each tarball before uploading it.

## Debian and Ubuntu

The `.deb` files are attached to each GitHub Release, and install with

```sh
sudo apt install ./embcc_1.2.0_amd64.deb
```

There is no apt repository. To build one by hand on Linux, from a staged
install:

```sh
make -j8 all embdbg
make -j1 install DESTDIR=/tmp/stage PREFIX=/usr/local
sh packaging/deb.sh /tmp/stage/usr/local 1.2.0 amd64 dist
```

A pre-release version is written with a `~` inside the package
(`1.2.0~rc1`), so that it sorts before `1.2.0`.

## Homebrew

The formula builds from the tag's source tarball, with macOS's own
`/usr/bin/make` (GNU Make 3.81, which builds EmbCC), and its test
compiles, links and runs a program for the Mac, and compiles objects for
a Cortex-M4 and a RISC-V core.

It is published in a tap, a GitHub repository named
`EmbLinkOs/homebrew-embcc` that holds `Formula/embcc.rb`. Users then
install with

```sh
brew install EmbLinkOs/embcc/embcc
```

To create the tap, once:

1. Create the public repository `EmbLinkOs/homebrew-embcc` on GitHub.
2. Copy `packaging/homebrew/embcc.rb` into it as `Formula/embcc.rb`, and
   push.

For each release, after the tag is pushed:

1. Run `packaging/set-version.sh v1.2.0`. It downloads GitHub's source
   tarball for the tag and writes its SHA-256 into the formula, the RPM
   spec and the PKGBUILD. Commit the result to this repository.
2. Copy `packaging/homebrew/embcc.rb` to the tap's `Formula/embcc.rb`.
3. Check it, and push the tap:

   ```sh
   brew tap EmbLinkOs/embcc
   brew install --build-from-source EmbLinkOs/embcc/embcc
   brew test EmbLinkOs/embcc/embcc
   brew audit --strict EmbLinkOs/embcc/embcc
   ```

The formula has no bottles, so every user builds from source, which
takes a minute or two. `brew install --HEAD EmbLinkOs/embcc/embcc` builds
the `main` branch.

## RPM

`rpm/embcc.spec` builds from the tag's source tarball with the
distribution's compiler flags, and installs into `/usr`:

```sh
sudo dnf install rpm-build rpmdevtools gcc make
rpmdev-setuptree
spectool -g -R packaging/rpm/embcc.spec
rpmbuild -ba packaging/rpm/embcc.spec
```

The packages are written under `~/rpmbuild/RPMS/` and
`~/rpmbuild/SRPMS/`. To publish them, build the source RPM in a Fedora
COPR project (`copr-cli build PROJECT ~/rpmbuild/SRPMS/embcc-*.src.rpm`),
which builds it for each Fedora and EPEL release the project enables.

Strip is turned off in the spec: the archives under `lib/embcc` are for
the targets EmbCC compiles for, which the build machine's `strip` cannot
read.

## Arch Linux

`arch/PKGBUILD` builds from the tag's source tarball with makepkg's
compiler flags, and installs into `/usr`:

```sh
cd packaging/arch
makepkg -si
```

To publish it in the AUR, as the package `embcc`:

```sh
git clone ssh://aur@aur.archlinux.org/embcc.git aur-embcc
cp packaging/arch/PKGBUILD aur-embcc/
cd aur-embcc
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO
git commit -m "embcc 1.2.0"
git push
```

## License

The repository has no LICENSE file yet. The Homebrew formula declares no
license, the RPM spec says `Unspecified` and the PKGBUILD says `unknown`.
A package without a license cannot enter Homebrew's core tap or a
distribution's own repositories. When the repository has one, put its
SPDX identifier in all three recipes.
