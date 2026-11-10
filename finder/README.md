# Finder

The 9/UX desktop: volume and alias icons on rio's desktop layer, spatial
folder windows (icon and list views), select, open, rename, duplicate,
Get Info, Trash, and remembered geometry and layout. It is an ordinary
libux application with its own published UI tree.

Finder is a view of a Plan 9 namespace, not a global filesystem. Volumes
are only what the volume registry declares; operations follow what each
file server actually supports and report honestly when it doesn't; and
documents open through the plumber.

Design: [§4 Finder](../docs/design.md#4-finder).
Plan: [Phase 3](../docs/plan.md#phase-3-finder-mvp).
