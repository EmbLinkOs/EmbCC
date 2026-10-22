# Security

## Reporting a vulnerability

Report it privately, through GitHub: on the repository's **Security**
tab, choose **Report a vulnerability**. Please do not open a public
issue for it. You will get an answer within a week, and a fix or a
plan before anything is made public.

## What counts

- A file EmbCC's tools read (a C or C++ source, an object, an archive,
  a linker script, an ELF image, an SVD file, a device pack) that makes
  one of them run code of the file's choosing, or write outside the
  output it was asked for.
- A network service in the toolchain (EmbSim's and `embdbg`'s GDB
  remote protocol, `embls`) that a peer can make do more than the
  protocol allows.
- A release or `install.sh` that installs something other than what
  the release's SHA-256 files name.
- Wrong code that removes a check the program relies on, such as a
  bounds check or a stack guard, in a way an attacker can reach.

Other wrong code is a bug, and the most urgent kind: report it as an
ordinary issue with the "Wrong code" form.

## Versions

Fixes go into `main` and the next release. While EmbCC is before 1.0,
earlier releases are not patched.
