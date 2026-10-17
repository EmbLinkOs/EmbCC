# Homebrew formula for EmbCC, built from the tagged source tarball.
#
# It is published in the tap EmbLinkOs/homebrew-embcc, as Formula/embcc.rb;
# packaging/README.md says how. packaging/set-version.sh sets the url and
# sha256 lines for a new release.
class Embcc < Formula
  desc "C and C++ compiler and toolchain for embedded systems"
  homepage "https://github.com/EmbLinkOs/EmbCC"
  license "Apache-2.0"
  url "https://github.com/EmbLinkOs/EmbCC/archive/refs/tags/v0.0.1-beta.tar.gz"
  sha256 "868b5349223ead6fe8d590c43716758c049dea10b6992018fba5102010b990fd"
  head "https://github.com/EmbLinkOs/EmbCC.git", branch: "main"

  def install
    # The tools first, in parallel. The install rule then builds the
    # libraries for every target with the new compiler and copies
    # everything into the prefix. It runs as one job, because its
    # prerequisites are not ordered before its copying step.
    #
    # macOS's /usr/bin/make (GNU Make 3.81) builds EmbCC, so the formula
    # needs no newer make.
    system "make", "all", "embdbg"
    ENV.deparallelize
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    (testpath/"hello.c").write <<~C
      #include <stdio.h>
      #include <string.h>

      int main(void)
      {
          printf("hello from EmbCC %d\\n", (int)strlen("embedded"));
          return 0;
      }
    C

    # The installed compiler finds its own headers and libraries.
    assert_match "layout: installed", shell_output("#{bin}/embcc --print-search-dirs")

    # A program for this machine, linked and run.
    if OS.mac?
      arch = Hardware::CPU.arm? ? "aarch64" : "x86_64"
      system bin/"embcc", "--target=#{arch}-apple-darwin", "hello.c", "-o", "hello"
      assert_equal "hello from EmbCC 8\n", shell_output("./hello")
    elsif Hardware::CPU.intel?
      system bin/"embcc", "--target=x86_64-linux-gnu", "hello.c", "-o", "hello"
      chmod 0755, "hello"
      assert_equal "hello from EmbCC 8\n", shell_output("./hello")
    else
      # embld does not link AArch64 Linux programs; compile only.
      system bin/"embcc", "--target=aarch64-linux-gnu", "-c", "hello.c", "-o", "hello.o"
    end

    # Firmware objects for a Cortex-M4 and a RISC-V core, from the same
    # binary: ELF files whose e_machine is ARM (40) and RISC-V (243).
    { "thumbv7em-none-eabi" => 40, "riscv32-unknown-elf" => 243 }.each do |target, machine|
      system bin/"embcc", "--target=#{target}", "--opt=size", "-c", "hello.c", "-o", "#{target}.o"
      elf = File.binread("#{target}.o", 20)
      assert_equal "\x7FELF".b, elf[0, 4]
      assert_equal machine, elf[18, 2].unpack1("v")
    end
  end
end
