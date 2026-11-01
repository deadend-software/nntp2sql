AC_PREREQ([2.69])
AC_INIT([nntp2sql],[0.50-beta],[support@example.com])
AC_CANONICAL_HOST
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_CONFIG_SRCDIR([src/export_html.c])
AC_CONFIG_HEADERS([config.h])

# Programs
AC_PROG_CC
AC_PROG_INSTALL
# Option to build viewer
AC_ARG_ENABLE([viewer], AS_HELP_STRING([--enable-viewer],[Build GTK viewer (isotope-viewer)]), [], [enable_viewer=yes])
AM_CONDITIONAL([BUILD_VIEWER],[test "x$enable_viewer" = "xyes"])

# Platform-specific icon support: the macOS Dock icon needs AppKit, a Windows .exe gets its icon from a resource.
case "$host_os" in
  darwin*) host_darwin=yes ;;
  mingw*|cygwin*|msys*) host_windows=yes ;;
esac
AM_CONDITIONAL([HOST_DARWIN],[test "x$host_darwin" = "xyes"])
AM_CONDITIONAL([HOST_WINDOWS],[test "x$host_windows" = "xyes"])
if test "x$host_windows" = "xyes"; then
  AC_CHECK_TOOL([WINDRES],[windres])
fi

# Default CFLAGS if not set
if test -z "$CFLAGS"; then
  CFLAGS="-O2 -Wall -Wextra -std=c99"
fi

# Auto-detect Homebrew prefix on macOS so headers like <mysql/mysql.h> are found
if test -d "/opt/homebrew/include"; then
  CPPFLAGS="$CPPFLAGS -I/opt/homebrew/include -I/opt/homebrew/opt/mysql/include"
  LDFLAGS="$LDFLAGS -L/opt/homebrew/lib -L/opt/homebrew/opt/mysql/lib"
fi

# Libraries (pkg-config)
PKG_CHECK_MODULES([OPENSSL],[openssl],[HAVE_OPENSSL=yes],[HAVE_OPENSSL=no])
PKG_CHECK_MODULES([SQLITE3],[sqlite3],[HAVE_SQLITE3=yes],[HAVE_SQLITE3=no])
PKG_CHECK_MODULES([MYSQL],[mysqlclient],[HAVE_MYSQL=yes],[HAVE_MYSQL=no])
PKG_CHECK_MODULES([GTK],[gtk4],[HAVE_GTK=yes],[HAVE_GTK=no])

# Optional PostgreSQL
AC_ARG_WITH([postgres], AS_HELP_STRING([--with-postgres],[Enable PostgreSQL via libpq]),[],[with_postgres=yes])
AS_IF([test "x$with_postgres" = "xyes"], [
  AC_CHECK_LIB([pq],[PQconnectdb],[HAVE_PQ=yes],[HAVE_PQ=no])
  AS_IF([test "x$HAVE_PQ" = "xyes"], [
    AC_DEFINE([HAVE_PQ],[1],[Define if libpq (PostgreSQL) is available])
    AC_SUBST([PQ_LIBS],["-lpq"])
  ], [
    AC_SUBST([PQ_LIBS],[""])
  ])
], [
  AC_SUBST([PQ_LIBS],[""])
])

# Conditionals
AM_CONDITIONAL([WITH_OPENSSL],[test "$HAVE_OPENSSL" = "yes"])
AM_CONDITIONAL([WITH_SQLITE3],[test "$HAVE_SQLITE3" = "yes"])
AM_CONDITIONAL([WITH_MYSQL],[test "$HAVE_MYSQL" = "yes"])
AM_CONDITIONAL([WITH_GTK],[test "$HAVE_GTK" = "yes"])

# Outputs
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
