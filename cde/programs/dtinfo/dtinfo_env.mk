# Some common env variables needed by the various modules.  This file
# is included by various Makefile.am files in the dtinfo hierarchy.

# common defines
DTINFO_DEFINES = -DDtinfoClient -DUseWideChars -DInternationalize	\
-DUseMotifXpm -DUseTooltalk -DUseSessionMgmt -DDTSEARCH -DCDE_NEXT	\
-DEXPAND_TEMPLATES

if LINUX
DTINFO_DEFINES += -DNATIVE_EXCEPTIONS
endif

if BSD
DTINFO_DEFINES += -DNATIVE_EXCEPTIONS
endif

if SOLARIS
DTINFO_DEFINES += -DHAS_TERMINATE -DNATIVE_EXCEPTIONS
endif

# Build and maintained source locations.  Generated interfaces take
# precedence, while clean shadow builds can still resolve checked-in headers.
OLIAS = $(top_builddir)/programs/dtinfo
OLIASSRC = $(top_srcdir)/programs/dtinfo
WWL = $(OLIAS)/dtinfo/wwl
WWLSRC = $(OLIASSRC)/dtinfo/wwl
MMDB = $(top_builddir)/lib/DtMmdb
MMDBSRC = $(top_srcdir)/lib/DtMmdb
TOOLS = $(OLIAS)/tools
UAS = $(OLIAS)/dtinfo/src/UAS
UASSRC = $(OLIASSRC)/dtinfo/src/UAS

WWL_INCLUDES = -I$(WWL)/include -I$(WWLSRC)/include
WWL_LIBS=$(STATIC) -L$(WWL)/src -lWWL
MMDB_INCLUDES = -I$(MMDB) -I$(MMDBSRC)
MMDB_LIBS= -L$(MMDB) -lDtMmdb
UAS_INCLUDES = -I$(UAS)/Base -I$(UASSRC)/Base
TREERES = $(TOOLS)/misc/treeres
MSGSETS = $(TOOLS)/misc/msgsets

DTINFO_INCLUDES = -I$(OLIAS)/dtinfo/src -I$(OLIASSRC)/dtinfo/src \
	$(UAS_INCLUDES) $(MMDB_INCLUDES) $(WWL_INCLUDES)
