MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = dtstyle

EXTRA_DIST = tests/check-home-environment.py tests/check-home-environment.c \
             tests/check-palette-file-create.py tests/check-palette-file-create.c \
             tests/check-exit-signal.py tests/check-exit-signal.c \
             tests/check-color-minmax.py tests/check-color-minmax.c \
             tests/check-dtstyle-hardening.py tests/check-dtstyle-hardening.c

check-local:
	CC="$(CC)" python3 $(srcdir)/tests/check-home-environment.py
	CC="$(CC)" python3 $(srcdir)/tests/check-palette-file-create.py
	CC="$(CC)" python3 $(srcdir)/tests/check-exit-signal.py
	CC="$(CC)" python3 $(srcdir)/tests/check-color-minmax.py
	CC="$(CC)" python3 $(srcdir)/tests/check-dtstyle-hardening.py

dtstyle_CPPFLAGS = -DCDE_CONFIGURATION_TOP=\"$(CDE_CONFIGURATION_TOP)\"	\
	-DCDE_INSTALLATION_TOP=\"$(CDE_INSTALLATION_TOP)\" -I../dthelp	\
	$(DT_INCDIR)

dtstyle_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB) $(XCURSORLIB) -lcairo -lm -ldl

if LINUX
dtstyle_CPPFLAGS += -DHAVE_XRANDR=1
dtstyle_LDADD += -lXrandr
endif

if BSD
dtstyle_CPPFLAGS += -DHAVE_XRANDR=1
dtstyle_LDADD += -lXrandr
endif

dtstyle_SOURCES = Main.c MainWin.c Font.c FontParser.c FontCatalog.c FontFaceCatalog.c Audio.c Startup.c Mouse.c	\
		  SaveRestore.c Backdrop.c Screen.c Display.c Keyboard.c		\
		  ColorMain.c ColorPalette.c ColorFile.c ColorEdit.c	\
		  Resource.c Protocol.c Dtwm.c Help.c HelpCB.c		\
		  I18nMain.c I18nEnv.c I18nUtil.c Audio.h ColorMain.h	\
		  Help.h Keyboard.h OWsync.h Screen.h Display.h Backdrop.h	\
		  ColorPalette.h I18nEnv.h Main.h Protocol.h		\
		  Startup.h ColorEdit.h Dtwm.h I18nMain.h MainWin.h	\
		  Resource.h ColorFile.h Font.h FontCatalog.h FontFaceCatalog.h I18nUtil.h Mouse.h	\
		  SaveRestore.h

if SOLARIS
dtstyle_SOURCES += OWsync.c
endif

LOCAL_CPP_DEFINES = -DCDE_INSTALLATION_TOP=${prefix}

CPP_TARGETS = Dtstyle Backdrops Palettes

BUILT_SOURCES = $(CPP_TARGETS)
CLEANFILES = $(CPP_TARGETS)

Dtstyle: Dtstyle.src
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(LOCAL_CPP_DEFINES) $< > $@

Backdrops: Backdrops.src
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(LOCAL_CPP_DEFINES) $< > $@

Palettes: Palettes.src
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(LOCAL_CPP_DEFINES) $< > $@
