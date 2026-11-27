PORTNAME=	ck-core-motif9
DISTVERSION=	2.4.0
CATEGORIES=	x11-toolkits wayland

MAINTAINER=	cklukas@users.noreply.github.com
COMMENT=	CK Core Motif ABI 9 toolkit (X11 and Wayland backends) under /usr/dt
WWW=		https://github.com/cklukas/ck-core-motif

LICENSE=	LGPL21+

BUILD_DEPENDS=	wayland-protocols>=1.38:graphics/wayland-protocols \
		evdev-proto>0:devel/evdev-proto
LIB_DEPENDS=	libfontconfig.so:x11-fonts/fontconfig \
		libfreetype.so:print/freetype2 \
		libjpeg.so:graphics/jpeg-turbo \
		libpng.so:graphics/png \
		libwayland-client.so:graphics/wayland \
		libxkbcommon.so:x11/libxkbcommon

USES=		autoreconf gmake iconv libtool localbase pkgconfig xorg
USE_GITHUB=	yes
GH_ACCOUNT=	cklukas
GH_PROJECT=	ck-core-motif
USE_XORG=	ice sm x11 xext xft xi xmu xp xpm xrender xt
USE_LDCONFIG=	/usr/dt/lib

GNU_CONFIGURE=	yes
PREFIX=		/usr/dt
CONFIGURE_ARGS=	--prefix=/usr/dt \
		--enable-wayland \
		--enable-xft \
		--enable-utf8 \
		--disable-static

INSTALL_TARGET=	install-strip

.include <bsd.port.mk>
