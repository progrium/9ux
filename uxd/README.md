# uxd: UI registry and event broker

Discovers running 9/UX applications and presents them as one tree at
`/mnt/ux/apps/...`. Applications stay authoritative for their own control
state; `uxd` maps rio window IDs (`/dev/winid`) to logical applications,
aggregates their trees, routes events, and enforces the control policy
(introspection is separate from mutation, default deny for writes).

It is a broker, not a monolithic desktop daemon: rio's extensions and each
application's own 9P server keep working if `uxd` restarts.

Design: [§5](../docs/design.md#5-toolbox-libux-native-9p-visible-ui),
[§8](../docs/design.md#8-services-and-interfaces),
[§9](../docs/design.md#9-security-and-reliability).
Plan: [Phase 2](../docs/plan.md#phase-2-9p-toolbox).
