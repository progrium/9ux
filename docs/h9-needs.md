# What 9/UX needs from h9

Gaps found while building 9/UX on h9 (pinned in `scripts/pins.sh`). Each
entry says who found it, what happens, the workaround in this repo, and
what h9 would need. The h9 stream (H in [workplan.md](workplan.md))
takes them upstream.

## DMEXCL isn't enforced on the host-tree root

- **Found by:** S5 (uxmeta), at H9_REV b27cacd, native backend.
- **What happens:** a file created with `DMEXCL` shows the `l` mode bit,
  but a second `open` of it succeeds while the first is still open. On
  9front's own file servers the second open fails with "file in use".
- **Workaround:** uxmeta's single-writer lock also creates a `/srv`
  entry with `ORCLOSE`, which devsrv refuses to create twice and removes
  when the holder exits (`uxmeta/layer.c`, `metalock`).
- **h9 would need:** exclusive-use files in the host-tree file server
  (refuse a second open of a `DMEXCL` file while one is open in the
  machine), or a documented statement that it doesn't, so programs don't
  rely on it.
