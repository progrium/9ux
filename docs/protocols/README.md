# 9/UX file protocols

Every interface between 9/UX components is a set of files served over
9P. This directory is where those files are written down. A component
builds against the document here, never against another component's
code, and every protocol here ships with a fake under
[`test/fakes/`](../../test/fakes/) so its clients can be tested before
the real server exists.

| Document | Server | Clients | Mounted at |
|---|---|---|---|
| [rio.md](rio.md) | rio (stock files) | every rio program | `/dev`, `/mnt/wsys` |
| [devux.md](devux.md) | rio (9/UX additions) | libux, uxd, Finder, desktop clients | `/dev/ux` |
| [ux.md](ux.md) | each libux app, aggregated by uxd | uxd, scripts, tests, other apps | `/mnt/ux` |
| [meta.md](meta.md) | uxmeta | Finder, libux, launcher | `/mnt/meta` |
| [res.md](res.md) | resfs | libux, Finder | `/mnt/res` |

`rio.md`, `meta.md` and `res.md` are written by the streams that own those
servers (see [workplan.md](../workplan.md#ownership)). This file holds
the conventions all of them share. A protocol document may tighten a
rule here, but it says so where it does.

## Words used

- **must**, **must not**, **should**, **may** mean what RFC 2119 says.
- A **client** opens files; a **server** serves them. An app is the
  server of its own control tree and a client of `/dev/ux`.
- A **stream file** is a file whose reads deliver messages over time
  (every `events` file). Every other file is a **state file** (a
  snapshot that can be read at any time) or a **ctl file** (commands
  written to it).

## Text

- Everything is UTF-8. No byte-order marks, no NUL bytes. Binary data
  (images, resource bytes) lives in its own files and is never mixed with
  text.
- A **line** is UTF-8 text ending in a single `\n`. Servers write `\n`
  after every line, including the last. Clients must accept a missing
  final newline in what they write.
- State files that hold one value (`label`, `title`, `value` of a
  one-line field) hold the value followed by one `\n`. The newline is
  not part of the value. A value that can itself contain newlines (a
  multi-line text field) is served raw, with no added newline, and says
  so in its protocol.
- State files are read from offset 0. A server builds the snapshot when
  the file is opened, so reading the whole file in several reads gives
  one consistent snapshot. To see a newer value, open the file again.

## Messages: tokens and attributes

ctl commands, event messages, and multi-field state lines are all the
same shape: a line split into **tokens** with the rules of
`tokenize(2)`, and written with the rules of `quote(2)`:

- Tokens are separated by spaces or tabs.
- A token containing a space, tab, quote or newline, or an empty token,
  is wrapped in single quotes; a single quote inside it is doubled.
  `'it''s here'` is the token `it's here`. The C side uses `quotefmtinstall`
  and `%q` to write and `tokenize` to read; in rc, `'...'` quoting is the
  same rule.
- A newline can't appear inside a token. Values that need newlines are
  written to their own file, not passed in a message.

The first token is a **verb** (in ctl commands) or an **event name**
(in events). Positional arguments come next, then **attributes**: tokens
of the form `name=value`, split at the first `=`. The whole token is
quoted, not just the value: `'label=Save As…'`.

- Writers quote a token only when `tokenize` needs it (space, tab,
  quote, newline, or an empty token). `%q` also quotes tokens holding
  `=`, which is harmless: readers must accept any valid quoting.
- In rc, `=` in an argument is a syntax error, so attribute tokens are
  quoted there: `echo menu file 'title=File'`, and a value with spaces
  is quoted twice, `'''label=Read Me'''`, so the quotes reach the file.
- Attribute names are lowercase ASCII words (`[a-z][a-z0-9]*`).
- Attributes may come in any order. A name appears at most once unless
  a protocol says it repeats (such as `win=` in a registration).
- Readers must ignore attributes they don't know. Writers must not send
  attributes the protocol doesn't define, except names beginning with
  `x` followed by a dot (`x.finder.foo=1`), which are free for
  experiments and are ignored by everyone else.

### Value forms

| Form | Written as | Example |
|---|---|---|
| Boolean | `on` or `off` | `enabled=off` |
| Integer | decimal, optional leading `-` | `x=-12` |
| Point | `x,y` | `pt=40,12` |
| Rectangle | `minx,miny,maxx,maxy` (Plan 9 `Rectangle`, max exclusive) | `rect=0,0,640,480` |
| List | values joined with `,` (no spaces) | `mods=shift` |
| Flags | the flags that are set, each a word; joined by `,` in an attribute, by spaces on a state file's line; empty means none set | `state=enabled,focused` |
| Text | any UTF-8 without newline, quoted as needed | `'title=Read Me'` |

Coordinates are integers in pixels. Rectangles inside a window are
relative to that window's `screen->r.min` (window-local). Rectangles that
describe windows themselves are in screen coordinates, as in
`/dev/wctl`.

## Identifiers

| ID | Form | Who assigns it | Lifetime |
|---|---|---|---|
| `winid` | decimal integer | rio (`/dev/winid`) | the window's lifetime; never reused while rio runs |
| app ID | `name-N`, `name` from `[a-z][a-z0-9-]*`, `N` decimal | uxd at registration | until the app exits or uxd restarts (then the app re-registers and may get a new N) |
| control ID | `[a-z0-9][a-z0-9._-]*`, at most 64 bytes | the app (from its code or resources) | stable across runs of the same app version |
| action ID | same form as a control ID | the app | stable across runs; scoped to the app |
| menu ID | same form as a control ID | the app | stable across runs; scoped to the app |
| grant token | 32 lowercase hex digits | uxd | until revoked |

- `/dev/winid` reads as the number padded with spaces to 11 bytes (stock
  rio). On the wire, 9/UX always writes plain decimal with no padding.
  `0` is never a window, and is used to mean "no window".
- IDs are case-sensitive and never contain `/`, spaces, quotes or `=`,
  so they are always safe as path elements and as unquoted tokens.
- Control, action and menu IDs come from the app, not from labels or
  positions, so they don't change when the UI is translated or
  rearranged.

## ctl files

- A write holds one or more commands, one per line. The server runs
  them in order and stops at the first error, which the write returns.
  Commands before the failing one have taken effect; protocols that
  need all-or-nothing batches give a `begin`/`commit` pair.
- A write must not exceed the file's iounit (8192 bytes unless the
  server says otherwise). A command can't be split across writes.
- A successful write returns the full count written.
- Reading a ctl file is allowed and returns help for that file: one
  line per verb it accepts, `verb class`, where `class` is the right
  needed to use it (see [ux.md](ux.md#rights)) or `-` on servers with
  no rights at all, such as rio's (uxd refuses `-` verbs in app trees). This is how uxd learns which rights a control's verbs need, and
  it lets a person discover commands with `cat`.

## Errors

A failed write, open, or read returns an error string through `errstr`.
Error strings start with one of the stable phrases below, optionally
followed by `: ` and detail for people. Programs match on the phrase
only.

| Phrase | Meaning |
|---|---|
| `unknown command` | The verb isn't one this file accepts |
| `bad syntax` | Wrong number of arguments, a malformed attribute, or bad quoting |
| `bad value` | Well-formed, but a value is out of range or not allowed |
| `not found` | A named object (window, control, menu, action, app) doesn't exist |
| `in use` | Someone else holds it (a claimed desktop, a window owned by another app) |
| `disabled` | The target exists but is disabled (a disabled button, menu item) |
| `permission denied` | The client's rights don't allow it |
| `gone` | The object was removed while the file was open; reopen to see the new state |
| `too large` | A write over iounit, or a value over a protocol's limit |
| `short read` | The read count is too small to hold one whole message |
| `version mismatch` | A client asked for a major version the server doesn't speak |
| `busy` | A transient conflict; the client may retry |

Example: `bad value: rect is empty`.

## Stream files (events)

All `events` files in 9/UX behave the same way.

- **Per-open subscriptions.** Each open of an `events` file is an
  independent subscription with its own queue and cursor. Two readers
  see the same messages. A read on one never consumes another's.
- **Blocking reads.** A read returns at once if messages are queued,
  and otherwise blocks until one is. Servers must answer `Tflush` for a
  blocked read promptly; that is how a client stops waiting. Only one
  read may be outstanding on a subscription: a second read on the same
  fid while one is blocked fails with `busy`. (Open the file again for a
  second reader.)
- **Whole messages.** A read returns one or more complete lines and
  never splits a line. If the count is too small for the next message,
  the read fails with `short read`. Read with at least 8192 bytes. (So
  rc's `read`, which reads a byte at a time, can't be used; `cat`, or
  `dd -bs 8192 -count 1` for whatever is queued, can.)
- **Offsets are ignored**; the file is a stream.
- **Message shape:** `seq event attr...`, where `seq` is a decimal
  sequence number. Sequence numbers come from one counter per server,
  shared across all of that server's event files, and increase by at
  least one per message. The same event has the same `seq` in every
  file and for every reader, so readers can line up events from
  different files of one server. Numbers start from 1 when the server
  starts. `resync` takes a fresh number when it is queued; `gone` takes
  one fresh number shared by every subscription it ends; `lost` takes the
  number of the message that didn't fit, so it never uses up a number
  other readers see for something else.
- **First message.** The first message on every new subscription is
  `seq resync`, queued at open. Its `seq` marks the start of the
  subscription.
- **Snapshots then events.** To follow an object: open its `events`,
  read the `resync`, then read the state files, then apply events.
  Events carry the new state of what changed (`value 'value=hello'`), not
  a delta, so an event already reflected in the snapshot is harmless to
  apply again.
- **Bounded queues.** Each subscription's queue has a limit (256
  messages unless a protocol says otherwise). When a message would
  overflow it, the server drops everything queued and queues one
  `seq lost n=N`, where `N` is how many messages were dropped,
  including the one that didn't fit. It keeps queueing new messages
  after that. On `lost`, a client must read the snapshots again. A slow
  reader never slows down the server or other readers.
- **End.** When the object an `events` file follows goes away, the
  server queues `seq gone` and then returns end of file (a zero-length
  read) to every later read. A server that exits takes its
  subscriptions with it; clients see an error or end of file and should
  reconnect if they want to keep following.
- **Unknown events** must be ignored by readers.

## Versions and features

Each server's root has a `version` file:

```text
devux 1.0
feature focus
feature desktop
feature menubar
```

The first line is the protocol name, a space, and `major.minor`. Then
one `feature name` line per optional feature the server supports, in
any order.

- A minor version adds files, verbs, attributes, events or features, and
  never changes what existing ones mean. Clients must work with any
  minor version of their major version.
- A major version change breaks compatibility. A client that doesn't
  know the server's major version must stop with `version mismatch`
  rather than guess.
- A client checks `feature` lines instead of version numbers for
  optional behavior.

## Lifetimes and ownership

- A registration made by writing to a ctl file belongs to that open
  file (fid). When the fid is clunked, because the client closed it or
  exited, the server removes what it registered. Clients keep the fid
  open for as long as their registration should live.
- Servers must not block their own main loop on a client. A client
  that stops reading, or is slow, affects only its own subscriptions.

## Testing

Each protocol has a fake in `test/fakes/` that implements it closely
enough to test clients against, plus a script that runs the fake's own
tests. See [test/fakes/README.md](../../test/fakes/README.md).
