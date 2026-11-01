/* Application icon for the GTK programs.
 *
 *  Each program has its own icon, named like the program ("isotope-viewer", "hydrogen-group-viewer").
 *
 *  Linux / BSD  GTK looks the icon up in the icon theme: installed under <prefix>/share/icons/hicolor
 *               (and named by the .desktop files), or, when run from a build folder or a relocated install,
 *               found next to the program (./icons or ../share/icons) because that folder is added to the search path.
 *  macOS        GTK has no per-window icons, so the Dock icon is set through the Objective-C runtime. An .icns for a
 *               real .app bundle is provided in icons/ as well.
 *  Windows      The window icon comes from the same theme lookup (<prefix>/share/icons); the .exe's own icon is
 *               compiled in from icons/<program>.rc (see Makefile.am).
 */
#ifndef VIEWER_ICON_H
#define VIEWER_ICON_H
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <mach-o/dyld.h>
#endif
#ifdef G_OS_WIN32
#include <windows.h>
#endif

/* Call before gtk_init(): names the program so desktops match it to its .desktop file and icon. */
static void app_icon_prepare(const char *program_name, const char *display_name) {
    g_set_prgname(program_name);
    g_set_application_name(display_name);
}

/* Directory holding the running program, or NULL. */
static char *app_icon_exe_dir(const char *argv0) {
    char *path = NULL;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *buf = g_malloc(size + 1);
    if (_NSGetExecutablePath(buf, &size) == 0) { char *real = realpath(buf, NULL); path = real ? g_strdup(real) : g_strdup(buf); free(real); }
    g_free(buf);
#elif defined(G_OS_WIN32)
    wchar_t wide[MAX_PATH];
    if (GetModuleFileNameW(NULL, wide, MAX_PATH)) path = g_utf16_to_utf8((const gunichar2 *)wide, -1, NULL, NULL, NULL);
#else
    path = g_file_read_link("/proc/self/exe", NULL);            /* Linux */
    if (!path) path = g_file_read_link("/proc/curproc/file", NULL); /* FreeBSD with procfs */
    if (!path && argv0) {
        char *found = strchr(argv0, '/') ? g_strdup(argv0) : g_find_program_in_path(argv0);
        if (found) { char *real = realpath(found, NULL); path = real ? g_strdup(real) : g_strdup(found); free(real); g_free(found); }
    }
#endif
    char *dir = path ? g_path_get_dirname(path) : NULL;
    g_free(path);
    return dir;
}

#ifdef __APPLE__
/* The Dock shows NSApplication.applicationIconImage; a bare executable has no bundle icon, so set it here. */
static void app_icon_set_dock_icon(const char *png_path) {
    id (*send)(id, SEL, ...) = (id (*)(id, SEL, ...))objc_msgSend;
    Class application = objc_getClass("NSApplication"), image_class = objc_getClass("NSImage"), string_class = objc_getClass("NSString");
    if (!application || !image_class || !string_class) return;
    id app = send((id)application, sel_registerName("sharedApplication"));
    id path = ((id (*)(id, SEL, const char *))objc_msgSend)((id)string_class, sel_registerName("stringWithUTF8String:"), png_path);
    id image = send((id)image_class, sel_registerName("alloc"));
    image = ((id (*)(id, SEL, id))objc_msgSend)(image, sel_registerName("initWithContentsOfFile:"), path);
    if (app && image) ((void (*)(id, SEL, id))objc_msgSend)(app, sel_registerName("setApplicationIconImage:"), image);
}
#endif

/* Call after gtk_init(): makes icon_name the default icon of every window and, on macOS, the Dock icon. */
static void app_icon_setup(const char *argv0, const char *icon_name) {
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
    char *exe_dir = app_icon_exe_dir(argv0);
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    if (exe_dir) {
        g_ptr_array_add(roots, g_build_filename(exe_dir, "icons", NULL));                  /* build folder: ./icons */
        g_ptr_array_add(roots, g_build_filename(exe_dir, "..", "share", "icons", NULL));   /* relocated install */
        g_ptr_array_add(roots, g_build_filename(exe_dir, "share", "icons", NULL));         /* Windows: <prefix>/bin/share? kept for bundles */
    }
#ifdef ICON_INSTALL_DIR
    g_ptr_array_add(roots, g_strdup(ICON_INSTALL_DIR));                                    /* the configured install prefix */
#endif
    char *dock_png = NULL;
    for (guint i = 0; i < roots->len; ++i) {
        const char *root = g_ptr_array_index(roots, i);
        char *hicolor = g_build_filename(root, "hicolor", NULL);
        if (g_file_test(hicolor, G_FILE_TEST_IS_DIR)) {
            gtk_icon_theme_add_search_path(theme, root);
            if (!dock_png) {
                char *png_name = g_strconcat(icon_name, ".png", NULL);
                char *candidate = g_build_filename(hicolor, "512x512", "apps", png_name, NULL);
                g_free(png_name);
                if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) dock_png = candidate; else g_free(candidate);
            }
        }
        g_free(hicolor);
    }
    gtk_window_set_default_icon_name(icon_name);
#ifdef __APPLE__
    if (dock_png) app_icon_set_dock_icon(dock_png);
#endif
    g_free(dock_png); g_ptr_array_unref(roots); g_free(exe_dir);
}
#endif
