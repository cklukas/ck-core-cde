MAINTAINERCLEANFILES = Makefile.in

# get our env variables
include $(top_srcdir)/programs/dtinfo/dtinfo_env.mk

# get the toplevel Classlist
include Classlist.mk

# CLASSLIST is defined in the top (dtinfo) level Classlist.mk file
SUBDIRS = Widgets $(CLASSLIBS) Messages cgm

RESFILES = Macros.res Strings.res Messages.res Query/QueryEditor.res	\
	Agents/BookmarkEdit.res Agents/MapAgent.res			\
	Agents/MarkList.res Agents/PrintPanelAgent.res			\
	Agents/Preferences.res Agents/SearchResultsAgent.res		\
	Agents/NodeHistoryAgent.res Agents/NodeWindowAgent.res		\
	Agents/GraphicAgent.res Query/QueryTermView.res


BUILT_SOURCES = Prelude.h Support/Support.d Support/Support.h		\
	UAS/UAS.d UAS/UAS.h Basic/Basic.d Basic/Basic.h			\
	OliasSearch/OliasSearch.d OliasSearch/OliasSearch.h		\
	Marks/Marks.d Marks/Marks.h Graphics/Graphics.d			\
	Graphics/Graphics.h Preferences/Preferences.d			\
	Preferences/Preferences.h Managers/Managers.d			\
	Managers/Managers.h Other/Other.d Other/Other.h Query/Query.d	\
	Query/Query.h Agents/Agents.d Agents/Agents.h			\
	OnlineRender/OnlineRender.d OnlineRender/OnlineRender.h		\
									\
	Dtinfo

CLEANFILES = $(BUILT_SOURCES)

Prelude.h:
	$(top_builddir)/programs/dtinfo/tools/misc/pmaker $(CLASSLIBS)

Dtinfo: Dtinfo.res $(RESFILES)
	$(CPP_PROGRAM) -traditional -nostdinc -C $(srcdir)/Dtinfo.res | $(TREERES) > $@

# dtinfo class dfile and hfile building for the modules
include $(top_srcdir)/programs/dtinfo/dtinfo/src/dtinfo_dfiles_hfiles.mk

# gather up the messages and generate the file. MSGS comes from the
# dtinfo/src/Classlist.mk file.

noinst_DATA = dtinfo.msg.updated

dtinfo.msg.updated:
	@set -e; \
	 msgs=; for i in $(MSGS) ; do msgs="$$msgs $$i/$$i.msg"; done; \
	 trap '$(RM) dtinfo.cpp dtinfo.msg.preprocessed $@.tmp' 0 1 2 3 15; \
	 cat $$msgs > dtinfo.cpp; \
	 $(CPP_PROGRAM) -traditional -nostdinc -P -C -I$(abs_builddir) \
	   $(srcdir)/dtinfo_msg.tmpl > dtinfo.msg.preprocessed; \
	 $(MSGSETS) < dtinfo.msg.preprocessed > $@.tmp; \
	 mv $@.tmp $@

CLEANFILES += dtinfo.msg.updated dtinfo.cpp dtinfo.msg.preprocessed \
	      dtinfo.msg.updated.tmp

## dtinfo proper
OLIASNORMALLIBS= Agents/libAgents.a Other/libOther.a			\
           Basic/libBasic.a Preferences/libPreferences.a		\
           UAS/libUAS.a Query/libQuery.a Graphics/libGraphics.a	\
           Managers/libManagers.a Support/libSupport.a		\
           Marks/libMarks.a OnlineRender/libOnlineRender.a		\
           OliasSearch/libOliasSearch.a Widgets/libWidgets.a		\
           cgm/libcgm.a

bin_PROGRAMS = dtinfo

dtinfo_CXXFLAGS = $(DTINFO_DEFINES) $(DTINFO_INCLUDES)

dtinfo_LDADD = $(OLIASNORMALLIBS) $(LIBSEARCH) $(MMDB_LIBS) $(WWL_LIBS) \
	$(DTCLIENTLIBS) $(XTOOLLIB) $(LMDBLIB)

dtinfo_SOURCES = main.C Support/InputNotifier.C
