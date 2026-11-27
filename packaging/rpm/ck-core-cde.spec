# Matched RPM cohort spec for Fedora 41+, RHEL / AlmaLinux / Rocky 9-10, and openSUSE Tumbleweed / Leap 15.6.
# Produces:
#   - ck-core-motif9  (2.4.0, /usr/dt/lib/libXm.so.9, coexists with system motif / libXm4)
#   - ck-core-fs-lib  (0.3.0, /usr/dt/lib/libck_file_engine.so*)
#   - ck-core-cde     (3.0.0, X11/XLibre and Wayland desktop + xdg-desktop-portal-cde + rpc-cmsd)
#   - ck-core-dtlogin (3.0.0, Wayland seat + X11 display manager, _dtgreeter + PAM + systemd)
#   - ck-core-apps    (1.0.0, CK Core Motif desktop applications)

%bcond_with prebuilt_stage
%global debug_package %{nil}
%global _build_id_links none

# Isolate private /usr/dt/lib ABI 9 SONAMEs so ck-core-motif9 never collides with
# system motif (Fedora/RHEL libXm.so.4) or libXm4 (openSUSE).
%global __provides_exclude_from ^/usr/dt/lib/.*\\.so.*$
%global __requires_exclude ^(libXm|libMrm|libUil|libDt.*|libtt|libcsa|libck_.*)\\.so.*$

%global motif_ver 2.4.0
%global fs_ver    0.3.0
%global cde_ver   3.0.0
%global apps_ver  1.0.0

Name:           ck-core-cde
Version:        %{cde_ver}
Release:        1%{?dist}
Summary:        CK Core Common Desktop Environment (X11/XLibre and Wayland)
License:        LGPL-2.1-or-later AND EPL-2.0
URL:            https://github.com/cklukas/ck-core-cde

%if %{without prebuilt_stage}
Source0:        ck-core-motif-%{motif_ver}.tar.xz
Source1:        ck-core-fs-lib-%{fs_ver}.tar.xz
Source2:        ck-core-cde-%{cde_ver}.tar.xz
Source3:        ck-core-apps-%{apps_ver}.tar.xz

BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  bison
BuildRequires:  cmake
BuildRequires:  flex
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  gettext-devel
BuildRequires:  libtool
BuildRequires:  make
BuildRequires:  ninja-build
BuildRequires:  patchelf
BuildRequires:  pkgconfig
BuildRequires:  python3
BuildRequires:  systemd-rpm-macros
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(cairo-pdf)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(libarchive)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libinput)
BuildRequires:  pkgconfig(libjpeg)
BuildRequires:  pkgconfig(libpng)
BuildRequires:  pkgconfig(libtirpc)
BuildRequires:  pkgconfig(lmdb)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(wlroots-0.19)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-randr)
BuildRequires:  pkgconfig(xcb-xfixes)
BuildRequires:  pkgconfig(xcursor)
BuildRequires:  pkgconfig(xdmcp)
BuildRequires:  pkgconfig(xext)
BuildRequires:  pkgconfig(xft)
BuildRequires:  pkgconfig(xi)
BuildRequires:  pkgconfig(xinerama)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(xmu)
BuildRequires:  pkgconfig(xpm)
BuildRequires:  pkgconfig(xrandr)
BuildRequires:  pkgconfig(xrender)
BuildRequires:  pkgconfig(xss)
BuildRequires:  pkgconfig(xt)
BuildRequires:  pkgconfig(xtst)
%if 0%{?suse_version}
BuildRequires:  cups-devel
BuildRequires:  pam-devel
BuildRequires:  tcl-devel
%else
BuildRequires:  cups-devel
BuildRequires:  pam-devel
BuildRequires:  tcl-devel
%endif
%else
BuildRequires:  systemd-rpm-macros
%endif

Requires:       ck-core-motif9 = %{motif_ver}-%{release}
Requires:       ck-core-fs-lib = %{fs_ver}-%{release}
Requires:       rpcbind
%if 0%{?suse_version}
Requires:       ksh
Requires:       xorg-x11-fonts-core
Requires:       (xwayland or xorg-x11-server)
Recommends:     ck-core-apps
Recommends:     xdg-desktop-portal
Recommends:     xdg-desktop-portal-wlr
Recommends:     pipewire
%else
Requires:       ksh
Requires:       xorg-x11-server-utils
Requires:       (xorg-x11-server-Xwayland or xorg-x11-server-Xorg)
Recommends:     ck-core-apps
Recommends:     gnome-keyring
Recommends:     gnupg2
Recommends:     libsecret
Recommends:     pipewire
Recommends:     xdg-desktop-portal
Recommends:     xdg-desktop-portal-wlr
%endif
%{?systemd_requires}

%description
Modernized C23 Common Desktop Environment built against CK Core Motif ABI 9,
shipping both the classic X11/XLibre dtwm session and the native wlroots-0.19
dtwm-wayland compositor, xdg-desktop-portal-cde, Cairo/Pango PDF + CUPS IPP
printing, OAuth2/JMAP dtmail, CalDAV dtcm, and ck-core-fs-lib file operations.

%package -n ck-core-motif9
Version:        %{motif_ver}
Summary:        CK Core Motif 2.4.0 native ABI 9 runtime, headers, and UIL compiler

%description -n ck-core-motif9
CK Core Motif ABI 9 toolkit installed beneath /usr/dt with Wayland and X11
backends, coexisting cleanly with distribution Motif 2.3 (libXm.so.4).

%package -n ck-core-fs-lib
Version:        %{fs_ver}
Summary:        CK Core headless filesystem synchronization and search engine

%description -n ck-core-fs-lib
Headless C/C++ filesystem comparison, transfer, trash, and streaming search
backend shared by dtfile, xdg-desktop-portal-cde, and CK Core utilities.

%package -n ck-core-dtlogin
Version:        %{cde_ver}
Summary:        CK Core CDE Wayland and X11 Display Manager (dtlogin)
Requires:       ck-core-cde = %{cde_ver}-%{release}
Requires(pre):  shadow-utils
%{?systemd_requires}

%description -n ck-core-dtlogin
Display manager for CK Core CDE supporting unprivileged _dtgreeter Wayland
seats (systemd-logind or seatd), X11/XLibre local displays, PAM session setup,
and FreeDesktop wayland-sessions / xsessions discovery.

%package -n ck-core-apps
Version:        %{apps_ver}
Summary:        CK Core Motif desktop applications for CDE (X11 and Wayland)
Requires:       ck-core-motif9 = %{motif_ver}-%{release}
Requires:       ck-core-cde = %{cde_ver}-%{release}

%description -n ck-core-apps
Companion native Motif ABI 9 applications (ck-calc, ck-clock, ck-grab, ck-pdf,
ck-video, ck-view, ck-browser, ck-status, ck-tasks, and utilities) integrated
with CDE actions, color palettes, and Wayland protocols.

%prep
%if %{without prebuilt_stage}
%setup -q -c -T -a 0 -a 1 -a 2 -a 3
%endif

%build
%if %{without prebuilt_stage}
STAGE="%{_builddir}/stage-pfx"
rm -rf "$STAGE"
mkdir -p "$STAGE"

pushd ck-core-motif-%{motif_ver}
./autogen.sh
./configure --prefix=/usr/dt --enable-wayland --enable-xft --enable-utf8
%make_build
make install DESTDIR="$STAGE/motif"
popd

cmake -S ck-core-fs-lib-%{fs_ver} -B build-fs -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/dt
cmake --build build-fs
DESTDIR="$STAGE/fs" cmake --install build-fs

pushd ck-core-cde-%{cde_ver}/cde
export PATH="$STAGE/motif/usr/dt/bin:$PATH"
export PKG_CONFIG_PATH="$STAGE/motif/usr/dt/lib/pkgconfig:$STAGE/fs/usr/dt/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CPPFLAGS="-I$STAGE/motif/usr/dt/include -I$STAGE/fs/usr/dt/include"
export LDFLAGS="-L$STAGE/motif/usr/dt/lib -L$STAGE/fs/usr/dt/lib -Wl,-rpath,/usr/dt/lib"
./autogen.sh
./configure --prefix=/usr/dt --sysconfdir=/etc
%make_build
make install DESTDIR="$STAGE/cde"
popd

cmake -S ck-core-apps-%{apps_ver} -B build-apps -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH="$STAGE/motif/usr/dt;$STAGE/cde/usr/dt" \
  -DCMAKE_INSTALL_RPATH="/usr/dt/lib"
cmake --build build-apps
DESTDIR="$STAGE/apps" cmake --install build-apps
%endif

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}

%if %{with prebuilt_stage}
cp -a %{prebuilt_stage_dir}/. %{buildroot}/
%else
STAGE="%{_builddir}/stage-pfx"
cp -a "$STAGE/motif/." %{buildroot}/
cp -a "$STAGE/fs/."    %{buildroot}/
cp -a "$STAGE/cde/."   %{buildroot}/
cp -a "$STAGE/apps/."  %{buildroot}/
find %{buildroot}/usr/dt -name '*.la' -delete

install -d -m 0755 %{buildroot}/etc/ld.so.conf.d
printf '/usr/dt/lib\n' > %{buildroot}/etc/ld.so.conf.d/ck-core-cde.conf

install -d -m 0755 %{buildroot}/etc/profile.d
cat > %{buildroot}/etc/profile.d/ck-core-cde.sh <<'EOF'
case ":${PATH}:" in *:/usr/dt/bin:*) ;; *) export PATH="${PATH}:/usr/dt/bin" ;; esac
EOF

DEB_META="%{_builddir}/ck-core-cde-%{cde_ver}/cde/debian"
install -D -m 0644 "$DEB_META/cde.desktop" %{buildroot}/usr/share/xsessions/cde.desktop
install -D -m 0644 "$DEB_META/cde-wayland.desktop" %{buildroot}/usr/share/wayland-sessions/cde-wayland.desktop
install -D -m 0644 "$DEB_META/cde-portals.conf" %{buildroot}/usr/share/xdg-desktop-portal/cde-portals.conf
install -D -m 0644 "%{_builddir}/ck-core-cde-%{cde_ver}/cde/programs/dtportal/cde.portal" \
  %{buildroot}/usr/share/xdg-desktop-portal/portals/cde.portal
install -D -m 0755 "$DEB_META/Xsession.wayland" %{buildroot}/usr/dt/bin/Xsession.wayland
install -D -m 0644 "$DEB_META/rpc-cmsd.service" %{buildroot}%{_unitdir}/rpc-cmsd.service
install -D -m 0644 "$DEB_META/dtlogin.service" %{buildroot}%{_unitdir}/dtlogin.service
sed -i '/ExecStartPre=/d' %{buildroot}%{_unitdir}/dtlogin.service

install -D -m 0644 /dev/stdin %{buildroot}/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.cde.service <<'EOF'
[D-BUS Service]
Name=org.freedesktop.impl.portal.desktop.cde
Exec=/usr/dt/bin/xdg-desktop-portal-cde
SystemdService=xdg-desktop-portal-cde.service
EOF

install -D -m 0644 /dev/stdin %{buildroot}%{_userunitdir}/xdg-desktop-portal-cde.service <<'EOF'
[Unit]
Description=CK Core CDE XDG Desktop Portal Backend (Native Motif)
PartOf=graphical-session.target

[Service]
Type=dbus
BusName=org.freedesktop.impl.portal.desktop.cde
ExecStart=/usr/dt/bin/xdg-desktop-portal-cde
Restart=on-failure
EOF
%endif

# Distro-native PAM configurations for Fedora/RHEL vs openSUSE
install -d -m 0755 %{buildroot}/etc/pam.d
%if 0%{?suse_version}
cat > %{buildroot}/etc/pam.d/dtlogin <<'EOF'
#%PAM-1.0
auth     include  common-auth
account  include  common-account
password include  common-password
session  required pam_loginuid.so
session  include  common-session
-session optional pam_systemd.so
EOF
cat > %{buildroot}/etc/pam.d/dtsession <<'EOF'
#%PAM-1.0
auth     include  common-auth
account  include  common-account
EOF
%else
cat > %{buildroot}/etc/pam.d/dtlogin <<'EOF'
#%PAM-1.0
auth       substack     password-auth
auth       include      postlogin
account    required     pam_nologin.so
account    include      password-auth
password   include      password-auth
session    required     pam_selinux.so close
session    required     pam_loginuid.so
session    required     pam_selinux.so open
session    optional     pam_keyinit.so force revoke
session    include      password-auth
session    include      postlogin
-session   optional     pam_systemd.so
EOF
cat > %{buildroot}/etc/pam.d/dtsession <<'EOF'
#%PAM-1.0
auth       substack     password-auth
account    include      password-auth
EOF
%endif

cat > %{buildroot}/etc/pam.d/dtlogin-greeter <<'EOF'
#%PAM-1.0
auth       sufficient   pam_permit.so
account    sufficient   pam_permit.so
-session   optional     pam_systemd.so class=greeter
session    required     pam_unix.so
EOF

%post -n ck-core-motif9
/sbin/ldconfig || :

%postun -n ck-core-motif9
/sbin/ldconfig || :

%post -n ck-core-fs-lib
/sbin/ldconfig || :

%postun -n ck-core-fs-lib
/sbin/ldconfig || :

%post
/sbin/ldconfig || :
install -d -m 1777 /var/spool/calendar || :
%systemd_post rpc-cmsd.service
if [ $1 -eq 1 ] && [ -d /run/systemd/system ]; then
    systemctl enable --now rpc-cmsd.service >/dev/null 2>&1 || :
fi

%preun
%systemd_preun rpc-cmsd.service

%postun
/sbin/ldconfig || :
%systemd_postun_with_restart rpc-cmsd.service

%pre -n ck-core-dtlogin
getent group _dtgreeter >/dev/null 2>&1 || groupadd -r _dtgreeter || :
if ! getent passwd _dtgreeter >/dev/null 2>&1; then
    useradd -r -g _dtgreeter -d /var/dt -s /sbin/nologin \
      -c "CK Core CDE Wayland Greeter" _dtgreeter || :
fi
for grp in video render input; do
    if getent group "$grp" >/dev/null 2>&1; then
        usermod -a -G "$grp" _dtgreeter >/dev/null 2>&1 || :
    fi
done

%post -n ck-core-dtlogin
%systemd_post dtlogin.service

%preun -n ck-core-dtlogin
%systemd_preun dtlogin.service

%postun -n ck-core-dtlogin
%systemd_postun dtlogin.service

%files -n ck-core-motif9 -f %{_builddir}/files-motif9.list
%config(noreplace) /etc/ld.so.conf.d/ck-core-cde.conf

%files -n ck-core-fs-lib -f %{_builddir}/files-fs-lib.list

%files -f %{_builddir}/files-cde.list
%config(noreplace) /etc/pam.d/dtsession
%config(noreplace) /etc/profile.d/ck-core-cde.sh
/usr/share/xsessions/cde.desktop
/usr/share/wayland-sessions/cde-wayland.desktop
/usr/share/xdg-desktop-portal/cde-portals.conf
/usr/share/xdg-desktop-portal/portals/cde.portal
/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.cde.service
%{_userunitdir}/xdg-desktop-portal-cde.service
%{_unitdir}/rpc-cmsd.service

%files -n ck-core-dtlogin
/usr/dt/bin/dtlogin
/usr/dt/bin/dtgreet
/usr/dt/bin/dtchooser
%config(noreplace) /etc/pam.d/dtlogin
%config(noreplace) /etc/pam.d/dtlogin-greeter
%{_unitdir}/dtlogin.service

%files -n ck-core-apps -f %{_builddir}/files-apps.list

%changelog
* Fri Nov 27 2026 CK Core Project <cklukas@users.noreply.github.com> - 3.0.0-1
- Add cross-distro Fedora / RHEL / openSUSE RPM packaging for CK Core Motif ABI 9,
  ck-core-fs-lib, CK Core CDE (X11 and Wayland), ck-core-dtlogin, and ck-core-apps.
