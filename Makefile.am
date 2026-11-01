SUBDIRS = .

bin_PROGRAMS = nntp2sql
if BUILD_VIEWER
bin_PROGRAMS += isotope-viewer hydrogen-group-viewer
endif

nntp2sql_SOURCES = main.c src/export_html.c
nntp2sql_LDADD = @OPENSSL_LIBS@ @SQLITE3_LIBS@ @MYSQL_LIBS@ @PQ_LIBS@
nntp2sql_CFLAGS = @OPENSSL_CFLAGS@ @SQLITE3_CFLAGS@ @MYSQL_CFLAGS@

# Ensure headers in src/ are found
AM_CPPFLAGS = -Isrc

isotope_viewer_SOURCES = gui/viewer.c
isotope_viewer_LDADD = @SQLITE3_LIBS@ @GTK_LIBS@ @MYSQL_LIBS@ @PQ_LIBS@ -lz $(ISOTOPE_ICON_OBJ)
isotope_viewer_LDFLAGS = $(GUI_EXTRA_LINK)
isotope_viewer_CFLAGS = @SQLITE3_CFLAGS@ @GTK_CFLAGS@ @MYSQL_CFLAGS@ -DICON_INSTALL_DIR='"$(datadir)/icons"'

hydrogen_group_viewer_SOURCES = gui/group_viewer.c
hydrogen_group_viewer_LDADD = @SQLITE3_LIBS@ @GTK_LIBS@ @MYSQL_LIBS@ @PQ_LIBS@ $(HYDROGEN_ICON_OBJ)
hydrogen_group_viewer_LDFLAGS = $(GUI_EXTRA_LINK)
hydrogen_group_viewer_CFLAGS = @SQLITE3_CFLAGS@ @GTK_CFLAGS@ @MYSQL_CFLAGS@ -DICON_INSTALL_DIR='"$(datadir)/icons"'

# ---- application icons ------------------------------------------------------------------------------
# Each GUI program has its own icon, named like the program.
# macOS: the Dock icon is set at run time through the Objective-C runtime (see gui/icon.h).
# Windows: windres compiles icons/<program>.rc into each .exe so Explorer and the taskbar show its icon.
GUI_EXTRA_LINK =
ISOTOPE_ICON_OBJ =
HYDROGEN_ICON_OBJ =
if HOST_DARWIN
GUI_EXTRA_LINK += -framework AppKit -lobjc
endif
if HOST_WINDOWS
ISOTOPE_ICON_OBJ += icons/isotope-viewer_rc.o
HYDROGEN_ICON_OBJ += icons/hydrogen-group-viewer_rc.o
icons/isotope-viewer_rc.o: icons/isotope-viewer.rc icons/isotope-viewer.ico
	$(WINDRES) -I $(srcdir)/icons $(srcdir)/icons/isotope-viewer.rc -o $@
icons/hydrogen-group-viewer_rc.o: icons/hydrogen-group-viewer.rc icons/hydrogen-group-viewer.ico
	$(WINDRES) -I $(srcdir)/icons $(srcdir)/icons/hydrogen-group-viewer.rc -o $@
CLEANFILES = icons/isotope-viewer_rc.o icons/hydrogen-group-viewer_rc.o
endif

# Linux / BSD: icon theme files and desktop entries (also installed elsewhere; harmless and used by packagers).
if BUILD_VIEWER
icon16dir = $(datadir)/icons/hicolor/16x16/apps
icon22dir = $(datadir)/icons/hicolor/22x22/apps
icon24dir = $(datadir)/icons/hicolor/24x24/apps
icon32dir = $(datadir)/icons/hicolor/32x32/apps
icon48dir = $(datadir)/icons/hicolor/48x48/apps
icon64dir = $(datadir)/icons/hicolor/64x64/apps
icon128dir = $(datadir)/icons/hicolor/128x128/apps
icon256dir = $(datadir)/icons/hicolor/256x256/apps
icon512dir = $(datadir)/icons/hicolor/512x512/apps
iconsvgdir = $(datadir)/icons/hicolor/scalable/apps
dist_icon16_DATA = icons/hicolor/16x16/apps/isotope-viewer.png icons/hicolor/16x16/apps/hydrogen-group-viewer.png
dist_icon22_DATA = icons/hicolor/22x22/apps/isotope-viewer.png icons/hicolor/22x22/apps/hydrogen-group-viewer.png
dist_icon24_DATA = icons/hicolor/24x24/apps/isotope-viewer.png icons/hicolor/24x24/apps/hydrogen-group-viewer.png
dist_icon32_DATA = icons/hicolor/32x32/apps/isotope-viewer.png icons/hicolor/32x32/apps/hydrogen-group-viewer.png
dist_icon48_DATA = icons/hicolor/48x48/apps/isotope-viewer.png icons/hicolor/48x48/apps/hydrogen-group-viewer.png
dist_icon64_DATA = icons/hicolor/64x64/apps/isotope-viewer.png icons/hicolor/64x64/apps/hydrogen-group-viewer.png
dist_icon128_DATA = icons/hicolor/128x128/apps/isotope-viewer.png icons/hicolor/128x128/apps/hydrogen-group-viewer.png
dist_icon256_DATA = icons/hicolor/256x256/apps/isotope-viewer.png icons/hicolor/256x256/apps/hydrogen-group-viewer.png
dist_icon512_DATA = icons/hicolor/512x512/apps/isotope-viewer.png icons/hicolor/512x512/apps/hydrogen-group-viewer.png
dist_iconsvg_DATA = icons/hicolor/scalable/apps/isotope-viewer.svg icons/hicolor/scalable/apps/hydrogen-group-viewer.svg
desktopdir = $(datadir)/applications
dist_desktop_DATA = icons/isotope-viewer.desktop icons/hydrogen-group-viewer.desktop
# .icns (macOS app bundles) and .ico (Windows) for packagers.
dist_pkgdata_DATA = icons/isotope-viewer.icns icons/isotope-viewer.ico icons/hydrogen-group-viewer.icns icons/hydrogen-group-viewer.ico
install-data-hook:
	-@if test -z "$(DESTDIR)"; then gtk4-update-icon-cache -q -t -f $(datadir)/icons/hicolor 2>/dev/null || gtk-update-icon-cache -q -t -f $(datadir)/icons/hicolor 2>/dev/null || true; fi
endif

man_MANS = man/nntp2sql.1
if BUILD_VIEWER
man_MANS += man/isotope-viewer.1 man/hydrogen-group-viewer.1
endif
dist_man_MANS = man/nntp2sql.1 man/isotope-viewer.1 man/hydrogen-group-viewer.1
EXTRA_DIST = README README.md configure.ac man/nntp2sql.1 man/isotope-viewer.1 man/hydrogen-group-viewer.1 src/export_html.c src/db.h gui/viewer.c gui/group_viewer.c gui/catalog.h gui/transfer.h gui/yenc.h gui/article.h gui/icon.h icons/make-icons.sh icons/isotope-viewer.rc icons/hydrogen-group-viewer.rc
