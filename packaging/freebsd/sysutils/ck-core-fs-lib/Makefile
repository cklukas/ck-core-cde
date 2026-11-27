PORTNAME=	ck-core-fs-lib
DISTVERSION=	0.3.0
CATEGORIES=	sysutils

MAINTAINER=	cklukas@users.noreply.github.com
COMMENT=	CK Core headless filesystem synchronization, trash, and search engine
WWW=		https://github.com/cklukas/ck-core-fs-lib

LICENSE=	MIT

USES=		cmake compiler:c++20-lang pkgconfig
USE_GITHUB=	yes
GH_ACCOUNT=	cklukas
GH_PROJECT=	ck-core-fs-lib
USE_LDCONFIG=	/usr/dt/lib

PREFIX=		/usr/dt
CMAKE_ARGS=	-DCMAKE_INSTALL_PREFIX=/usr/dt \
		-DCMAKE_INSTALL_RPATH=/usr/dt/lib

.include <bsd.port.mk>
