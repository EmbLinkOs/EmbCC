#!/bin/sh
# Reproducible builds and the SBOM (src/driver/sbom.c, util.c's prefix maps):
#
#   - the same sources built in two directories, with -ffile-prefix-map
#     naming each /build and SOURCE_DATE_EPOCH set, give the same objects,
#     image and bill byte for byte (with -g, so the DWARF's paths count);
#   - the bill is SPDX 2.3: the image's SHA-256 is its own, each source and
#     header is there with its hash and its SPDX-License-Identifier, the
#     runtime archive is there, and every file is GENERATED_FROM-related;
#   - --sbom-from writes it from the image again, and CycloneDX 1.5 too.
set -u
echo "TEST-MARKER sbom-reproducible"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/sbom-reproducible
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
command -v python3 > /dev/null 2>&1 || { echo "skipped: no python3"; exit 0; }
T=thumbv7m-none-eabi
RT=$PWD/build/libc/$T/librt.a
[ -f "$RT" ] || { echo "skipped: no $RT"; exit 0; }
printf 'MEMORY { FLASH (rx) : ORIGIN = 0, LENGTH = 256K  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }\nSECTIONS { .text : { *(.text*) *(.rodata*) } > FLASH .data : { *(.data*) } > RAM AT > FLASH .bss : { *(.bss*) *(COMMON) } > RAM }\n' > "$out/m.ld"
for d in one two/deeper; do
    mkdir -p "$out/$d/src"
    printf '/* SPDX-License-Identifier: MIT */\n#include "cfg.h"\nint add(int a, int b) { return a + b + CFG; }\nconst char *where(void) { return __FILE__; }\n' > "$out/$d/src/a.c"
    printf '// SPDX-License-Identifier: Apache-2.0\n#define CFG 3\n' > "$out/$d/src/cfg.h"
    printf 'int add(int, int);\nint main(void) { return add(1, 2); }\n' > "$out/$d/src/m.c"
    ( cd "$out/$d" && SOURCE_DATE_EPOCH=1700000000 "$EMBCC" --target=$T -O1 -g \
        --sbom=fw.spdx.json -ffile-prefix-map="$out/$d"=/build \
        -nostdlib -T "$out/m.ld" -e main "$out/$d/src/a.c" "$out/$d/src/m.c" \
        "$RT" -o fw.elf ) || fail "build in $d"
done
cmp -s "$out/one/fw.elf" "$out/two/deeper/fw.elf" || fail "the images differ"
cmp -s "$out/one/fw.spdx.json" "$out/two/deeper/fw.spdx.json" ||
    fail "the bills differ: $(diff "$out/one/fw.spdx.json" "$out/two/deeper/fw.spdx.json" | head -5)"
! grep -q "$out" "$out/one/fw.elf" || fail "the image still names the build directory"
python3 - "$out/one" <<'EOF2' || fail "the SPDX bill"
import hashlib, json, sys
d = sys.argv[1]
b = json.load(open(d + '/fw.spdx.json'))
assert b['spdxVersion'] == 'SPDX-2.3', b['spdxVersion']
assert b['creationInfo']['created'] == '2023-11-14T22:13:20Z', b['creationInfo']
img = [p for p in b['packages'] if p['SPDXID'] == 'SPDXRef-Package-Image'][0]
assert img['checksums'][0]['checksumValue'] == hashlib.sha256(open(d + '/fw.elf', 'rb').read()).hexdigest()
assert img['primaryPackagePurpose'] == 'FIRMWARE'
files = {f['fileName']: f for f in b['files']}
a = files['/build/src/a.c']
assert a['licenseInfoInFiles'] == ['MIT'], a
assert a['checksums'][0]['checksumValue'] == hashlib.sha256(open(d + '/src/a.c', 'rb').read()).hexdigest()
assert files['/build/src/cfg.h']['licenseInfoInFiles'] == ['Apache-2.0']
assert any(f.endswith('librt.a') and files[f]['fileTypes'] == ['ARCHIVE'] for f in files)
assert not any('embcc-tmp' in f for f in files), files.keys()
rel = [r for r in b['relationships'] if r['relationshipType'] == 'GENERATED_FROM']
assert len(rel) == len(b['files'])
EOF2
"$EMBCC" --sbom-from "$out/one/fw.elf" --sbom-format=cyclonedx -o "$out/fw.cdx.json" ||
    fail "--sbom-from"
python3 - "$out/fw.cdx.json" <<'EOF2' || fail "the CycloneDX bill"
import json, sys
b = json.load(open(sys.argv[1]))
assert b['bomFormat'] == 'CycloneDX' and b['specVersion'] == '1.5'
assert b['metadata']['component']['type'] == 'firmware'
names = [c['name'] for c in b['components']]
assert '/build/src/a.c' in names and '/build/src/cfg.h' in names, names
EOF2
echo "the same bytes from two directories, and an SPDX and a CycloneDX bill"
