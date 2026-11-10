# libux: the 9/UX Toolbox

A native C library on `libdraw` and `libthread` that gives 9/UX
applications their controls, layout, dialogs, menus, text input, file
chooser, resource lookup, and clipboard integration.

Its central rule: **controls are application state, not pixels.** Every
libux application serves its typed control tree over 9P, so a shell
script and the on-screen UI see the same objects:

```text
/mnt/ux/apps/sketch-42/windows/17/controls/save/{type,label,state,ctl,events}
```

First controls (Phase 2): `App`, `Window`, `Menu`, `Dialog`, `Button`,
`TextField`. Later: `Checkbox`, `List`, `ScrollView`, `IconView`,
`FileChooser`.

Design: [§5 Toolbox](../docs/design.md#5-toolbox-libux-native-9p-visible-ui).
Plan: [Phase 2](../docs/plan.md#phase-2-9p-toolbox).
