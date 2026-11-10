# resfs: resource file server

A read-only 9P server that parses classic Macintosh resource files
(`.rsrc`, later AppleSingle/AppleDouble) and serves decoded resources:

```text
/mnt/res/sketch/icon/128/image
/mnt/res/sketch/MENU/128/{raw,text}
/mnt/res/sketch/STR#/128/raw
```

Priority types: `ICN#`, `ics#`, `icl4`, `icl8`, `ics4`, `ics8`, `CURS`,
`STR `, `STR#`, `MENU`, `DLOG`, `DITL`, `ALRT`; `BNDL`/`FREF` only as
import hints. Resource files are untrusted input: every length, offset,
and dimension is checked before allocation, and the decoders are fuzzed.

The parser is plain C with no Plan 9 dependencies beyond libc, so it can
be fuzzed on the host as well as tested inside h9.

Design: [§7 Resources](../docs/design.md#7-resources-and-application-packaging).
Plan: [Phase 4](../docs/plan.md#phase-4-resources-and-full-interaction).
