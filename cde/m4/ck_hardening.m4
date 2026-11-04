dnl ck_hardening.m4 - opt-in diagnostics, runtime hardening and sanitizers.
dnl
dnl CK_HARDENING_INIT
dnl   Defines --enable-hardening and --enable-sanitizers[=LIST].  Neither
dnl   switch is on by default, so an ordinary build is unchanged.  The
dnl   compiler is probed here, but nothing reaches the compile line until
dnl   CK_HARDENING_APPLY: a sanitized runtime would make AC_RUN_IFELSE feature
dnl   tests report spurious failures, so the flags are applied after the last
dnl   feature test.
dnl
dnl   --enable-hardening
dnl     -Wall -Wextra -Wformat=2 (which includes -Wformat-security) and
dnl     -Wnull-dereference expose
dnl     null-dereference, format, truncation and unchecked-status defects.
dnl     -Wno-unused-parameter stays because Xt and Motif callback signatures
dnl     fix their parameter lists.  -fstack-protector-strong is always added;
dnl     -D_FORTIFY_SOURCE=3 only when the build is optimized, since fortified
dnl     libc calls need optimization to take effect.  Each flag is probed, so a
dnl     compiler without it (for example clang without -Wnull-dereference)
dnl     skips it.  The caller drops its own warning suppressions when
dnl     $enable_hardening is yes.
dnl
dnl   --enable-sanitizers[=LIST]
dnl     Compile and link with -fsanitize=LIST (default address,undefined) and
dnl     -fno-omit-frame-pointer.  Configure fails when the compiler cannot link
dnl     a sanitized program.  A sanitized library must be consumed by a
dnl     sanitized program: build and run both sides with the same list.
dnl
dnl CK_HARDENING_APPLY
dnl   Appends the accepted flags to CFLAGS, CXXFLAGS (when a C++ compiler is
dnl   configured) and LDFLAGS.  Call it after the last feature test.

AC_DEFUN([_CK_HARDENING_FLAG],
[AC_MSG_CHECKING([whether $CC accepts $1])
 ck_save_CFLAGS=$CFLAGS
 CFLAGS="$CFLAGS -Werror $1"
 AC_COMPILE_IFELSE([AC_LANG_PROGRAM([])],
   [ck_flag_ok=yes], [ck_flag_ok=no])
 CFLAGS=$ck_save_CFLAGS
 AC_MSG_RESULT([$ck_flag_ok])
 AS_IF([test "x$ck_flag_ok" = xyes], [$2="$$2 $1"])
])

AC_DEFUN([CK_HARDENING_INIT],
[AC_ARG_ENABLE([hardening],
  [AS_HELP_STRING([--enable-hardening],
    [build with extra warnings, stack protection and fortified libc calls (default: no)])],
  [], [enable_hardening=no])
 AS_CASE([$enable_hardening], [yes|no], [],
   [AC_MSG_ERROR([--enable-hardening expects yes or no])])
 AC_ARG_ENABLE([sanitizers],
  [AS_HELP_STRING([--enable-sanitizers@<:@=LIST@:>@],
    [build and link with -fsanitize=LIST (default list: address,undefined)])],
  [], [enable_sanitizers=no])

 ck_hardening_flags=
 AS_IF([test "x$enable_hardening" = xyes], [
   _CK_HARDENING_FLAG([-Wall], [ck_hardening_flags])
   _CK_HARDENING_FLAG([-Wextra], [ck_hardening_flags])
   _CK_HARDENING_FLAG([-Wno-unused-parameter], [ck_hardening_flags])
   _CK_HARDENING_FLAG([-Wformat=2], [ck_hardening_flags])
   _CK_HARDENING_FLAG([-Wnull-dereference], [ck_hardening_flags])
   _CK_HARDENING_FLAG([-fstack-protector-strong], [ck_hardening_flags])
   AS_CASE([" $CFLAGS "],
     [*" -O1 "*|*" -O2 "*|*" -O3 "*|*" -Os "*|*" -Og "*|*" -Ofast "*],
       [_CK_HARDENING_FLAG([-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3],
          [ck_hardening_flags])],
       [AC_MSG_NOTICE([the build is not optimized; skipping -D_FORTIFY_SOURCE=3])])
 ])

 ck_sanitize_list=
 AS_CASE([$enable_sanitizers],
   [no], [],
   [yes], [ck_sanitize_list=address,undefined],
   [ck_sanitize_list=$enable_sanitizers])
 AS_IF([test -n "$ck_sanitize_list"], [
   AC_MSG_CHECKING([whether $CC links with -fsanitize=$ck_sanitize_list])
   ck_save_CFLAGS=$CFLAGS
   ck_save_LDFLAGS=$LDFLAGS
   CFLAGS="$CFLAGS -fsanitize=$ck_sanitize_list -fno-omit-frame-pointer"
   LDFLAGS="$LDFLAGS -fsanitize=$ck_sanitize_list"
   AC_LINK_IFELSE([AC_LANG_PROGRAM([])],
     [AC_MSG_RESULT([yes])],
     [AC_MSG_RESULT([no])
      AC_MSG_ERROR([the C compiler cannot build and link with -fsanitize=$ck_sanitize_list])])
   CFLAGS=$ck_save_CFLAGS
   LDFLAGS=$ck_save_LDFLAGS
 ])
])

AC_DEFUN([CK_HARDENING_APPLY],
[AS_IF([test -n "$ck_hardening_flags"], [
   CFLAGS="$CFLAGS$ck_hardening_flags"
   AS_IF([test -n "$CXX"], [CXXFLAGS="$CXXFLAGS$ck_hardening_flags"])
   AC_MSG_NOTICE([hardening flags:$ck_hardening_flags])
 ])
 AS_IF([test -n "$ck_sanitize_list"], [
   CFLAGS="$CFLAGS -fsanitize=$ck_sanitize_list -fno-omit-frame-pointer"
   AS_IF([test -n "$CXX"],
     [CXXFLAGS="$CXXFLAGS -fsanitize=$ck_sanitize_list -fno-omit-frame-pointer"])
   LDFLAGS="$LDFLAGS -fsanitize=$ck_sanitize_list"
   AC_MSG_NOTICE([sanitizers: -fsanitize=$ck_sanitize_list])
 ])
])
