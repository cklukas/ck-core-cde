# CK Core CDE Worklog

This repository mirrors ongoing CK-Core-specific additions on top of the upstream CDE sources. Recent commits added polish to icon handling, interaction affordances, and user documentation:

- **Extra-large icon view and IconicPath UX** – File Manager now has a new option to requests extra-large pixmaps, auto-scales icons when needed, exposes the extra-large option across prefs/resources, adds a Run... action, and lets IconicPath components summon the shared popup menu via Button3 so breadcrumbs behave like real path entries.
- **Storage size popup** – the UI now surfaces detailed storage usage info in a dedicated popup.
- **Run command cleanup** – A new shell action `Run` can be used to directly start (GUI) programs. The prior "Run" action has been renamed to "Run..." and for that a checkbox "Run in Terminal" has been added, which can be used to run commands in Terminal (default), or to not run a program in Terminal.
- **Desktop/window ergonomics** – grid-point movement and resize of windows with Shift, clicking the desktop will defocus the active window, and double-clicking the title bar to maximize/restore a window.

## Building with CK Core Motif

Build and install CK Core Motif ABI 9 into an isolated prefix first, including
its Xm, Mrm and Uil libraries, headers and UIL compiler. Use the same prefix
for the whole CDE build. For example, on Linux with Xft enabled:

```sh
export PATH="$MOTIF_SDK/bin:$PATH"
export CPPFLAGS="-I$MOTIF_SDK/include $(pkg-config --cflags xft)"
export LDFLAGS="-L$MOTIF_SDK/lib -Wl,-rpath,$MOTIF_SDK/lib"
cd cde
./autogen.sh
./configure
make -j4
make -j4 check
```

Set `MOTIF_SDK` to the absolute installation prefix. CDE uses Motif private
widget records, so rebuild CDE and its libraries when changing toolkit ABI.

Desktop KornShell uses a checksum-pinned upstream source archive. The build
guide covers offline builds, patch updates and the widget integration test.
[Desktop KornShell dependency](docs/build.md#desktop-kornshell-dependency)

## Original README
Refer to the upstream README for the base project: [cde/README.md](cde/README.md).
