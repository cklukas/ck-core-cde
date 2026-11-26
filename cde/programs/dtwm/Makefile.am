MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtwm dtfplist dtwm-media-ctl cde-color-sync

if BUILD_DTWM_WAYLAND
bin_PROGRAMS += dtwm-wayland dtwm-panel
endif

EXTRA_DIST = tests/check-property-readers.py tests/property-client.c \
             tests/check-desktop-requests.py tests/check-session-word-width.py tests/check-session-word-width.c \
             tests/check-config-temp-file.py tests/check-config-temp-file.c \
             tests/check-session-counts.py tests/check-session-counts.c \
             tests/check-quit-signal.py tests/check-quit-signal.c \
             tests/check-dtwm-hardening.py tests/check-dtwm-hardening.c \
             tests/check-dtwm-wayland.py tests/check-dtwm-wayland-workspaces.py \
             tests/check-dtwm-wayland-icons.py tests/check-dtwm-wayland-wp05.py \
             tests/check-dtwm-wayland-wp06.py tests/check-dtwm-wayland-wp09.py \
             tests/check-dtwm-wayland-wp10.py tests/check-dtwm-wayland-wp11.py \
             tests/check-dtwm-wayland-wp12.py tests/check-dtwm-wayland-wp13.py \
             tests/check-dtwm-wayland-wp14.py tests/check-dtwm-wayland-wp15.py \
             tests/check-dtwm-wayland-wp16.py tests/check-dtwm-wayland-wp17.py \
             tests/check-dtwm-wayland-wp18.py tests/check-dtwm-wayland-wp19.py \
             tests/check-dtwm-wayland-wp20.py \
             tests/check-dtwm-wayland-wp24-noxwayland.py \
             tests/check-dtwm-wayland-hidpi.py \
             tests/check-dtwm-wayland-menus.py \
             tests/check-dtwm-wayland-busy-light.py \
             tests/check-dtwm-wayland-activation.py \
             tests/check-dtwm-wayland-gpu-protocols.py \
             tests/check-dtwm-wayland-pointer-lock.py \
             tests/check-dtwm-wayland-touch-tablet.py \
             tests/check-dtwm-wayland-session-security.py \
             tests/check-dtwm-wayland-color-multidpi.py \
             tests/check-dtwm-wayland-protocols-v2.py \
             tests/check-dtwm-wayland-panel-dnd.py \
             tests/wayland-test-pointer.c \
             tests/check-dtappintegrate-curated.py \
             tests/check-dtwm-wayland-exit-session.py \
             tests/check-dtwm-icon-order.py \
             tests/check-dtwm-push-recall.py \
             wayland/protocols/wlr-layer-shell-unstable-v1.xml \
             wayland/protocols/wlr-output-power-management-unstable-v1.xml \
             wayland/protocols/wlr-foreign-toplevel-management-unstable-v1.xml \
             wayland/protocols/input-method-unstable-v2.xml

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-property-readers.py
	CC="$(CC)" python3 $(srcdir)/tests/check-desktop-requests.py
	CC="$(CC)" python3 $(srcdir)/tests/check-session-word-width.py
	CC="$(CC)" python3 $(srcdir)/tests/check-session-counts.py
	CC="$(CC)" python3 $(srcdir)/tests/check-config-temp-file.py
	CC="$(CC)" python3 $(srcdir)/tests/check-quit-signal.py
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-hardening.py
	python3 $(srcdir)/tests/check-dtappintegrate-curated.py
if BUILD_DTWM_WAYLAND
	python3 $(srcdir)/tests/check-dtwm-wayland.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-workspaces.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-icons.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp05.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp06.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp09.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp10.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp11.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp12.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp13.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp14.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp15.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp16.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp17.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp18.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp19.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp20.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-wp24-noxwayland.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-hidpi.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-menus.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-busy-light.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-activation.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-gpu-protocols.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-pointer-lock.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-touch-tablet.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-session-security.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-color-multidpi.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-protocols-v2.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-panel-dnd.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-wayland-exit-session.py --compositor ./dtwm-wayland
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-icon-order.py --compositor ./dtwm-wayland --dtwm ./dtwm
	CC="$(CC)" python3 $(srcdir)/tests/check-dtwm-push-recall.py --compositor ./dtwm-wayland --dtwm ./dtwm
endif

noinst_DATA = sys.dtwmrc Dtwm.defs

BUILT_SOURCES = sys.dtwmrc Dtwm.defs
CLEANFILES = sys.dtwmrc Dtwm.defs

if BUILD_DTWM_WAYLAND
WAYLAND_GEN_HDRS = wayland/xdg-shell-protocol.h \
                   wayland/wlr-layer-shell-unstable-v1-protocol.h \
                   wayland/wlr-output-power-management-unstable-v1-protocol.h \
                   wayland/cursor-shape-v1-protocol.h \
                   wayland/ext-image-copy-capture-v1-protocol.h \
                   wayland/ext-workspace-v1-protocol.h \
                   wayland/xdg-toplevel-icon-v1-protocol.h \
                   wayland/xdg-system-bell-v1-protocol.h \
                   wayland/pointer-constraints-unstable-v1-protocol.h \
                   wayland/tablet-v2-protocol.h \
                   wayland/xdg-toplevel-tag-v1-protocol.h \
                   wayland/color-management-v1-protocol.h \
                   wayland/tearing-control-v1-protocol.h \
                   wayland/fifo-v1-protocol.h
WAYLAND_GEN_SRCS = wayland/xdg-shell-protocol.c \
                   wayland/ext-workspace-v1-protocol.c \
                   wayland/xdg-toplevel-icon-v1-protocol.c \
                   wayland/xdg-system-bell-v1-protocol.c \
                   wayland/xdg-toplevel-tag-v1-protocol.c \
                   wayland/fifo-v1-protocol.c
BUILT_SOURCES += $(WAYLAND_GEN_HDRS) $(WAYLAND_GEN_SRCS)
CLEANFILES += $(WAYLAND_GEN_HDRS) $(WAYLAND_GEN_SRCS)

wayland/xdg-shell-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/stable/xdg-shell/xdg-shell.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS_DATADIR)/stable/xdg-shell/xdg-shell.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) private-code $< $@

wayland/wlr-layer-shell-unstable-v1-protocol.h: $(srcdir)/wayland/protocols/wlr-layer-shell-unstable-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/wlr-output-power-management-unstable-v1-protocol.h: $(srcdir)/wayland/protocols/wlr-output-power-management-unstable-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/cursor-shape-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/cursor-shape/cursor-shape-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/ext-image-copy-capture-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/ext-workspace-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/ext-workspace/ext-workspace-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/ext-workspace-v1-protocol.c: $(WAYLAND_PROTOCOLS_DATADIR)/staging/ext-workspace/ext-workspace-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) private-code $< $@

wayland/xdg-toplevel-icon-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/xdg-toplevel-icon-v1-protocol.c: $(WAYLAND_PROTOCOLS_DATADIR)/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) private-code $< $@

wayland/xdg-system-bell-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/xdg-system-bell/xdg-system-bell-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/xdg-system-bell-v1-protocol.c: $(WAYLAND_PROTOCOLS_DATADIR)/staging/xdg-system-bell/xdg-system-bell-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) private-code $< $@

wayland/pointer-constraints-unstable-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/tablet-v2-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/unstable/tablet/tablet-unstable-v2.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/xdg-toplevel-tag-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/xdg-toplevel-tag-v1-protocol.c: $(WAYLAND_PROTOCOLS_DATADIR)/staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) private-code $< $@

wayland/color-management-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/color-management/color-management-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/tearing-control-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/tearing-control/tearing-control-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/fifo-v1-protocol.h: $(WAYLAND_PROTOCOLS_DATADIR)/staging/fifo/fifo-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) server-header $< $@

wayland/fifo-v1-protocol.c: $(WAYLAND_PROTOCOLS_DATADIR)/staging/fifo/fifo-v1.xml
	@$(MKDIR_P) wayland
	$(WAYLAND_SCANNER) private-code $< $@

dtwm_wayland_SOURCES = wayland/dtwm_wayland.h \
                       wayland/cde_bevel.c \
                       wayland/cde_workspace.c \
                       wayland/cde_icons.c \
                       wayland/cde_xdnd_bridge.c \
                       wayland/cde_session_lock.c \
                       wayland/cde_output_mgmt.c \
                       wayland/cde_ime.c \
                       wayland/cde_power_display.c \
                       wayland/cde_accessibility.c \
                       wayland/dtwm_wayland_main.c
nodist_dtwm_wayland_SOURCES = $(WAYLAND_GEN_HDRS) $(WAYLAND_GEN_SRCS)
$(dtwm_wayland_OBJECTS): $(WAYLAND_GEN_HDRS)
dtwm_wayland_CPPFLAGS = -I$(builddir)/wayland -I$(srcdir)/wayland -DWLR_USE_UNSTABLE=1 \
                        -DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\"
dtwm_wayland_CFLAGS = $(WAYLAND_COMP_CFLAGS)
dtwm_wayland_LDADD = $(WAYLAND_COMP_LIBS) -lxcb-randr -lxcb-xfixes -linput -ldl -lm

dtwm_panel_SOURCES = wayland/WmFpWayland.c \
                     Button.c        Callback.c      Clock.c \
                     DataBaseLoad.c  PanelS.c        Parse.c \
                     PopupMenu.c     Session.c       UI.c \
                     WmFP.c          WmParse.c
dtwm_panel_CPPFLAGS = $(AM_CPPFLAGS) -I$(srcdir) -DDTWM_WAYLAND_PANEL=1
dtwm_panel_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB) $(XCURSORLIB)
if SOLARIS
dtwm_panel_LDADD += -lintl -lresolv
endif
endif

AM_CPPFLAGS = -DLARGECURSORS -DR2_COMPAT -DOPAQUE -DSHAPE -DUNMAP_ON_RESTART \
	      -DBATCH_DRAG_REQUESTS -DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\" \
	      -DCDE_CONFIGURATION_TOP=\"$(CDE_CONFIGURATION_TOP)\"

dtwm_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB) $(XCURSORLIB) -ldl
dtfplist_LDADD = $(DTCLIENTLIBS) $(TIRPCLIB) $(XTOOLLIB)

if SOLARIS
dtwm_LDADD += -lintl -lresolv
dtfplist_LDADD += -lintl -lresolv
endif

dtwm_SOURCES = WmCDInfo.c      WmCDecor.c      WmCEvent.c \
               WmCPlace.c      WmColormap.c    WmError.c \
               WmEvent.c       WmEwmh.c        WmFeedback.c \
               WmFunction.c    WmGraphics.c    WmIDecor.c \
               WmIPlace.c      WmIconBox.c     WmImage.c \
               WmInitWs.c      WmKeyFocus.c    WmMain.c \
               WmManage.c      WmMenu.c        WmProperty.c \
               WmProtocol.c    WmResCvt.c      WmResParse.c \
               WmResource.c    WmSignal.c      WmWinConf.c \
               ActionIconCache.c WmWinInfo.c     WmWinList.c     WmWinState.c \
               WmMultiHead.c   Button.c        Callback.c \
               Clock.c         DataBaseLoad.c  PanelS.c \
               Parse.c         PopupMenu.c     Session.c \
               UI.c            WmBackdrop.c    WmFP.c \
               WmHelp.c        WmIPC.c         WmInstantTitle.c \
               WmOL.c          WmParse.c       WmParseP.c \
               WmPresence.c    WmWrkspace.c    WmXSMP.c

dtfplist_SOURCES = Print.c Parse.c DataBaseLoad.c WmParse.c Session.c

dtwm_media_ctl_SOURCES = dtwm_media_ctl.c
dtwm_media_ctl_LDADD = -ldl -lm

cde_color_sync_SOURCES = cde_color_sync.c
cde_color_sync_CPPFLAGS = $(AM_CPPFLAGS)
cde_color_sync_LDADD = -lX11 -ldl

SCRIPTFLAGS = -DCDE_INSTALLATION_TOP=$(CDE_INSTALLATION_TOP) \
              -DCDE_CONFIGURATION_TOP=$(CDE_CONFIGURATION_TOP)

Dtwm.defs: Dtwm.defs.src
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(SCRIPTFLAGS) $< > $@

sys.dtwmrc: sys.dtwmrc.src
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(SCRIPTFLAGS) $< > $@
