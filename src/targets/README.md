# src/targets

The target database: what EmbCC knows about each target as data, one file
per family (`x86.def`, `arm.def`, `riscv.def`, ...). Each file holds the
family's data model (`DATA_MODEL`: type sizes, signedness, alignment cap,
byte order) and every triple `--target` accepts for it (`TRIPLE`: the
architecture, OS, object format, canonical or alias, sub-architecture,
hard-float or big-endian). [`targets.def`](targets.def) says how a row
reads and lists the families.

`src/arch/target.c` reads the rows into its tables; the backends keep
only the code that is genuinely per-architecture. A new target is a
family file here, its line in `targets.def`, and its backend in
`src/arch/<arch>/` ([Adding a target](../../docs/internals/backends.md#adding-a-target)).

The database grows step by step: the predefined macros, the CPU and
feature lists (`-mcpu`, `-march`) and the boards are next
([the redesign plan](../../docs/internals/redesign.md)).
