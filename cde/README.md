CDE - The Common Destop Environment
===

In 2012, CDE was opensourced under the terms of the LGPL V2 license by
the Open Group.

You may reuse and redistribute this code under the terms of this
license. See the COPYING file for details.

# Downloading

Downloading this release:

CDE may be downloaded in source form from the Common Desktop
Environment website:

http://sourceforge.net/projects/cdesktopenv/

Or via git:

git clone git://git.code.sf.net/p/cdesktopenv/code CDE

The git repository will always be more up to date than the
downloadable tarballs we make available, so if you have problems,
please try the latest version from git master.

Note also that the master branch may be unstable, so your milage may
vary.

# Compiling

Complete build and installation instructions can be found on the CDE
wiki:

http://sourceforge.net/p/cdesktopenv/wiki/Home/

Please go there and read the appropriate section(s) for your OS (Linux
or FreeBSD/OpenBSD/NetBSD currently) prior to attmpting to build it.

There are a variety of dependencies that must be met, as well as
specific set up steps required to build, especially relating to
localization and locales.

Do not expect to just type 'make' and have it actually work without
meeting the prerequisites and following the correct steps as spelled
out on the wiki.

There are also a lot of other documents and information there that you
might find useful.

Assuming you've met all of the requirements regarding packages needed
for the build, you can follow the standard autoconf method:

```
$ ./autogen.sh
$ ./configure
$ make
$ sudo make install
```

NOTE: BSD users must currently install and use gmake to compile, as
well as specify the location of the TCL libraries and headers.  So
the instructions for them would looke like:

```
$ ./autogen.sh
$ ./configure --with-tcl=/usr/local/lib/tcl8.6 MAKE="gmake"
$ gmake
$ sudo gmake install
```

Of course change to location of your TCL directory as needed for your
system.

# Support

## Mailing list

https://lists.sourceforge.net/lists/listinfo/cdesktopenv-devel

## IRC

There is a CDE IRC channel on irc.libera.chat, channel #cde

## Patches welcome

Please see

https://sourceforge.net/p/cdesktopenv/wiki/Contributing%20to%20CDE/

for information on how to contribute.

## Desktop resource overrides

CDE builds its runtime resource database out of several layers (`~/.dt/sessions/*/dt.resources`, the system `sys.resources`, `dtwmrc`, etc.), so simply editing `~/.Xresources` usually isn’t enough: the session manager regenerates its own `dt.resources` every login and overwrites whatever was merged earlier. To make sure your per-user fonts stick permanently we now:

  * teach `dtsession_res` to load `~/.Xresources` whenever the session manager runs (`-xresources` is accepted alongside `-xdefaults`), and
  * merge `~/.Xresources` as the last step in `SmRestore` so the root window’s `RESOURCE_MANAGER` ends up with whatever is in your home file.

You can keep defining `*FontList`, `*systemFont[1-7]`, `*buttonFontList`, etc. in `~/.Xresources` (and keep a `~/.dt/<LANG>/dtstyle.rc` if you want Style Manager overrides) and those values will now survive login. The Style Manager font dialog now builds an install-wide font catalog: the left pane lists each family only once, the next panes show variants (weight/slant), charsets, and sizes for that family, and the size list presents bitmap pixel sizes or scalable point sizes depending on the selected descriptor. The dialog keeps the preview and descriptor in sync with that selection, and the preview loads the selected XLFD through the appropriate font list path so you see the actual rendering size before applying the change. The font dialog can also apply the chosen font to `dtwm` (titlebar/feedback fonts, with optional icon/global/FrontPanel font resources) and optionally restart `dtwm` so window title text updates immediately without maintaining separate `Dtwm*...*fontList` entries in `~/.Xresources`. The `dtfile` icon grid now queries each `DtIconGadget`’s cached string extents instead of assuming fixed-width fonts, so file labels automatically leave enough room even for proportional scalable fonts. Likewise, `dtterm` now measures the actual cell width of whatever font set or font the terminal is using rather than relying on `max_bounds.width`, so columns stay aligned even when a scalable font is selected. If you do need to pin a specific `dt.resources` copy, place your overrides in `~/.dt/sessions/current/dt.resources` before launching CDE.
The `dtwm` FrontPanel workspace switcher now recomputes switch button widths based on the active font list (so long workspace names remain readable with scalable fonts), and its rename edit field stays compact for short labels.
The FrontPanel date icon overlay now intentionally uses a fixed font so it remains legible even if you select large/scalable panel fonts.
DtHelp dialogs now pick up the Style Manager’s text font via `DtHelpDialog*homeTopic.fontList` / `DtHelpQuickDialog*closeButton.fontList`, and the display area as well as the “Volume:” path labels inherit the same `DtHelpDialog*DisplayArea.userFont`, `DtHelpDialog*DisplayArea.fontList`, `DtHelpDialog*volumeLabel.fontList`, and `DtHelpDialog*pathLabel.fontList`, with `DtHelpDialog*overrideFontList` set so the help viewer discards its `-dt-interface user…` fallbacks and renders using the scalable font you chose.  The help browser now also measures the X display’s DPI and automatically scales any embedded images at render time when that DPI exceeds ~120 so diagrams and icons stay legible on high-resolution screens.
