PORTNAME=	ck-core-cde
DISTVERSION=	3.0.0
CATEGORIES=	x11 wayland

MAINTAINER=	cklukas@users.noreply.github.com
COMMENT=	CK Core Common Desktop Environment (Wayland seatd/drm-kmod and X11)
WWW=		https://github.com/cklukas/ck-core-cde

LICENSE=	LGPL21+ EPL
LICENSE_COMB=	multi

BUILD_DEPENDS=	/usr/dt/lib/libXm.so.9:x11-toolkits/ck-core-motif9 \
		/usr/dt/lib/libck_file_engine.so:sysutils/ck-core-fs-lib \
		evdev-proto>0:devel/evdev-proto \
		wayland-protocols>=1.38:graphics/wayland-protocols
LIB_DEPENDS=	libarchive.so:archivers/libarchive \
		libcairo.so:graphics/cairo \
		libcups.so:print/cups \
		libcurl.so:ftp/curl \
		libdbus-1.so:devel/dbus \
		libdrm.so:graphics/libdrm \
		libfontconfig.so:x11-fonts/fontconfig \
		libfreetype.so:print/freetype2 \
		libgio-2.0.so:devel/glib20 \
		libinput.so:x11/libinput \
		liblmdb.so:databases/lmdb \
		libpango-1.0.so:x11-toolkits/pango \
		libpixman-1.so:x11/pixman \
		libseat.so:sysutils/seatd \
		libwayland-server.so:graphics/wayland \
		libwlroots-0.19.so:x11-toolkits/wlroots019 \
		libxcb-randr.so:x11/libxcb \
		libxkbcommon.so:x11/libxkbcommon
RUN_DEPENDS=	/usr/dt/lib/libXm.so.9:x11-toolkits/ck-core-motif9 \
		/usr/dt/lib/libck_file_engine.so:sysutils/ck-core-fs-lib \
		dbus-daemon:devel/dbus \
		ksh93:shells/ksh \
		seatd:sysutils/seatd \
		Xwayland:x11-servers/xwayland

USES=		autoreconf bison compiler:c++17-lang gettext gmake iconv jpeg \
		libtool localbase pkgconfig ssl tcl xorg
USE_GITHUB=	yes
GH_ACCOUNT=	cklukas
GH_PROJECT=	ck-core-cde
USE_XORG=	ice sm x11 xau xcursor xdmcp xext xft xi xinerama xmu xpm \
		xrandr xrender xscrnsaver xt xtst
USE_LDCONFIG=	/usr/dt/lib
USE_RC_SUBR=	dtlogin rpc_cmsd
SUB_FILES=	dtlogin.pam dtlogin-greeter.pam dtsession.pam pkg-message

WRKSRC_SUBDIR=	cde
GNU_CONFIGURE=	yes
PREFIX=		/usr/dt
CONFIGURE_ENV=	PATH="/usr/dt/bin:${PATH}" \
		PKG_CONFIG_PATH="/usr/dt/libdata/pkgconfig:/usr/dt/lib/pkgconfig:${LOCALBASE}/libdata/pkgconfig" \
		CPPFLAGS="-I/usr/dt/include -I${LOCALBASE}/include" \
		LDFLAGS="-L/usr/dt/lib -Wl,-rpath,/usr/dt/lib -L${LOCALBASE}/lib"
CONFIGURE_ARGS=	--prefix=/usr/dt \
		--sysconfdir=${LOCALBASE}/etc

USERS=		_dtgreeter
GROUPS=		_dtgreeter

post-install:
	${INSTALL_SCRIPT} ${WRKSRC}/debian/Xsession.wayland \
		${STAGEDIR}/usr/dt/bin/Xsession.wayland
	@${MKDIR} ${STAGEDIR}${LOCALBASE}/share/xsessions \
		${STAGEDIR}${LOCALBASE}/share/wayland-sessions \
		${STAGEDIR}${LOCALBASE}/share/xdg-desktop-portal/portals \
		${STAGEDIR}${LOCALBASE}/share/dbus-1/services \
		${STAGEDIR}${LOCALBASE}/etc/pam.d
	${INSTALL_DATA} ${WRKSRC}/debian/cde.desktop \
		${STAGEDIR}${LOCALBASE}/share/xsessions/cde.desktop
	${INSTALL_DATA} ${WRKSRC}/debian/cde-wayland.desktop \
		${STAGEDIR}${LOCALBASE}/share/wayland-sessions/cde-wayland.desktop
	${INSTALL_DATA} ${WRKSRC}/debian/cde-portals.conf \
		${STAGEDIR}${LOCALBASE}/share/xdg-desktop-portal/cde-portals.conf
	${INSTALL_DATA} ${WRKSRC}/programs/dtportal/cde.portal \
		${STAGEDIR}${LOCALBASE}/share/xdg-desktop-portal/portals/cde.portal
	${INSTALL_DATA} ${WRKDIR}/dtlogin.pam \
		${STAGEDIR}${LOCALBASE}/etc/pam.d/dtlogin
	${INSTALL_DATA} ${WRKDIR}/dtlogin-greeter.pam \
		${STAGEDIR}${LOCALBASE}/etc/pam.d/dtlogin-greeter
	${INSTALL_DATA} ${WRKDIR}/dtsession.pam \
		${STAGEDIR}${LOCALBASE}/etc/pam.d/dtsession

.include <bsd.port.mk>
