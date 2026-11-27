MAINTAINERCLEANFILES = Makefile.in

EXTRA_DIST = cde.portal \
             org.freedesktop.impl.portal.desktop.cde.service.in \
             xdg-desktop-portal-cde.service.in \
             tests/check-xdg-desktop-portal-cde.py

if BUILD_DTPORTAL
bin_PROGRAMS = xdg-desktop-portal-cde

xdg_desktop_portal_cde_SOURCES = dtportal.h \
                                 portal_dialogs.c \
                                 portal_remote_eis.c \
                                 dtportal_main.c

xdg_desktop_portal_cde_CPPFLAGS = -DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\" \
                                  -DCDE_CONFIGURATION_TOP=\"$(CDE_CONFIGURATION_TOP)\" \
                                  $(DTPORTAL_DEPS_CFLAGS) \
                                  $(WAYLAND_CLIENT_CFLAGS)
xdg_desktop_portal_cde_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB) \
                               $(DTPORTAL_DEPS_LIBS) \
                               $(WAYLAND_CLIENT_LIBS) \
                               -ldl -lm

if HAVE_CK_CORE_FS
xdg_desktop_portal_cde_CPPFLAGS += -DHAVE_CK_CORE_FS=1 $(CK_CORE_FS_CFLAGS)
xdg_desktop_portal_cde_LDADD += $(CK_CORE_FS_LIBS)
endif

portaldir = $(datadir)/xdg-desktop-portal/portals
portal_DATA = cde.portal

dbusservicedir = $(datadir)/dbus-1/services
dbusservice_DATA = org.freedesktop.impl.portal.desktop.cde.service

systemduserdir = $(prefix)/lib/systemd/user
systemduser_DATA = xdg-desktop-portal-cde.service

CLEANFILES = org.freedesktop.impl.portal.desktop.cde.service xdg-desktop-portal-cde.service

org.freedesktop.impl.portal.desktop.cde.service: org.freedesktop.impl.portal.desktop.cde.service.in
	sed -e 's|@CDE_INSTALLATION_TOP[@]|$(CDE_INSTALLATION_TOP)|g' \
	    $(srcdir)/org.freedesktop.impl.portal.desktop.cde.service.in > $@

xdg-desktop-portal-cde.service: xdg-desktop-portal-cde.service.in
	sed -e 's|@CDE_INSTALLATION_TOP[@]|$(CDE_INSTALLATION_TOP)|g' \
	    $(srcdir)/xdg-desktop-portal-cde.service.in > $@

if BUILD_DTWM_WAYLAND
check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-xdg-desktop-portal-cde.py \
	  --portal ./xdg-desktop-portal-cde \
	  --compositor ../dtwm/dtwm-wayland
endif
endif
