# clipd: the typed clipboard

An optional 9P service, `/mnt/clip`, that holds several representations
of one copied thing (text, a file list, an image), and keeps the text
one in step with rio's snarf in both directions, so legacy programs
copy and paste exactly as before. Protocol: [clip.md](../docs/protocols/clip.md).

```rc
clipd [-D] [-n snarffile] [-s srvname] [-m mtpt]   # posts /srv/clip.$user, mounts /mnt/clip
echo -n hello >/mnt/clip/data/text                 # copy (also into snarf)
cat /mnt/clip/types                                # gen N, then one line per type
cat /mnt/clip/data/text                            # paste
```

Start it inside the rio whose snarf it should follow (from `riostart`,
say). libux uses it when `/mnt/clip` is mounted, and plain `/dev/snarf`
otherwise.

`-n FILE` uses a plain file instead of `/dev/snarf`, which is how the
tests run it, and also makes it its own fake for clients' tests.

## Building and testing

```sh
scripts/h9.sh --rc 'cd clipd && mk'        # $O.out
scripts/h9.sh --rc 'cd clipd && mk test'   # test/clip.rc
```

[test/clip.rc](test/clip.rc) checks copying one type and a batch,
snarf following text copies, a legacy copy into snarf becoming the
clipboard (on open, and as an event from the poller), abort, clear,
and bad type names.
