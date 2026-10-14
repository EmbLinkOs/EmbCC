# RPM package of EmbCC, built from the tagged source tarball.
#
#   spectool -g -R packaging/rpm/embcc.spec     (or download Source0 into
#                                                ~/rpmbuild/SOURCES)
#   rpmbuild -ba packaging/rpm/embcc.spec
#
# packaging/README.md describes the rest, and packaging/set-version.sh sets
# the version for a new release.

# The release tag, without its 'v'. RPM's Version cannot hold a '-', and a
# '~' sorts before the release it precedes, so 1.1.0-rc1 is 1.1.0~rc1.
%global upstream_version 1.0.0-m2.complete

# The libraries under lib/embcc are for the targets EmbCC compiles for
# (Cortex-M, RISC-V, AVR, ...), not for this machine: strip cannot read
# them, and there is no debug information to extract from them.
%global debug_package %{nil}
%global __strip /bin/true

Name:           embcc
Version:        1.0.0~m2.complete
Release:        1%{?dist}
Summary:        C and C++ compiler and toolchain for embedded systems

# The repository has no LICENSE file yet; set this to its SPDX identifier
# when it has one.
License:        Unspecified
URL:            https://github.com/EmbLinkOs/EmbCC
Source0:        %{url}/archive/refs/tags/v%{upstream_version}.tar.gz#/EmbCC-%{upstream_version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make

%description
EmbCC compiles C and C++ for Arm Cortex-M and Cortex-A, RISC-V, AVR,
Xtensa, TriCore, RX, ColdFire, MIPS, PowerPC, SPARC and LoongArch, and for
x86-64 and AArch64 operating systems. One embcc binary targets every one
of them. The package also holds the assembler, the linker, the debugger,
the simulator, the firmware tools, and the headers and libraries for each
target.

%prep
%autosetup -n EmbCC-%{upstream_version}

%build
# The distribution's flags, with the C standard the sources are written
# to. The tools build in parallel; the libraries are built by `make
# install`, with the compiler this step makes.
%make_build CFLAGS="-std=c99 %{optflags}" all embdbg

%install
# One job: install's prerequisites are not ordered before its copying
# step, so with -j it can copy before the libraries exist.
make -j1 install CFLAGS="-std=c99 %{optflags}" DESTDIR=%{buildroot} PREFIX=%{_prefix}

%check
# The installed compiler finds its headers relative to itself.
%{buildroot}%{_bindir}/embcc --print-search-dirs | grep -q '^layout: installed'
printf '#include <stdio.h>\nint main(void) { puts("ok"); return 0; }\n' > check.c
%{buildroot}%{_bindir}/embcc --target=thumbv7em-none-eabi -c check.c -o check.o

%files
%{_bindir}/emb*
%{_prefix}/lib/embcc/
%{_datadir}/embcc/
%{_mandir}/man1/embcc.1*
