# resfs: resource file server

A read-only 9P server that parses classic Macintosh resource files
(`.rsrc`, AppleSingle and AppleDouble) and serves decoded resources:

```text
/mnt/res/sketch/icon/128/image        # best 32x32 icon, image(6)
/mnt/res/sketch/MENU/128/{raw,text}
/mnt/res/sketch/STR#/128/{raw,text}
/mnt/res/sketch/CURS/128/{raw,image,cursor}
```

The tree and formats: [docs/protocols/res.md](../docs/protocols/res.md).

Priority types: `ICN#`, `ics#`, `icl4`, `icl8`, `ics4`, `ics8`, `CURS`,
`STR `, `STR#`, `MENU`, `DLOG`, `DITL`, `ALRT`; `BNDL`/`FREF` only as
import hints. Resource files are untrusted input: every length, offset,
and dimension is checked before allocation, and the decoders are fuzzed.

The parser is plain C with no Plan 9 dependencies beyond libc, so it can
be fuzzed on the host as well as tested inside h9.

| File | What |
|---|---|
| `res.c` | resource map parser, lookup, index |
| `icon.c`, `text.c` | decoders to image(6), `/dev/cursor` form, and text |
| `macroman.c`, `rbuf.c` | MacRoman to UTF-8; output buffers, escaping and quoting |
| `resfs.c` | the 9P server (9front only) |
| `rescomp.c` | compiles resource source text to `.rsrc`, and back with `-d` |
| `token.c`, `encode.c`, `image.c`, `reswrite.c` | reading text forms and image(6), encoding resources, writing resource files |
| `rescat.c` | prints what resfs would serve, for tests and inspection |
| `test/` | `mkfixtures.py` makes fixtures and goldens from scratch; `cases` lists them |
| `fuzz/` | libFuzzer target, and a mutation driver for hosts without libFuzzer |

```sh
make test          # host: build rescat with ASan/UBSan, run the golden cases
make fuzz          # host, clang with libFuzzer: FUZZTIME=60 by default
make fuzz-driver   # host, any compiler: FUZZRUNS random mutations
h9 --rc 'cd resfs && mk test'   # 9front: golden cases, then the 9P server
```

No Apple resource data is in the repository: every fixture is drawn by
`test/mkfixtures.py`.

Design: [§7 Resources](../docs/design.md#7-resources-and-application-packaging).
Plan: [Phase 4](../docs/plan.md#phase-4-resources-and-full-interaction).
