MAINTAINERCLEANFILES = Makefile.in

appdefaultsdir = $(CDE_INSTALLATION_TOP)/app-defaults/C

dist_appdefaults_DATA = Ttsnoop

AB_MAKEFILES = \
	Makefile.aix \
	Makefile.sunos \
	Makefile.linux \
	Makefile.freebsd \
	Makefile.netbsd \
	Makefile.openbsd

DTCODEGENCAT = NLSPATH=$(top_builddir)/programs/dtappbuilder/src/abmf/dtcodegen.cat

DTCODEGEN = $(DTCODEGENCAT) $(top_builddir)/programs/dtappbuilder/src/abmf/dtcodegen

CLEANFILES = Ttsnoop bil-inputs.stamp .dtcodegen.log *.BAK *.delta *.patched $(AB_MAKEFILES) \
	     *_stubs.C *_ui.c

BIL_INPUTS = ttsnoop.bip ttsnoop.bil apiTracer.bil ttChooser.bil \
	patternProps.bil stringChooser.bil messageProps.bil sessionChooser.bil \
	fileChooser.bil argChooser.bil callbackChooser.bil

bil-inputs.stamp: $(addprefix $(srcdir)/,$(BIL_INPUTS))
	if test "$(abs_srcdir)" != "$(abs_builddir)"; then \
		for input in $(BIL_INPUTS); do \
			$(RM) "$$input"; \
			$(LN_S) "$(abs_srcdir)/$$input" "$$input"; \
		done; \
	fi
	$(AM_V_at)touch $@

clean-local:
	if test "$(abs_srcdir)" != "$(abs_builddir)"; then \
		$(RM) $(BIL_INPUTS); \
	fi

.NOTPARALLEL:

bin_PROGRAMS = ttsnoop

ttsnoop_LDADD = $(DTCLIENTLIBS) $(XTOOLLIB) $(LIBTERM)

BUILT_SOURCES = ttsnoop_ui.h apiTracer_ui.h ttChooser_ui.h patternProps_ui.h \
		stringChooser_ui.h messageProps_ui.h sessionChooser_ui.h \
		fileChooser_ui.h argChooser_ui.h callbackChooser_ui.h \
		ttsnoop.h

ttsnoop_SOURCES = stringChooser_stubs.C ttChooser_stubs.C \
		  ttsnoop_ui.c 		apiTracer_stubs.C \
	    	  argChooser_stubs.C 	callbackChooser_stubs.C \
		  fileChooser_stubs.C 	messageProps_stubs.C \
	       	  patternProps_stubs.C 	sessionChooser_stubs.C \
		  ttsnoop.C 		ttsnoop_stubs.C \
	  	  apiTracer_ui.c	argChooser_ui.c \
		  callbackChooser_ui.c  dtb_utils.c \
		  fileChooser_ui.c      messageProps_ui.c \
		  patternProps_ui.c     sessionChooser_ui.c \
		  stringChooser_ui.c    ttChooser_ui.c \
	 	  tt_c++.C 		DtTt.C

ttsnoop_ui.c ttsnoop_ui.h ttsnoop_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/ttsnoop.bil $(srcdir)/ttsnoop_stubs.C.src
	$(CP) $(srcdir)/ttsnoop_stubs.C.src ttsnoop_stubs.c
	-if [ ! -w ttsnoop_stubs.c ]; then \
		chmod +w ttsnoop_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip ttsnoop.bil
	$(RM) ttsnoop_stubs.C
	mv ttsnoop_stubs.c ttsnoop_stubs.C
	cat $(srcdir)/externC.h ttsnoop_ui.h $(srcdir)/closeBrace.h > ttsnoop_ui.h.patched
	mv ttsnoop_ui.h.patched ttsnoop_ui.h
	$(SED) \
	-e 's/^DtbTt/extern String snooperArgv[];DtbTt/' \
	-e 's/XmNheight, 257,//' \
	-e 's/XmNwidth, 580,//' \
	-e 's/XmNcolumns, 80,//' \
	-e 's/XmNrows, 24/DtNsubprocessArgv, snooperArgv/' \
	ttsnoop_ui.c > ttsnoop_ui.c.patched
	mv ttsnoop_ui.c.patched ttsnoop_ui.c

apiTracer_ui.c apiTracer_ui.h apiTracer_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/apiTracer.bil $(srcdir)/apiTracer_stubs.C.src
	$(CP) $(srcdir)/apiTracer_stubs.C.src apiTracer_stubs.c
	-if [ ! -w apiTracer_stubs.c ]; then \
		chmod +w apiTracer_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip apiTracer.bil
	$(RM) apiTracer_stubs.C
	mv apiTracer_stubs.c apiTracer_stubs.C
	cat $(srcdir)/externC.h apiTracer_ui.h $(srcdir)/closeBrace.h > apiTracer_ui.h.patched
	mv apiTracer_ui.h.patched apiTracer_ui.h
	$(SED) -e 's/^DtbApi/extern String apiTracerArgv[];DtbApi/' \
	-e 's/XmNrows, 24/XmNrows, 24, DtNsubprocessArgv, apiTracerArgv/' \
	apiTracer_ui.c > apiTracer_ui.c.patched
	mv apiTracer_ui.c.patched apiTracer_ui.c

ttChooser_ui.c ttChooser_ui.h ttChooser_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/ttChooser.bil $(srcdir)/ttChooser_stubs.C.src
	$(CP) $(srcdir)/ttChooser_stubs.C.src ttChooser_stubs.c
	-if [ ! -w ttChooser_stubs.c ]; then \
		chmod +w ttChooser_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip ttChooser.bil
	$(RM) ttChooser_stubs.C
	mv ttChooser_stubs.c ttChooser_stubs.C
	cat $(srcdir)/externC.h ttChooser_ui.h $(srcdir)/closeBrace.h > ttChooser_ui.h.patched
	mv ttChooser_ui.h.patched ttChooser_ui.h

patternProps_ui.c patternProps_ui.h patternProps_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/patternProps.bil $(srcdir)/patternProps_stubs.C.src
	$(CP) $(srcdir)/patternProps_stubs.C.src patternProps_stubs.c
	-if [ ! -w patternProps_stubs.c ]; then \
		chmod +w patternProps_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip patternProps.bil
	$(RM) patternProps_stubs.C
	mv patternProps_stubs.c patternProps_stubs.C
	cat $(srcdir)/externC.h patternProps_ui.h $(srcdir)/closeBrace.h > patternProps_ui.h.patched
	mv patternProps_ui.h.patched patternProps_ui.h

stringChooser_ui.c stringChooser_ui.h stringChooser_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/stringChooser.bil $(srcdir)/stringChooser_stubs.C.src
	$(CP) $(srcdir)/stringChooser_stubs.C.src stringChooser_stubs.c
	-if [ ! -w stringChooser_stubs.c ]; then \
		chmod +w stringChooser_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip stringChooser.bil
	$(RM) stringChooser_stubs.C
	mv stringChooser_stubs.c stringChooser_stubs.C
	cat $(srcdir)/externC.h stringChooser_ui.h $(srcdir)/closeBrace.h > stringChooser_ui.h.patched
	mv stringChooser_ui.h.patched stringChooser_ui.h

messageProps_ui.c messageProps_ui.h messageProps_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/messageProps.bil $(srcdir)/messageProps_stubs.C.src
	$(CP) $(srcdir)/messageProps_stubs.C.src messageProps_stubs.c
	-if [ ! -w messageProps_stubs.c ]; then \
		chmod +w messageProps_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip messageProps.bil
	$(RM) messageProps_stubs.C
	mv messageProps_stubs.c messageProps_stubs.C
	cat $(srcdir)/externC.h messageProps_ui.h $(srcdir)/closeBrace.h > messageProps_ui.h.patched
	mv messageProps_ui.h.patched messageProps_ui.h

sessionChooser_ui.c sessionChooser_ui.h sessionChooser_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/sessionChooser.bil $(srcdir)/sessionChooser_stubs.C.src
	$(CP) $(srcdir)/sessionChooser_stubs.C.src sessionChooser_stubs.c
	-if [ ! -w sessionChooser_stubs.c ]; then \
		chmod +w sessionChooser_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip sessionChooser.bil
	$(RM) sessionChooser_stubs.C
	mv sessionChooser_stubs.c sessionChooser_stubs.C
	cat $(srcdir)/externC.h sessionChooser_ui.h $(srcdir)/closeBrace.h > sessionChooser_ui.h.patched
	mv sessionChooser_ui.h.patched sessionChooser_ui.h

fileChooser_ui.c fileChooser_ui.h fileChooser_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/fileChooser.bil $(srcdir)/fileChooser_stubs.C.src
	$(CP) $(srcdir)/fileChooser_stubs.C.src fileChooser_stubs.c
	-if [ ! -w fileChooser_stubs.c ]; then \
		chmod +w fileChooser_stubs.c; fi 
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip fileChooser.bil
	$(RM) fileChooser_stubs.C
	mv fileChooser_stubs.c fileChooser_stubs.C
	cat $(srcdir)/externC.h fileChooser_ui.h $(srcdir)/closeBrace.h > fileChooser_ui.h.patched
	mv fileChooser_ui.h.patched fileChooser_ui.h

argChooser_ui.c argChooser_ui.h argChooser_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/argChooser.bil $(srcdir)/argChooser_stubs.C.src
	$(CP) $(srcdir)/argChooser_stubs.C.src argChooser_stubs.c
	-if [ ! -w argChooser_stubs.c ]; then \
		chmod +w argChooser_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip argChooser.bil
	$(RM) argChooser_stubs.C
	mv argChooser_stubs.c argChooser_stubs.C
	cat $(srcdir)/externC.h argChooser_ui.h $(srcdir)/closeBrace.h > argChooser_ui.h.patched
	mv argChooser_ui.h.patched argChooser_ui.h

callbackChooser_ui.c callbackChooser_ui.h callbackChooser_stubs.C: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/callbackChooser.bil $(srcdir)/callbackChooser_stubs.C.src
	$(CP) $(srcdir)/callbackChooser_stubs.C.src callbackChooser_stubs.c
	-if [ ! -w callbackChooser_stubs.c ]; then \
		chmod +w callbackChooser_stubs.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip callbackChooser.bil
	$(RM) callbackChooser_stubs.C
	mv callbackChooser_stubs.c callbackChooser_stubs.C
	cat $(srcdir)/externC.h callbackChooser_ui.h $(srcdir)/closeBrace.h > callbackChooser_ui.h.patched
	mv callbackChooser_ui.h.patched callbackChooser_ui.h

ttsnoop.C ttsnoop.h dtb_utils.c dtb_utils.h: bil-inputs.stamp $(srcdir)/ttsnoop.bip $(srcdir)/ttsnoop.C.src
	$(CP) $(srcdir)/ttsnoop.C.src ttsnoop.c
	-if [ ! -w ttsnoop.c ]; then chmod +w ttsnoop.c; fi
	$(DTCODEGEN) -changed -merge -p ttsnoop.bip -main
	$(RM) ttsnoop.C
	mv ttsnoop.c ttsnoop.C
	chmod -w ttsnoop.C
	cat $(srcdir)/externC.h dtb_utils.h $(srcdir)/closeBrace.h > dtb_utils.h.patched
	mv dtb_utils.h.patched dtb_utils.h
	$(SED) \
	-e 's/\(MoreButton;\)/\1Boolean _DtCanHelp( const char * );/' \
	-e 's/\(XtManageChild(Quick_help\)/XtSetSensitive(MoreButton,_DtCanHelp(help_data->help_text));\1/' \
	-e 's/\(buffer, vol = NULL, loc = NULL;\)/\1Boolean _DtHelped( Widget );/' \
	-e 's/\(if( (cp=strrchr\)/if (_DtHelped( help_dialog)) return;\1/' \
	dtb_utils.c > dtb_utils.c.patched
	mv dtb_utils.c.patched dtb_utils.c

Ttsnoop: $(srcdir)/Ttsnoop.src
	$(RM) $@
	$(GENCPP) -P -DXCOMM=# $(LOCAL_CPP_DEFINES) $< > $@
