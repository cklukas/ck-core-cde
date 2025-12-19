# CK Core CDE Worklog

This repository mirrors ongoing CK-specific additions on top of the upstream CDE sources. Recent commits emphasize polish to icon handling, interaction affordances, and user documentation:

- **License and documentation** – added a pointer to the original `cde/README.md` license text for anyone who checks out this forked hierarchy.
- **Extra-large icon view and IconicPath UX** – File Manager now requests extra-large pixmaps, auto-scales icons when needed, exposes the extra-large option across prefs/resources, adds a Run... action, and lets IconicPath components summon the shared popup menu via Button3 so breadcrumbs behave like real path entries.
- **Storage size popup** – the UI now surfaces detailed storage usage info in a dedicated popup.
- **Run command cleanup** – Shell actions such as `Run` now route through a straightforward command action/backing script instead of the old mapping, making terminal launches simpler to understand.
- **Desktop/window ergonomics** – grid-point movement with Shift, defending against defocus when clicking the desktop, and double-clicking the title bar to maximize/restore all keep window management feeling responsive.

## Original README
Refer to the upstream README for the base project: [cde/README.md](cde/README.md).
