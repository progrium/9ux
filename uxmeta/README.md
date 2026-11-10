# uxmeta: metadata service

Owns 9/UX's persistent metadata: file associations, application
declarations, the volume registry, icon choices, and Finder layout. The
data lives in human-editable ndb files; `uxmeta` is the single serialized
writer, replaces files atomically, publishes a generation number, and
generates or complements plumbing rules so the plumber stays the dispatch
path.

```text
ext=.txt open=edit icon=text
volume=home path=/usr/glenda icon=disk desktop=yes
folder=/usr/glenda/doc view=icons sort=name x=100 y=90 width=550 height=400
```

Precedence: system defaults, then installed apps, then user preferences,
then workspace overrides.

Design: [§6 Metadata](../docs/design.md#6-metadata-ndb--uxmeta).
Plan: [Phase 3](../docs/plan.md#phase-3-finder-mvp).
