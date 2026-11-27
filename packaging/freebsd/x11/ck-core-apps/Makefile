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

USES=		cmake compiler:c++20-lang desktop-file-utils pkgconfig xorg
USE_GITHUB=	yes
GH_ACCOUNT=	cklukas
GH_PROJECT=	ck-core-apps
USE_XORG=	x11 xext xft xi xmu xpm xrandr xrender xt

CMAKE_ARGS=	-DCMAKE_PREFIX_PATH="/usr/dt;${LOCALBASE}" \
		-DCMAKE_INSTALL_RPATH="/usr/dt/lib:${LOCALBASE}/lib"

.include <bsd.port.mk>
