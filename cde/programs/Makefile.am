MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = backdrops icons dsdm dtmail dtpad dtfile dtwm dtlogin		\
        dtsession dthello dtstyle dtexec dtdbcache dticon dtterm	\
        dtcalc dtaction dtspcd dtscreen dtksh dtcm dtsearchpath		\
        dtappbuilder palettes dtappintegrate dtprintegrate dtconfig	\
        dtcreate dtprintinfo fontaliases dtdspmsg ttsnoop dtimsstart	\
        dtpdm dtsr dtpdmd dtinfo types tttypes util dtopen localized	\
        dthelp dtdocbook

# The user defined font tools are not built by this tree; their shared
# library source is still checked.
EXTRA_DIST = dtudcfonted/libfuty/tests/check-tmp-path-create.py \
             dtudcfonted/libfuty/tests/check-tmp-path-create.c

check-local:
	CC="$(CC)" python3 $(srcdir)/dtudcfonted/libfuty/tests/check-tmp-path-create.py

install-data-hook:
	-if [ $(CDE_INSTALLATION_TOP)/bin != $(bindir) ];then \
		cd $(DESTDIR)$(CDE_INSTALLATION_TOP) && $(LN_S) $(bindir) bin ; fi
