PORTNAME=	ck-core-apps
DISTVERSION=	1.0.0
CATEGORIES=	x11 wayland

MAINTAINER=	cklukas@users.noreply.github.com
COMMENT=	CK Core Motif ABI 9 desktop applications for CDE (X11 and Wayland)
WWW=		https://github.com/cklukas/ck-core-apps

LICENSE=	MIT

BUILD_DEPENDS=	/usr/dt/lib/libXm.so.9:x11-toolkits/ck-core-motif9 \
		/usr/dt/lib/libDtSvc.so:x11/ck-core-cde
LIB_DEPENDS=	libasound.so:audio/alsa-lib \
		libavcodec.so:multimedia/ffmpeg \
		libcairo.so:graphics/cairo \
		libcurl.so:ftp/curl \
		libdrm.so:graphics/libdrm \
		libpoppler-glib.so:graphics/poppler-glib \
		libwayland-client.so:graphics/wayland
RUN_DEPENDS=	/usr/dt/lib/libXm.so.9:x11-toolkits/ck-core-motif9 \
		/usr/dt/bin/dtwm-wayland:x11/ck-core-cde

USES=		compiler:c++20-lang desktop-file-utils gmake localbase pkgconfig xorg
USE_GITHUB=	yes
GH_ACCOUNT=	cklukas
GH_PROJECT=	ck-core-apps
USE_XORG=	ice sm x11 xext xft xi xinerama xmu xpm xrandr xrender xt

MAKE_ARGS=	MOTIF_PREFIX=/usr/dt \
		CDE_PREFIX=/usr/dt \
		CDE_SRC_INC=/usr/dt/include

do-install:
	@${MKDIR} ${STAGEDIR}${PREFIX}/bin
	for b in ${WRKSRC}/build/bin/*; do \
		[ -f "$$b" ] || continue; \
		${INSTALL_PROGRAM} "$$b" ${STAGEDIR}${PREFIX}/bin/; \
	done
	${SETENV} ${MAKE_ENV} ${MAKE_CMD} -C ${WRKSRC} \
		PREFIX=${PREFIX} DESTDIR=${STAGEDIR} \
		install-toolbar-icons install-midi-assets install-audio-assets install-video-assets

.include <bsd.port.mk>
