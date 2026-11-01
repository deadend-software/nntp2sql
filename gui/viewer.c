#include <gtk/gtk.h>
#include "transfer.h"
#include "article.h"
#include "icon.h"

#define VIEWER_TYPE_ROW (viewer_row_get_type())
G_DECLARE_FINAL_TYPE(ViewerRow, viewer_row, VIEWER, ROW, GObject)
struct _ViewerRow {
    GObject parent_instance;
    NewsFile *file;
    NewsArticle *article;
    guint depth; /* reply depth when shown threaded */
};
G_DEFINE_TYPE(ViewerRow, viewer_row, G_TYPE_OBJECT)
static void viewer_row_init(ViewerRow *self) { (void)self; }
static void viewer_row_class_init(ViewerRowClass *klass) { (void)klass; }

typedef struct App App;
typedef struct {
    App *app;
    GPtrArray *files; /* borrowed NewsFile* pointers for single or queued batch download */
    NewsArticle *article;
    TransferSettings settings;
    TransferProgress progress;
    gint64 started;
    gboolean view_image, save_copy; /* open in the image viewer; also keep a copy in save_dir */
    char *temp_dir, *save_dir;
} DownloadRequest;

struct App {
    GtkWindow *window;
    GtkWidget *connection, *dbtype, *db_dropdown, *detect_dbs, *load, *search, *mode, *hide_incomplete, *group_dropdown, *host, *port, *user, *password;
    GtkWidget *security, *workers, *directory, *browse, *download, *download_to, *read, *cancel_button;
    GtkWidget *queue_selected, *check_all, *clear_queue, *view_image, *auto_text, *save_images;
    GtkWidget *summary, *progress_label, *progress_bar, *details;
    GtkStringList *db_list, *group_list;
    GListStore *store;
    GtkMultiSelection *selection;
    NewsCatalog *catalog;
    GCancellable *cancel;
    DownloadRequest *active;
    gboolean busy, closing, updating_dbs, updating_groups, binding_row;
    guint current_dbtype;
    char *conn_by_type[3];
    GMainLoop *loop;
    char *preferences;
    char *config_db; /* database whose stored nntp2sql settings were last applied */
    GPtrArray *all_groups; /* names of every group that has articles; the articles load per group */
    char *loaded_group;    /* group whose articles are loaded; "*" = every group; NULL = none yet */
    GtkWidget *hide_text;
    char *post_from; /* "Name <addr>" used when replying */
};

static const char *backend_type_name(guint idx) {
    static const char *types[] = {"sqlite", "mysql", "postgres"};
    return types[idx <= 2 ? idx : 0];
}

static void status(App *app, const char *message) {
    gtk_label_set_text(GTK_LABEL(app->summary), message);
}
static const char *entry_text(GtkWidget *entry) {
    return gtk_editable_get_text(GTK_EDITABLE(entry));
}
static void set_busy(App *app, gboolean busy) {
    app->busy = busy;
    gtk_widget_set_sensitive(app->load, !busy);
    if (app->detect_dbs) gtk_widget_set_sensitive(app->detect_dbs, !busy);
    gtk_widget_set_sensitive(app->download, !busy);
    if (app->view_image) gtk_widget_set_sensitive(app->view_image, !busy);
    gtk_widget_set_sensitive(app->download_to, !busy);
    gtk_widget_set_sensitive(app->read, !busy);
    gtk_widget_set_sensitive(app->queue_selected, !busy);
    gtk_widget_set_sensitive(app->check_all, !busy);
    gtk_widget_set_sensitive(app->clear_queue, !busy);
    gtk_widget_set_sensitive(app->cancel_button, busy);
}
/* View modes: 0 = yEnc files, 1 = all articles, 2 = text articles only (no yEnc marker in the subject). */
static guint view_mode(App *app) { return gtk_drop_down_get_selected(GTK_DROP_DOWN(app->mode)); }
static const char *mode_noun(guint mode) { return mode == 0 ? "candidate yEnc files" : mode == 2 ? "text articles" : "articles"; }

/* A dropdown must never change because the wheel or trackpad scrolled over it: a stray scroll flipped the
 * Group filter to a single group and silently hid every other article. Swallow scroll events on them. */
static gboolean swallow_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer d) { (void)c; (void)dx; (void)dy; (void)d; return TRUE; }
static void block_scroll_changes(GtkWidget *dropdown) {
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(swallow_scroll), NULL);
    gtk_widget_add_controller(dropdown, scroll);
}

static void refresh_rows(App *app);
static void selection_changed(GtkSelectionModel *model, guint position, guint n_items, gpointer data);
static void start_transfer(App *app, const char *directory, gboolean read_article);
static void start_discover_dbs(App *app, gboolean auto_select);
static void start_load(GtkButton *button, gpointer data);
static void start_load_request(App *app, const char *group, gboolean groups_only);

static guint count_queued_files(App *app) {
    if (!app->catalog) return 0;
    guint count = 0;
    for (guint i = 0; i < app->catalog->files->len; ++i) {
        NewsFile *f = g_ptr_array_index(app->catalog->files, i);
        if (f->queued) count++;
    }
    return count;
}

static void save_preferences(App *app) {
    guint cur = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    if (cur <= 2 && *entry_text(app->connection)) {
        g_free(app->conn_by_type[cur]);
        app->conn_by_type[cur] = g_strdup(entry_text(app->connection));
    }
    GKeyFile *key = g_key_file_new();
    g_key_file_set_string(key, "viewer", "folder", entry_text(app->directory));
    g_key_file_set_string(key, "viewer", "host", entry_text(app->host));
    g_key_file_set_string(key, "viewer", "user", entry_text(app->user));
    g_key_file_set_integer(key, "viewer", "port", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->port)));
    g_key_file_set_integer(key, "viewer", "workers", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->workers)));
    g_key_file_set_integer(key, "viewer", "security", gtk_drop_down_get_selected(GTK_DROP_DOWN(app->security)));
    g_key_file_set_integer(key, "viewer", "dbtype", (gint)cur);
    g_key_file_set_boolean(key, "viewer", "auto_text", gtk_check_button_get_active(GTK_CHECK_BUTTON(app->auto_text)));
    g_key_file_set_boolean(key, "viewer", "save_images", gtk_check_button_get_active(GTK_CHECK_BUTTON(app->save_images)));
    if (app->post_from && *app->post_from) g_key_file_set_string(key, "viewer", "post_from", app->post_from);
    g_key_file_set_boolean(key, "viewer", "hide_text", gtk_check_button_get_active(GTK_CHECK_BUTTON(app->hide_text)));
    g_key_file_set_boolean(key, "viewer", "hide_incomplete", gtk_check_button_get_active(GTK_CHECK_BUTTON(app->hide_incomplete)));
    const char *conn_keys[3] = {"conn_sqlite", "conn_mysql", "conn_postgres"};
    for (int i = 0; i < 3; ++i) {
        if (app->conn_by_type[i] && *app->conn_by_type[i])
            g_key_file_set_string(key, "viewer", conn_keys[i], app->conn_by_type[i]);
    }
    char *parent = g_path_get_dirname(app->preferences);
    GError *error = NULL;
    if (g_mkdir_with_parents(parent, 0700) < 0 || !g_key_file_save_to_file(key, app->preferences, &error)) {
        status(app, error ? error->message : "Unable to save preferences");
        g_clear_error(&error);
    }
    g_free(parent); g_key_file_unref(key);
}
static void load_preferences(App *app) {
    GKeyFile *key = g_key_file_new();
    if (g_key_file_load_from_file(key, app->preferences, G_KEY_FILE_NONE, NULL)) {
        GtkWidget *entries[] = {app->directory, app->host, app->user};
        const char *keys[] = {"folder", "host", "user"};
        for (int i = 0; i < 3; ++i) {
            char *value = g_key_file_get_string(key, "viewer", keys[i], NULL);
            if (value) gtk_editable_set_text(GTK_EDITABLE(entries[i]), value);
            g_free(value);
        }
        const char *conn_keys[3] = {"conn_sqlite", "conn_mysql", "conn_postgres"};
        for (int i = 0; i < 3; ++i) {
            char *cval = g_key_file_get_string(key, "viewer", conn_keys[i], NULL);
            if (cval && *cval) {
                g_free(app->conn_by_type[i]);
                app->conn_by_type[i] = cval;
            } else {
                g_free(cval);
            }
        }
        gint port = g_key_file_get_integer(key, "viewer", "port", NULL);
        gint workers = g_key_file_get_integer(key, "viewer", "workers", NULL);
        gint security = g_key_file_get_integer(key, "viewer", "security", NULL);
        gint dbtype = g_key_file_get_integer(key, "viewer", "dbtype", NULL);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(app->auto_text), g_key_file_get_boolean(key, "viewer", "auto_text", NULL));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(app->save_images), g_key_file_get_boolean(key, "viewer", "save_images", NULL));
        g_free(app->post_from); app->post_from = g_key_file_get_string(key, "viewer", "post_from", NULL);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(app->hide_text), g_key_file_get_boolean(key, "viewer", "hide_text", NULL));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(app->hide_incomplete),
                                    g_key_file_get_boolean(key, "viewer", "hide_incomplete", NULL));
        if (port > 0 && port <= 65535) gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->port), port);
        if (workers > 0 && workers <= 64) gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->workers), workers);
        if (security >= 0 && security <= 2) gtk_drop_down_set_selected(GTK_DROP_DOWN(app->security), (guint)security);
        if (dbtype >= 0 && dbtype <= 2) {
            app->current_dbtype = (guint)dbtype;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(app->dbtype), (guint)dbtype);
        }
    }
    g_key_file_unref(key);
}
/* Defaults from the file `nntp2sql --save-config` writes ($XDG_CONFIG_HOME or ~/.config/nntp2sql/nntp2sql.conf).
 * When present it supplies the server, login, security, workers and database connection at launch,
 * taking precedence over the viewer's own remembered values. Returns TRUE if the file was applied. */
static gboolean apply_nntp2sql_config(App *app) {
    char *path = g_build_filename(g_get_user_config_dir(), "nntp2sql", "nntp2sql.conf", NULL);
    char *contents = NULL;
    gboolean found = g_file_get_contents(path, &contents, NULL, NULL);
    g_free(path);
    if (!found) return FALSE;
    GHashTable *kv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char **lines = g_strsplit(contents, "\n", -1);
    for (char **l = lines; *l; ++l) {
        g_strstrip(*l);
        if (!**l || **l == '#') continue;
        char *eq = strchr(*l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = g_ascii_strdown(g_strstrip(*l), -1);
        g_hash_table_replace(kv, key, g_strdup(g_strstrip(eq + 1))); /* values are kept verbatim; no key is mandatory */
    }
    g_strfreev(lines); g_free(contents);
#define CONF(k) ((const char *)g_hash_table_lookup(kv, k))
    const char *host = CONF("host"), *port = CONF("port"), *user = CONF("user"), *pass = CONF("pass");
    if (host && *host) gtk_editable_set_text(GTK_EDITABLE(app->host), host);
    if (user) gtk_editable_set_text(GTK_EDITABLE(app->user), user);
    if (pass) gtk_editable_set_text(GTK_EDITABLE(app->password), pass);
    if (port && atoi(port) > 0 && atoi(port) <= 65535) gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->port), atoi(port));
    if (CONF("ssl") || CONF("starttls")) {
        gboolean ssl = CONF("ssl") && atoi(CONF("ssl")), starttls = CONF("starttls") && atoi(CONF("starttls"));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(app->security), ssl ? 0 : starttls ? 1 : 2);
    }
    if (CONF("threads") && atoi(CONF("threads")) >= 1 && atoi(CONF("threads")) <= 64)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->workers), atoi(CONF("threads")));

    const char *dbtype = CONF("db_type"), *dbname = CONF("db_name");
    if (dbtype && *dbtype && dbname && *dbname) {
        guint type = !g_ascii_strcasecmp(dbtype, "sqlite") ? 0 :
                     (!g_ascii_strcasecmp(dbtype, "mysql") || !g_ascii_strcasecmp(dbtype, "mariadb")) ? 1 : 2;
        char *conn;
        if (type == 0) conn = g_strdup(dbname);
        else {
            const char *dh = CONF("db_host"), *dp = CONF("db_port"), *du = CONF("db_user"), *dw = CONF("db_pass");
            const char *def_user = type == 1 ? "root" : "postgres";
            if (dw && strchr(dw, ',')) /* a comma would break the comma-separated form */
                conn = g_strdup_printf("host=%s dbname=%s user=%s password=%s port=%s", dh && *dh ? dh : "127.0.0.1", dbname,
                                       du && *du ? du : def_user, dw, dp && *dp ? dp : (type == 1 ? "3306" : "5432"));
            else
                conn = g_strdup_printf("%s,%s,%s,%s,%s", dh && *dh ? dh : "127.0.0.1", dbname, du && *du ? du : def_user,
                                       dw ? dw : "", dp && *dp ? dp : (type == 1 ? "3306" : "5432"));
        }
        g_free(app->conn_by_type[type]); app->conn_by_type[type] = conn;
        app->current_dbtype = type;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(app->dbtype), type);
    }
#undef CONF
    g_hash_table_destroy(kv);
    return TRUE;
}
static void set_row_queued(ViewerRow *row, gboolean queued) {
    if (!row) return;
    if (row->file) row->file->queued = queued;
    if (row->article) {
        row->article->queued = queued;
        if (row->article->file) row->article->file->queued = queued;
    }
}
static gboolean get_row_queued(ViewerRow *row) {
    if (!row) return FALSE;
    if (row->file) return row->file->queued;
    if (row->article) return row->article->file ? row->article->file->queued : row->article->queued;
    return FALSE;
}
static void row_check_toggled(GtkCheckButton *check, gpointer data) {
    App *app = data;
    if (app->binding_row) return;
    GtkListItem *item = g_object_get_data(G_OBJECT(check), "list-item");
    if (!item) return;
    ViewerRow *row = VIEWER_ROW(gtk_list_item_get_item(item));
    if (!row) return;
    gboolean active = gtk_check_button_get_active(check);
    set_row_queued(row, active);
    guint pos = gtk_list_item_get_position(item);
    if (pos != GTK_INVALID_LIST_POSITION &&
        gtk_selection_model_is_selected(GTK_SELECTION_MODEL(app->selection), pos)) {
        GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(app->selection));
        if (gtk_bitset_get_size(selected) > 1) {
            GtkBitsetIter iter;
            guint idx = 0;
            if (gtk_bitset_iter_init_first(&iter, selected, &idx)) {
                do {
                    ViewerRow *sel_row = g_list_model_get_item(G_LIST_MODEL(app->store), idx);
                    if (sel_row) {
                        set_row_queued(sel_row, active);
                        g_object_unref(sel_row);
                    }
                } while (gtk_bitset_iter_next(&iter, &idx));
            }
            gtk_bitset_unref(selected);
            refresh_rows(app);
            return;
        }
        gtk_bitset_unref(selected);
    }
    guint qcount = count_queued_files(app);
    char *msg = g_strdup_printf("%u yEnc file(s) queued for download.", qcount);
    status(app, msg);
    g_free(msg);
    selection_changed(GTK_SELECTION_MODEL(app->selection), 0, 0, app);
}
static void row_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory;
    App *app = data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *check = gtk_check_button_new();
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(check, "Queue file for download");
    GtkWidget *icon = gtk_image_new();
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *title = gtk_label_new(""), *subtitle = gtk_label_new("");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_widget_set_hexpand(labels, TRUE);
    gtk_widget_set_margin_top(box, 6); gtk_widget_set_margin_bottom(box, 6);
    gtk_box_append(GTK_BOX(labels), title); gtk_box_append(GTK_BOX(labels), subtitle);
    gtk_box_append(GTK_BOX(box), check); gtk_box_append(GTK_BOX(box), icon); gtk_box_append(GTK_BOX(box), labels);
    gtk_list_item_set_child(item, box);
    g_object_set_data(G_OBJECT(check), "list-item", item);
    g_signal_connect(check, "toggled", G_CALLBACK(row_check_toggled), app);
    g_object_set_data(G_OBJECT(item), "check", check);
    g_object_set_data(G_OBJECT(item), "icon", icon);
    g_object_set_data(G_OBJECT(item), "title", title);
    g_object_set_data(G_OBJECT(item), "subtitle", subtitle);
}
static void row_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
    (void)factory;
    App *app = data;
    ViewerRow *row = VIEWER_ROW(gtk_list_item_get_item(item));
    NewsFile *file = row->file;
    NewsArticle *article = row->article;
    GtkWidget *check = g_object_get_data(G_OBJECT(item), "check");
    app->binding_row = TRUE;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), get_row_queued(row));
    app->binding_row = FALSE;
    char *subtitle;
    if (file) {
        char *state = file_state(file);
        const char *qtag = file->queued ? "  •  [Queued]" : "";
        if (file->elapsed > 0)
            subtitle = g_strdup_printf("%s  •  %s  •  %s  •  %.2f s%s", file->group, file->author, state, file->elapsed, qtag);
        else
            subtitle = g_strdup_printf("%s  •  %s  •  %s%s", file->group, file->author, state, qtag);
        g_free(state);
    } else if (article->file) {
        char *state = file_state(article->file);
        const char *qtag = article->file->queued ? "  •  [Queued]" : "";
        subtitle = g_strdup_printf("%s  •  %s  •  %s  •  #%" G_GINT64_FORMAT "  •  %s%s",
                                   article->group, article->author, article->date, article->number, state, qtag);
        g_free(state);
    } else {
        const char *qtag = article->queued ? "  •  [Queued]" : "";
        subtitle = g_strdup_printf("%s  •  %s  •  %s  •  #%" G_GINT64_FORMAT "%s",
                                   article->group, article->author, article->date, article->number, qtag);
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(g_object_get_data(G_OBJECT(item), "icon")),
                                file || article->yenc ? "package-x-generic-symbolic" : "text-x-generic-symbolic");
    gtk_widget_set_margin_start(gtk_list_item_get_child(item), (int)MIN(row->depth, 24U) * 20);
    if (!file && row->depth > 0) {
        char *indented = g_strconcat("\342\206\263 ", article->subject, NULL); /* "↳ " marks a reply */
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(item), "title")), indented);
        g_free(indented);
    } else
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(item), "title")), file ? file->title : article->subject);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(item), "subtitle")), subtitle);
    g_free(subtitle);
}
static ViewerRow *first_selected_row(App *app, guint64 *out_count) {
    GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(app->selection));
    guint64 count = gtk_bitset_get_size(selected);
    if (out_count) *out_count = count;
    ViewerRow *row = NULL;
    if (count > 0) {
        guint first_idx = gtk_bitset_get_minimum(selected);
        row = g_list_model_get_item(G_LIST_MODEL(app->store), first_idx);
    }
    gtk_bitset_unref(selected);
    return row;
}
static void selection_changed(GtkSelectionModel *model, guint position, guint n_items, gpointer data) {
    (void)model; (void)position; (void)n_items;
    App *app = data;
    guint64 sel_count = 0;
    ViewerRow *row = first_selected_row(app, &sel_count);
    if (!row) {
        if (app->details) gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->details)), "", -1);
        return;
    }
    NewsFile *file = row->file;
    NewsArticle *article = row->article;
    guint queued_total = count_queued_files(app);
    char *header_prefix = sel_count > 1 ?
        g_strdup_printf("[%" G_GUINT64_FORMAT " items selected • %u total files queued]\n\n", sel_count, queued_total) :
        g_strdup("");
    char *text;
    if (file) {
        char *state = file_state(file);
        text = g_strdup_printf("%s%s\nGroup: %s\nAuthor: %s\nQueued: %s\n%s\nDownload time: %.2f seconds\nSaved file: %s\n\nFile grouping is inferred from subjects. Body sizes, ranges, filenames and checksums are verified during download.",
            header_prefix, file->title, file->group, file->author, file->queued ? "Yes" : "No",
            state, file->elapsed, file->saved_path ? file->saved_path : "Not downloaded");
        g_free(state);
    } else if (article->file) {
        char *state = file_state(article->file);
        text = g_strdup_printf("%s%s\nFrom: %s\nDate: %s\nGroup: %s\nArticle: #%" G_GINT64_FORMAT "\nMessage-ID: %s\nQueued: %s\nFile status: %s\nDownload time: %.2f seconds\nSaved file: %s\n\nyEnc part (%u/%u) — check the box or choose Download file / queue to fetch and verify the assembled file.",
            header_prefix, article->subject, article->author, article->date, article->group, article->number, article->message,
            article->file->queued ? "Yes" : "No", state, article->file->elapsed,
            article->file->saved_path ? article->file->saved_path : "Not downloaded",
            article->part, article->total);
        g_free(state);
    } else text = g_strdup_printf("%s%s\nFrom: %s\nDate: %s\nGroup: %s\nArticle: #%" G_GINT64_FORMAT "\nMessage-ID: %s\n\nChoose Read article to fetch its body.",
        header_prefix, article->subject, article->author, article->date, article->group, article->number, article->message);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->details)), text, -1);
    g_free(header_prefix); g_free(text);
    /* Auto-load: a single selected text article fetches its body without pressing Read article. */
    gboolean text_mode = view_mode(app) == 2; /* Text articles mode always shows the body of the selected post */
    if (((app->auto_text && gtk_check_button_get_active(GTK_CHECK_BUTTON(app->auto_text))) || text_mode) && sel_count == 1 &&
        article && !article->yenc && !app->busy) {
        if (*entry_text(app->host)) start_transfer(app, NULL, TRUE);
        else if (text_mode) status(app, "Enter the NNTP server (and login) above to read text articles.");
    }
    g_object_unref(row);
}
static const char *selected_group(App *app) {
    if (!app->group_dropdown || !app->group_list) return NULL;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->group_dropdown));
    if (idx < 2 || idx == GTK_INVALID_LIST_POSITION) return NULL; /* 0: choose a group, 1: all groups */
    return gtk_string_list_get_string(app->group_list, idx);
}/* The dropdown lists "choose a group", "all groups", then every group in the database. Only the chosen
 * group's articles are held in memory. */
static void populate_groups(App *app) {
    app->updating_groups = TRUE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->group_list));
    guint count = app->all_groups ? app->all_groups->len : 0;
    const char **additions = g_new0(const char *, count + 1);
    guint target = 0;
    for (guint i = 0; i < count; ++i) {
        additions[i] = g_ptr_array_index(app->all_groups, i);
        if (app->loaded_group && !strcmp(app->loaded_group, additions[i])) target = i + 2;
    }
    if (app->loaded_group && !strcmp(app->loaded_group, "*")) target = 1;
    gtk_string_list_splice(app->group_list, 2, n > 2 ? n - 2 : 0, additions);
    g_free(additions);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->group_dropdown), target);
    app->updating_groups = FALSE;
}/* ---- threading ---- */
typedef struct { NewsArticle *article; guint depth; } ThreadItem;

/* The article this one replies to: the last Message-ID in References that is present in the set. */
static NewsArticle *thread_parent(const NewsArticle *a, GHashTable *by_id) {
    if (!a->refs || !*a->refs) return NULL;
    const char *end = a->refs + strlen(a->refs);
    while (end > a->refs) {
        const char *close = end;
        while (close > a->refs && close[-1] != '>') --close;
        if (close <= a->refs) break;
        const char *open = close - 1;
        while (open > a->refs && open[-1] != '<') --open;
        if (open <= a->refs) break;
        char *id = g_strndup(open - 1, (gsize)(close - open) + 1);
        NewsArticle *parent = g_hash_table_lookup(by_id, id);
        g_free(id);
        if (parent && parent != a) return parent;
        end = open - 1;
    }
    return NULL;
}

/* Orders 'articles' as discussion threads (each reply directly after its parent, with its depth). Replies whose
 * parent is not in the set start their own thread. Iterative, and safe against reference loops. */
static GArray *thread_order(GPtrArray *articles) {
    GHashTable *by_id = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < articles->len; ++i) {
        NewsArticle *a = g_ptr_array_index(articles, i);
        if (a->message && *a->message) g_hash_table_replace(by_id, a->message, a);
    }
    GHashTable *kids = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_ptr_array_unref);
    GPtrArray *roots = g_ptr_array_new();
    for (guint i = 0; i < articles->len; ++i) {
        NewsArticle *a = g_ptr_array_index(articles, i), *parent = thread_parent(a, by_id);
        if (!parent) { g_ptr_array_add(roots, a); continue; }
        GPtrArray *list = g_hash_table_lookup(kids, parent);
        if (!list) { list = g_ptr_array_new(); g_hash_table_insert(kids, parent, list); }
        g_ptr_array_add(list, a);
    }
    GArray *out = g_array_sized_new(FALSE, FALSE, sizeof(ThreadItem), articles->len);
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    GArray *stack = g_array_new(FALSE, FALSE, sizeof(ThreadItem));
    for (guint pass = 0; pass < 2; ++pass) {
        /* Pass 1 covers articles left over by reference loops (A replies to B, B replies to A). */
        GPtrArray *starts = roots;
        GPtrArray *leftover = NULL;
        if (pass == 1) {
            leftover = g_ptr_array_new();
            for (guint i = 0; i < articles->len; ++i)
                if (!g_hash_table_contains(seen, g_ptr_array_index(articles, i))) g_ptr_array_add(leftover, g_ptr_array_index(articles, i));
            starts = leftover;
        }
        for (guint r = starts->len; r > 0; --r) { ThreadItem it = {g_ptr_array_index(starts, r - 1), 0}; g_array_append_val(stack, it); }
        while (stack->len) {
            ThreadItem it = g_array_index(stack, ThreadItem, stack->len - 1);
            g_array_set_size(stack, stack->len - 1);
            if (g_hash_table_contains(seen, it.article)) continue;
            g_hash_table_add(seen, it.article);
            g_array_append_val(out, it);
            GPtrArray *list = g_hash_table_lookup(kids, it.article);
            for (guint k = list ? list->len : 0; k > 0; --k) {
                ThreadItem child = {g_ptr_array_index(list, k - 1), it.depth + 1};
                g_array_append_val(stack, child);
            }
        }
        if (leftover) g_ptr_array_unref(leftover);
    }
    g_array_unref(stack); g_hash_table_destroy(seen); g_ptr_array_unref(roots);
    g_hash_table_destroy(kids); g_hash_table_destroy(by_id);
    return out;
}

static void refresh_rows(App *app) {
    if (!app->catalog) {
        g_list_store_remove_all(app->store);
        return;
    }
    const char *query = entry_text(app->search);
    const char *group_filter = selected_group(app);
    gboolean files = view_mode(app) == 0, text_only = view_mode(app) == 2;
    gboolean hide_incomplete = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->hide_incomplete));
    gboolean hide_text = app->hide_text && gtk_check_button_get_active(GTK_CHECK_BUTTON(app->hide_text));
    GPtrArray *matched_text = text_only ? g_ptr_array_new() : NULL; /* text articles to be threaded below */
    GPtrArray *array = files ? app->catalog->files : app->catalog->articles;
    GPtrArray *rows = g_ptr_array_new_full(MIN(array->len, 65536U), g_object_unref);
    for (guint i = 0; i < array->len; ++i) {
        NewsFile *file = files ? g_ptr_array_index(array, i) : NULL;
        NewsArticle *article = files ? NULL : g_ptr_array_index(array, i);
        const char *item_group = files ? file->group : article->group;
        if (group_filter && g_strcmp0(item_group, group_filter) != 0) continue;
        if (text_only && article->yenc) continue;
        if (hide_text && !files && !text_only && !article->yenc) continue; /* Hide text articles */
        NewsFile *owner = files ? file : article->file;
        if (hide_incomplete && owner && !file_has_headers(owner)) continue;
        if (files ? !catalog_file_matches(file, query) : !catalog_article_matches(article, query)) continue;
        if (text_only) { g_ptr_array_add(matched_text, article); continue; }
        ViewerRow *row = g_object_new(VIEWER_TYPE_ROW, NULL);
        row->file = file;
        row->article = article;
        g_ptr_array_add(rows, row);
    }
    if (text_only) { /* threaded: each reply directly under the post it answers */
        GArray *ordered = thread_order(matched_text);
        for (guint i = 0; i < ordered->len; ++i) {
            ThreadItem *it = &g_array_index(ordered, ThreadItem, i);
            ViewerRow *row = g_object_new(VIEWER_TYPE_ROW, NULL);
            row->article = it->article; row->depth = it->depth;
            g_ptr_array_add(rows, row);
        }
        g_array_unref(ordered); g_ptr_array_unref(matched_text);
    }
    /* Single bulk splice avoids O(N) per-item signals when displaying 2M+ records. */
    guint old_len = g_list_model_get_n_items(G_LIST_MODEL(app->store));
    g_list_store_splice(app->store, 0, old_len, rows->pdata, rows->len);
    g_ptr_array_unref(rows);
    selection_changed(GTK_SELECTION_MODEL(app->selection), 0, 0, app);
}
static void search_changed(GtkEditable *entry, gpointer data) {
    (void)entry;
    App *app = data;
    refresh_rows(app);
    if (app->catalog) {
        guint shown = g_list_model_get_n_items(G_LIST_MODEL(app->store));
        char *msg = g_strdup_printf("%u %s matched search (%u queued).",
                                    shown, mode_noun(view_mode(app)),
                                    count_queued_files(app));
        status(app, msg); g_free(msg);
    }
}
static void mode_changed(GObject *object, GParamSpec *parameter, gpointer data) {
    (void)object; (void)parameter;
    App *app = data;
    gtk_widget_set_sensitive(app->hide_text, view_mode(app) != 2); /* meaningless in the text-only view */
    refresh_rows(app);
}
static void hide_incomplete_toggled(GtkCheckButton *button, gpointer data) {
    (void)button;
    App *app = data;
    search_changed(GTK_EDITABLE(app->search), app);
}
static void group_changed(GObject *object, GParamSpec *parameter, gpointer data) {
    (void)object; (void)parameter;
    App *app = data;
    if (app->updating_groups) return;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->group_dropdown));
    if (idx == 0 || idx == GTK_INVALID_LIST_POSITION) return;
    /* Choosing a group loads that group's articles from the database; "All groups" loads everything. */
    start_load_request(app, idx == 1 ? NULL : selected_group(app), FALSE);
}static void queue_selected_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (!app->catalog) return;
    GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(app->selection));
    GtkBitsetIter iter;
    guint idx = 0, affected = 0;
    if (gtk_bitset_iter_init_first(&iter, selected, &idx)) {
        do {
            ViewerRow *row = g_list_model_get_item(G_LIST_MODEL(app->store), idx);
            if (row) {
                set_row_queued(row, TRUE);
                affected++;
                g_object_unref(row);
            }
        } while (gtk_bitset_iter_next(&iter, &idx));
    }
    gtk_bitset_unref(selected);
    refresh_rows(app);
    char *msg = g_strdup_printf("Queued %u selected item(s) • %u total yEnc files in queue.", affected, count_queued_files(app));
    status(app, msg); g_free(msg);
}
static void check_all_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (!app->catalog) return;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->store));
    for (guint i = 0; i < n; ++i) {
        ViewerRow *row = g_list_model_get_item(G_LIST_MODEL(app->store), i);
        if (row) {
            set_row_queued(row, TRUE);
            g_object_unref(row);
        }
    }
    refresh_rows(app);
    char *msg = g_strdup_printf("Checked all %u shown item(s) • %u total yEnc files in queue.", n, count_queued_files(app));
    status(app, msg); g_free(msg);
}
static void clear_queue_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (!app->catalog) return;
    for (guint i = 0; i < app->catalog->files->len; ++i) {
        NewsFile *f = g_ptr_array_index(app->catalog->files, i);
        f->queued = FALSE;
    }
    for (guint i = 0; i < app->catalog->articles->len; ++i) {
        NewsArticle *a = g_ptr_array_index(app->catalog->articles, i);
        a->queued = FALSE;
    }
    refresh_rows(app);
    status(app, "Cleared download queue.");
}
static void select_db_in_dropdown(App *app, const char *db_name) {
    if (!app->db_dropdown || !app->db_list || !db_name || !*db_name) return;
    app->updating_dbs = TRUE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->db_list));
    guint found_idx = 0;
    for (guint i = 1; i < n; ++i) {
        const char *item = gtk_string_list_get_string(app->db_list, i);
        if (item && strcmp(item, db_name) == 0) {
            found_idx = i;
            break;
        }
    }
    if (found_idx == 0) {
        gtk_string_list_append(app->db_list, db_name);
        found_idx = n;
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->db_dropdown), found_idx);
    app->updating_dbs = FALSE;
}

typedef struct {
    char *type;
    char *connection;
    gboolean auto_select;
} DiscoverRequest;

static void discover_request_free(gpointer data) {
    DiscoverRequest *r = data;
    g_free(r->type);
    g_free(r->connection);
    g_free(r);
}
static void discover_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; (void)cancel;
    DiscoverRequest *r = data;
    GError *error = NULL;
    GPtrArray *dbs = catalog_discover_databases(r->type, r->connection, &error);
    if (error && dbs->len == 0) {
        g_ptr_array_unref(dbs);
        g_task_return_error(task, error);
    } else {
        g_clear_error(&error);
        g_task_return_pointer(task, dbs, (GDestroyNotify)g_ptr_array_unref);
    }
}
static void discover_finished(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    App *app = data;
    DiscoverRequest *req = g_task_get_task_data(G_TASK(result));
    GError *error = NULL;
    GPtrArray *dbs = g_task_propagate_pointer(G_TASK(result), &error);
    app->updating_dbs = TRUE;
    guint old_n = g_list_model_get_n_items(G_LIST_MODEL(app->db_list));
    if (old_n > 1) gtk_string_list_splice(app->db_list, 1, old_n - 1, NULL);

    guint cur_type = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    const char *type_name = backend_type_name(cur_type);
    const char *cur_conn = entry_text(app->connection);

    if (dbs && dbs->len > 0) {
        const char **items = g_new0(const char *, dbs->len + 1);
        for (guint i = 0; i < dbs->len; ++i) items[i] = g_ptr_array_index(dbs, i);
        gtk_string_list_splice(app->db_list, 1, 0, (const char * const *)items);
        g_free(items);

        char *cur_db = NULL;
        if (!strcmp(type_name, "sqlite")) {
            cur_db = g_strdup(cur_conn);
        } else {
            CatalogConnParams p;
            catalog_parse_server_connection(type_name, cur_conn, &p);
            cur_db = g_strdup(p.database);
            catalog_conn_params_clear(&p);
        }
        guint match_idx = 0;
        for (guint i = 0; i < dbs->len; ++i) {
            const char *d = g_ptr_array_index(dbs, i);
            if (cur_db && strcmp(cur_db, d) == 0) {
                match_idx = i + 1;
                break;
            }
        }
        if (match_idx == 0 && req && req->auto_select) {
            match_idx = 1;
            const char *first_db = g_ptr_array_index(dbs, 0);
            char *updated_conn = catalog_format_connection_for_db(type_name, cur_conn, first_db);
            gtk_editable_set_text(GTK_EDITABLE(app->connection), updated_conn);
            g_free(app->conn_by_type[cur_type]);
            app->conn_by_type[cur_type] = updated_conn;
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(app->db_dropdown), match_idx);
        g_free(cur_db);

        if (!app->catalog && !app->all_groups) {
            GString *names = g_string_new(NULL);
            for (guint i = 0; i < MIN(dbs->len, 6U); ++i) {
                if (i > 0) g_string_append(names, ", ");
                g_string_append(names, (const char *)g_ptr_array_index(dbs, i));
            }
            if (dbs->len > 6) g_string_append_printf(names, " (+%u more)", dbs->len - 6);
            char *msg = g_strdup_printf("Detected %u nntp2sql database(s) [%s] \u2014 click Open / Reload database or choose from the Database dropdown.",
                                        dbs->len, names->str);
            status(app, msg);
            g_free(msg);
            g_string_free(names, TRUE);
        }
    } else {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(app->db_dropdown), 0);
        if (error && !req->auto_select) status(app, error->message);
        else if (!req->auto_select) status(app, "No nntp2sql-schema databases detected yet on this connection.");
    }
    app->updating_dbs = FALSE;
    if (dbs) g_ptr_array_unref(dbs);
    g_clear_error(&error);
}
static void start_discover_dbs(App *app, gboolean auto_select) {
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    const char *type = backend_type_name(idx);
    const char *conn = entry_text(app->connection);
    if (!conn || !*conn) {
        conn = catalog_default_connection(type);
        gtk_editable_set_text(GTK_EDITABLE(app->connection), conn);
    }
    DiscoverRequest *r = g_new0(DiscoverRequest, 1);
    r->type = g_strdup(type);
    r->connection = g_strdup(conn);
    r->auto_select = auto_select;
    GTask *task = g_task_new(NULL, NULL, discover_finished, app);
    g_task_set_task_data(task, r, discover_request_free);
    g_task_run_in_thread(task, discover_worker);
    g_object_unref(task);
}
static void detect_dbs_clicked(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    start_discover_dbs(app, TRUE);
}
static void dbtype_changed(GObject *object, GParamSpec *parameter, gpointer data) {
    (void)object; (void)parameter;
    App *app = data;
    guint new_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    if (new_idx > 2) return;
    if (app->current_dbtype <= 2 && app->current_dbtype != new_idx) {
        const char *old_text = entry_text(app->connection);
        if (old_text && *old_text) {
            g_free(app->conn_by_type[app->current_dbtype]);
            app->conn_by_type[app->current_dbtype] = g_strdup(old_text);
        }
    }
    app->current_dbtype = new_idx;
    const char *type = backend_type_name(new_idx);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->connection), catalog_default_placeholder(type));
    const char *target_conn = (app->conn_by_type[new_idx] && *app->conn_by_type[new_idx]) ?
        app->conn_by_type[new_idx] : catalog_default_connection(type);
    gtk_editable_set_text(GTK_EDITABLE(app->connection), target_conn);
    start_discover_dbs(app, TRUE);
}
static void db_dropdown_changed(GObject *object, GParamSpec *parameter, gpointer data) {
    (void)object; (void)parameter;
    App *app = data;
    if (app->updating_dbs) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->db_dropdown));
    if (sel == 0 || sel == GTK_INVALID_LIST_POSITION) return;
    const char *db_name = gtk_string_list_get_string(app->db_list, sel);
    if (!db_name || !*db_name) return;
    guint type_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    const char *type = backend_type_name(type_idx);
    char *updated = catalog_format_connection_for_db(type, entry_text(app->connection), db_name);
    gtk_editable_set_text(GTK_EDITABLE(app->connection), updated);
    g_free(app->conn_by_type[type_idx]);
    app->conn_by_type[type_idx] = updated;
    if (!app->busy) start_load(NULL, app);
}

typedef struct { char *type, *connection, *group; gboolean groups_only; } LoadRequest; /* group NULL + !groups_only = everything */
static void load_request_free(gpointer data) {
    LoadRequest *r = data; g_free(r->type); g_free(r->connection); g_free(r->group); g_free(r);
}
static void load_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    LoadRequest *r = data; GError *error = NULL;
    NewsCatalog *catalog = catalog_load_ex(r->type, r->connection, cancel, &error, r->group, r->groups_only);
    if (catalog) g_task_return_pointer(task, catalog, catalog_free);
    else g_task_return_error(task, error);
}
static void load_finished(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    App *app = data; GError *error = NULL;
    LoadRequest *req = g_task_get_task_data(G_TASK(result));
    NewsCatalog *catalog = g_task_propagate_pointer(G_TASK(result), &error);
    set_busy(app, FALSE);
    if (catalog) {
        g_list_store_remove_all(app->store); catalog_free(app->catalog); app->catalog = catalog;
        if (req->groups_only) { /* just the group names: remember them, hold no articles */
            if (app->all_groups) g_ptr_array_unref(app->all_groups);
            app->all_groups = g_ptr_array_new_with_free_func(g_free);
            for (guint i = 0; i < catalog->groups->len; ++i) g_ptr_array_add(app->all_groups, g_strdup(g_ptr_array_index(catalog->groups, i)));
            g_free(app->loaded_group); app->loaded_group = NULL;
        } else {
            g_free(app->loaded_group); app->loaded_group = g_strdup(req->group ? req->group : "*");
        }
        guint cur_type = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
        if (catalog->resolved_connection && *catalog->resolved_connection) {
            gtk_editable_set_text(GTK_EDITABLE(app->connection), catalog->resolved_connection);
            if (cur_type <= 2) {
                g_free(app->conn_by_type[cur_type]);
                app->conn_by_type[cur_type] = g_strdup(catalog->resolved_connection);
            }
        }
        if (catalog->resolved_database && *catalog->resolved_database) {
            select_db_in_dropdown(app, catalog->resolved_database);
        }
        /* Settings stored in the database (nntp2sql --save-config) apply the first time that database is
         * opened, so later edits to the fields are not overwritten by a reload. */
        if (catalog->default_host && *catalog->default_host &&
            g_strcmp0(app->config_db, catalog->resolved_database) != 0) {
            g_free(app->config_db);
            app->config_db = g_strdup(catalog->resolved_database);
            gtk_editable_set_text(GTK_EDITABLE(app->host), catalog->default_host);
            if (catalog->default_user) gtk_editable_set_text(GTK_EDITABLE(app->user), catalog->default_user);
            if (catalog->default_pass) gtk_editable_set_text(GTK_EDITABLE(app->password), catalog->default_pass);
            if (catalog->default_port > 0)
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->port), catalog->default_port);
            if (catalog->has_default_security)
                gtk_drop_down_set_selected(GTK_DROP_DOWN(app->security), catalog->default_security);
            if (catalog->default_workers > 0)
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->workers), catalog->default_workers);
        }
        populate_groups(app);
        refresh_rows(app);
        save_preferences(app);
        char *message;
        if (req->groups_only) {
            message = g_strdup_printf("Opened %s \u2022 %u groups have articles. Choose a group above to load it (or \u201cAll groups\u201d to load everything).",
                                      catalog->resolved_database ? catalog->resolved_database : "database", app->all_groups->len);
            g_clear_pointer(&app->catalog, catalog_free); /* nothing but the group names was wanted */
        } else {
            message = g_strdup_printf("Loaded %s \u2022 %s \u2022 %u articles \u2022 %u candidate yEnc files. Header completeness is not a download verification.",
                                      catalog->resolved_database ? catalog->resolved_database : "database",
                                      req->group ? req->group : "all groups", catalog->articles->len, catalog->files->len);
        }
        status(app, message); g_free(message);
    } else { status(app, error->message); g_clear_error(&error); }
    if (app->closing) g_main_loop_quit(app->loop);
}
static void start_load_request(App *app, const char *group, gboolean groups_only) {
    if (app->busy) return;
    guint type_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    const char *type = backend_type_name(type_idx);
    if (!*entry_text(app->connection)) {
        gtk_editable_set_text(GTK_EDITABLE(app->connection), catalog_default_connection(type));
    }
    LoadRequest *r = g_new0(LoadRequest, 1);
    r->type = g_strdup(type);
    r->connection = g_strdup(entry_text(app->connection));
    r->group = g_strdup(group); r->groups_only = groups_only;
    g_clear_object(&app->cancel); app->cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, app->cancel, load_finished, app);
    g_task_set_task_data(task, r, load_request_free);
    set_busy(app, TRUE);
    char *msg = groups_only ? g_strdup("Opening database\u2026") : group ? g_strdup_printf("Loading %s\u2026", group) : g_strdup("Loading every group\u2026");
    status(app, msg); g_free(msg);
    g_task_run_in_thread(task, load_worker); g_object_unref(task);
}
/* Opening or reloading a database loads only its group list; articles load when a group is chosen. */
static void start_load(GtkButton *button, gpointer data) {
    (void)button;
    start_load_request(data, NULL, TRUE);
}
static void request_free(gpointer data) {
    DownloadRequest *r = data;
    if (r->files) g_ptr_array_unref(r->files);
    if (r->temp_dir) g_rmdir(r->temp_dir);
    g_free(r->temp_dir); g_free(r->save_dir);
    settings_clear(&r->settings); g_mutex_clear(&r->progress.mutex); g_free(r);
}
static void transfer_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source;
    DownloadRequest *r = data; GError *error = NULL; char *output = NULL;
    if (r->files && r->files->len > 0)
        output = download_queued_files(r->files, &r->settings, &r->progress, cancel, &error);
    else {
        NewsConnection n = {0}; GString *body = g_string_new(NULL);
        gboolean ok = news_connect(&n, &r->settings, cancel, &error) && news_body(&n, r->article, &error);
        while (ok) {
            GByteArray *line = news_line(&n, &error);
            if (!line) { ok = FALSE; break; }
            if (line->len == 1 && line->data[0] == '.') { g_byte_array_unref(line); break; }
            gsize skip = line->len && line->data[0] == '.' ? 1 : 0;
            if (body->len + line->len > 8 * 1024 * 1024) { ok = yenc_error(&error, "Article body exceeds the 8 MiB text-view limit"); g_byte_array_unref(line); break; }
            g_string_append_len(body, (const char *)line->data + skip, (gssize)(line->len - skip));
            g_string_append_c(body, '\n'); g_byte_array_unref(line);
        }
        news_close(&n);
        if (ok) output = g_utf8_make_valid(body->str, (gssize)body->len);
        g_string_free(body, TRUE);
    }
    if (output) g_task_return_pointer(task, output, g_free);
    else g_task_return_error(task, error ? error : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Download failed"));
}
/* ---- built-in image viewer ---- */
typedef struct {
    GtkWindow *window;
    GtkWidget *picture, *info;
    GdkTexture *texture;
    GBytes *bytes;
    char *name;
    double zoom;
    gboolean fit;
} ImageViewer;

static void image_viewer_apply(ImageViewer *v) {
    int w = gdk_texture_get_width(v->texture), h = gdk_texture_get_height(v->texture);
    if (v->fit) {
        gtk_picture_set_can_shrink(GTK_PICTURE(v->picture), TRUE);
        gtk_widget_set_size_request(v->picture, -1, -1);
    } else {
        gtk_picture_set_can_shrink(GTK_PICTURE(v->picture), FALSE);
        gtk_widget_set_size_request(v->picture, (int)(w * v->zoom), (int)(h * v->zoom));
    }
    char *text = g_strdup_printf("%s — %d × %d px — %s", v->name, w, h,
                                 v->fit ? "fit to window" : "zoom");
    if (!v->fit) { char *z = g_strdup_printf("%s %d%%", text, (int)(v->zoom * 100 + 0.5)); g_free(text); text = z; }
    gtk_label_set_text(GTK_LABEL(v->info), text);
    g_free(text);
}
static void image_zoom(ImageViewer *v, double factor) {
    if (v->fit) { v->fit = FALSE; v->zoom = 1.0; }
    v->zoom = CLAMP(v->zoom * factor, 0.05, 16.0);
    image_viewer_apply(v);
}
static void image_zoom_in(GtkButton *b, gpointer d) { (void)b; image_zoom(d, 1.25); }
static void image_zoom_out(GtkButton *b, gpointer d) { (void)b; image_zoom(d, 0.8); }
static void image_zoom_actual(GtkButton *b, gpointer d) { (void)b; ImageViewer *v = d; v->fit = FALSE; v->zoom = 1.0; image_viewer_apply(v); }
static void image_zoom_fit(GtkButton *b, gpointer d) { (void)b; ImageViewer *v = d; v->fit = TRUE; image_viewer_apply(v); }
static void image_save_done(GObject *source, GAsyncResult *result, gpointer data) {
    ImageViewer *v = data; GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    if (file) {
        gsize len = 0; const void *bytes = g_bytes_get_data(v->bytes, &len);
        if (g_file_replace_contents(file, bytes, len, NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error)) {
            char *path = g_file_get_path(file), *msg = g_strdup_printf("Saved %s", path ? path : v->name);
            gtk_label_set_text(GTK_LABEL(v->info), msg); g_free(msg); g_free(path);
        } else gtk_label_set_text(GTK_LABEL(v->info), error->message);
        g_object_unref(file);
    }
    g_clear_error(&error);
}
static void image_save_clicked(GtkButton *b, gpointer d) {
    (void)b; ImageViewer *v = d;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save image");
    gtk_file_dialog_set_initial_name(dialog, v->name);
    gtk_file_dialog_save(dialog, v->window, NULL, image_save_done, v);
    g_object_unref(dialog);
}
static void image_viewer_destroy(GtkWidget *w, gpointer d) {
    (void)w; ImageViewer *v = d;
    g_object_unref(v->texture); g_bytes_unref(v->bytes); g_free(v->name); g_free(v);
}
/* The image toolbar (zoom, fit, save) and scrolled picture, for embedding in any window. */
static GtkWidget *image_pane_new(GtkWindow *window, GdkTexture *texture, GBytes *bytes, const char *name) {
    ImageViewer *v = g_new0(ImageViewer, 1);
    v->texture = g_object_ref(texture); v->bytes = g_bytes_ref(bytes); v->name = g_strdup(name); v->fit = TRUE; v->zoom = 1.0;
    v->window = window;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 8); gtk_widget_set_margin_end(bar, 8); gtk_widget_set_margin_top(bar, 8);
    struct { const char *label; GCallback cb; } buttons[] = {
        {"Zoom in", G_CALLBACK(image_zoom_in)}, {"Zoom out", G_CALLBACK(image_zoom_out)},
        {"100%", G_CALLBACK(image_zoom_actual)}, {"Fit", G_CALLBACK(image_zoom_fit)}, {"Save image…", G_CALLBACK(image_save_clicked)}};
    for (guint i = 0; i < G_N_ELEMENTS(buttons); ++i) {
        GtkWidget *button = gtk_button_new_with_label(buttons[i].label);
        g_signal_connect(button, "clicked", buttons[i].cb, v);
        gtk_box_append(GTK_BOX(bar), button);
    }
    v->info = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(v->info), 0); gtk_widget_set_hexpand(v->info, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(v->info), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_start(v->info, 8);
    gtk_box_append(GTK_BOX(bar), v->info);
    v->picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
    gtk_widget_set_hexpand(v->picture, TRUE); gtk_widget_set_vexpand(v->picture, TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), v->picture);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), bar); gtk_box_append(GTK_BOX(box), scroll);
    g_signal_connect(window, "destroy", G_CALLBACK(image_viewer_destroy), v);
    image_viewer_apply(v);
    return box;
}

/* ---- article window: contents, image, reply ---- */
typedef struct {
    const char *subject, *from, *date, *groups, *message_id, *references;
} ArticleMeta;

typedef struct {
    App *app;
    GtkWindow *window;
    char *subject, *from, *date, *groups, *message_id, *references, *text, *browsed_group;
} ArticleWindow;

typedef struct {
    App *app;
    GtkWindow *window;
    GtkWidget *from, *groups, *subject, *body, *status, *post;
    char *references;
} ReplyWindow;

static void reply_window_destroy(GtkWidget *w, gpointer d) { (void)w; ReplyWindow *rw = d; g_free(rw->references); g_free(rw); }

typedef struct { /* one POST in flight; the widgets are weak because the window may be closed meanwhile */
    TransferSettings settings;
    char *headers, *body;
    GtkWidget *status, *post;
} PostRequest;
static void post_request_free(gpointer data) {
    PostRequest *r = data;
    if (r->status) g_object_remove_weak_pointer(G_OBJECT(r->status), (gpointer *)&r->status);
    if (r->post) g_object_remove_weak_pointer(G_OBJECT(r->post), (gpointer *)&r->post);
    settings_clear(&r->settings); g_free(r->headers); g_free(r->body); g_free(r);
}
static void post_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; (void)cancel;
    PostRequest *r = data; GError *error = NULL; NewsConnection n = {0};
    gboolean ok = news_connect(&n, &r->settings, NULL, &error) && news_post(&n, r->headers, r->body, &error);
    if (n.stream) { news_command(&n, "QUIT", NULL); news_close(&n); }
    if (ok) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error ? error : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Posting failed"));
}
static void post_finished(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; (void)data;
    PostRequest *r = g_task_get_task_data(G_TASK(result));
    GError *error = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(result), &error);
    if (r->status) gtk_label_set_text(GTK_LABEL(r->status), ok ? "Posted. The server accepted your reply." : error->message);
    if (r->post) gtk_widget_set_sensitive(r->post, !ok); /* never allow an accidental second post of the same reply */
    g_clear_error(&error);
}
static void reply_post_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
    ReplyWindow *rw = data; GError *error = NULL;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    g_clear_error(&error);
    if (choice != 1) { gtk_label_set_text(GTK_LABEL(rw->status), "Not posted."); return; }
    App *app = rw->app;
    char *problem = NULL;
    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(rw->body));
    GtkTextIter s, e; gtk_text_buffer_get_bounds(tb, &s, &e);
    char *body = gtk_text_buffer_get_text(tb, &s, &e, FALSE);
    char *headers = article_build_headers(entry_text(rw->from), entry_text(rw->groups), entry_text(rw->subject), rw->references, &problem);
    if (!headers) { gtk_label_set_text(GTK_LABEL(rw->status), problem); g_free(problem); g_free(body); return; }
    PostRequest *r = g_new0(PostRequest, 1);
    r->headers = headers; r->body = body;
    r->settings.host = g_strdup(entry_text(app->host));
    r->settings.user = g_strdup(entry_text(app->user)); r->settings.password = g_strdup(entry_text(app->password));
    r->settings.directory = g_strdup("."); r->settings.timeout = 20;
    r->settings.port = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->port));
    r->settings.security = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->security));
    r->status = rw->status; r->post = rw->post;
    g_object_add_weak_pointer(G_OBJECT(r->status), (gpointer *)&r->status);
    g_object_add_weak_pointer(G_OBJECT(r->post), (gpointer *)&r->post);
    g_free(app->post_from); app->post_from = g_strdup(entry_text(rw->from)); save_preferences(app); /* remember the From line */
    gtk_widget_set_sensitive(rw->post, FALSE);
    gtk_label_set_text(GTK_LABEL(rw->status), "Posting\u2026");
    GTask *task = g_task_new(NULL, NULL, post_finished, NULL);
    g_task_set_task_data(task, r, post_request_free);
    g_task_run_in_thread(task, post_worker); g_object_unref(task);
}
static void reply_post_clicked(GtkButton *button, gpointer data) {
    (void)button; ReplyWindow *rw = data;
    if (!*entry_text(rw->app->host)) { gtk_label_set_text(GTK_LABEL(rw->status), "Enter the NNTP server (and login) in the main window first."); return; }
    char *problem = NULL;
    char *check = article_build_headers(entry_text(rw->from), entry_text(rw->groups), entry_text(rw->subject), rw->references, &problem);
    if (!check) { gtk_label_set_text(GTK_LABEL(rw->status), problem); g_free(problem); return; }
    g_free(check);
    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(rw->body));
    if (gtk_text_buffer_get_char_count(tb) == 0) { gtk_label_set_text(GTK_LABEL(rw->status), "The reply is empty."); return; }
    /* Posting is public and cannot be undone: always ask first. */
    char *message = g_strdup_printf("Post this reply to %s?", entry_text(rw->groups));
    char *detail = g_strdup_printf("It will be sent through %s and become public. It cannot be taken back.", entry_text(rw->app->host));
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_set_detail(dialog, detail);
    const char *labels[] = {"Cancel", "Post", NULL};
    gtk_alert_dialog_set_buttons(dialog, labels); gtk_alert_dialog_set_cancel_button(dialog, 0); gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_choose(dialog, rw->window, NULL, reply_post_confirmed, rw);
    g_object_unref(dialog); g_free(message); g_free(detail);
}
static void reply_cancel_clicked(GtkButton *button, gpointer data) { (void)button; gtk_window_close(((ReplyWindow *)data)->window); }

static GtkWidget *labeled_entry(GtkWidget *grid, int row, const char *label, const char *text) {
    GtkWidget *l = gtk_label_new(label), *entry = gtk_entry_new();
    gtk_label_set_xalign(GTK_LABEL(l), 1); gtk_widget_set_hexpand(entry, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry), text ? text : "");
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1); gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    return entry;
}
static void open_reply_window(ArticleWindow *aw) {
    ReplyWindow *rw = g_new0(ReplyWindow, 1);
    rw->app = aw->app;
    rw->references = article_reply_references(aw->references, aw->message_id);
    rw->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(rw->window, "Reply");
    gtk_window_set_transient_for(rw->window, aw->window);
    gtk_window_set_default_size(rw->window, 720, 560);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12); gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(grid), 8); gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    rw->from = labeled_entry(grid, 0, "From", aw->app->post_from);
    gtk_entry_set_placeholder_text(GTK_ENTRY(rw->from), "Your Name <you@example.com>");
    /* Default to the group being browsed; the original may be cross-posted to several. */
    char *first_group = NULL;
    if (aw->browsed_group && *aw->browsed_group) first_group = g_strdup(aw->browsed_group);
    else if (aw->groups) { char **parts = g_strsplit(aw->groups, ",", 2); first_group = g_strdup(g_strstrip(parts[0] ? parts[0] : "")); g_strfreev(parts); }
    rw->groups = labeled_entry(grid, 1, "Newsgroups", first_group); g_free(first_group);
    char *subject = article_reply_subject(aw->subject);
    rw->subject = labeled_entry(grid, 2, "Subject", subject); g_free(subject);
    gtk_box_append(GTK_BOX(box), grid);
    rw->body = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(rw->body), TRUE); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(rw->body), GTK_WRAP_WORD_CHAR);
    char *quoted = article_reply_body(aw->from, aw->date, aw->text);
    GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(rw->body));
    gtk_text_buffer_set_text(tb, quoted, -1); g_free(quoted);
    GtkTextIter end; gtk_text_buffer_get_end_iter(tb, &end); gtk_text_buffer_place_cursor(tb, &end);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), rw->body);
    gtk_widget_set_vexpand(scroll, TRUE); gtk_box_append(GTK_BOX(box), scroll);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    rw->status = gtk_label_new("Write your reply below the quoted text, then press Post.");
    gtk_label_set_xalign(GTK_LABEL(rw->status), 0); gtk_widget_set_hexpand(rw->status, TRUE); gtk_label_set_ellipsize(GTK_LABEL(rw->status), PANGO_ELLIPSIZE_END);
    rw->post = gtk_button_new_with_label("Post"); GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(rw->post, "suggested-action");
    g_signal_connect(rw->post, "clicked", G_CALLBACK(reply_post_clicked), rw);
    g_signal_connect(cancel, "clicked", G_CALLBACK(reply_cancel_clicked), rw);
    gtk_box_append(GTK_BOX(bar), rw->status); gtk_box_append(GTK_BOX(bar), cancel); gtk_box_append(GTK_BOX(bar), rw->post);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_window_set_child(rw->window, box);
    g_signal_connect(rw->window, "destroy", G_CALLBACK(reply_window_destroy), rw);
    gtk_window_present(rw->window);
    gtk_widget_grab_focus(rw->body);
}
static void article_reply_clicked(GtkButton *button, gpointer data) { (void)button; open_reply_window(data); }
static void article_close_clicked(GtkButton *button, gpointer data) { (void)button; gtk_window_close(((ArticleWindow *)data)->window); }
static void article_window_destroy(GtkWidget *w, gpointer d) {
    (void)w; ArticleWindow *aw = d;
    g_free(aw->subject); g_free(aw->from); g_free(aw->date); g_free(aw->groups); g_free(aw->message_id);
    g_free(aw->references); g_free(aw->text); g_free(aw->browsed_group); g_free(aw);
}
static GtkWidget *meta_label(const char *label, const char *value, GtkWidget *grid, int row) {
    GtkWidget *k = gtk_label_new(label), *v = gtk_label_new(value && *value ? value : "\u2014");
    gtk_widget_add_css_class(k, "dim-label"); gtk_label_set_xalign(GTK_LABEL(k), 1); gtk_label_set_xalign(GTK_LABEL(v), 0);
    /* One ellipsized line each: wrapping labels report a huge minimum height at narrow widths. */
    gtk_label_set_selectable(GTK_LABEL(v), TRUE); gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 90); gtk_label_set_width_chars(GTK_LABEL(v), 20);
    gtk_widget_set_hexpand(v, TRUE); gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1); gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    return v;
}
/* The window for one article: headers, the text, an image if the article carries one, and Reply. */
static void open_article_window(App *app, const ArticleMeta *meta, const char *text, const char *note,
                                GdkTexture *texture, GBytes *image_bytes, const char *image_name,
                                gboolean allow_reply, const char *browsed_group) {
    ArticleWindow *aw = g_new0(ArticleWindow, 1);
    aw->app = app;
    aw->subject = g_strdup(meta->subject); aw->from = g_strdup(meta->from); aw->date = g_strdup(meta->date);
    aw->groups = g_strdup(meta->groups); aw->message_id = g_strdup(meta->message_id); aw->references = g_strdup(meta->references);
    aw->text = g_strdup(text); aw->browsed_group = g_strdup(browsed_group);
    aw->window = GTK_WINDOW(gtk_window_new());
    char *title = g_strdup_printf("%s", meta->subject && *meta->subject ? meta->subject : (image_name ? image_name : "Article"));
    gtk_window_set_title(aw->window, title); g_free(title);
    gtk_window_set_default_size(aw->window, 860, texture ? 880 : 640);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12); gtk_widget_set_margin_top(box, 10); gtk_widget_set_margin_bottom(box, 10);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(grid), 10); gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    GtkWidget *subj = meta_label("Subject", meta->subject, grid, 0); gtk_widget_add_css_class(subj, "title-4");
    meta_label("From", meta->from, grid, 1); meta_label("Date", meta->date, grid, 2);
    meta_label("Newsgroups", meta->groups, grid, 3); meta_label("Message-ID", meta->message_id, grid, 4);
    gtk_box_append(GTK_BOX(box), grid);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE); gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR); gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 8); gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 8);
    GString *shown = g_string_new(text ? text : "");
    if (note && *note) { if (shown->len) g_string_append(shown, "\n\n"); g_string_append(shown, note); }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), shown->str, -1); g_string_free(shown, TRUE);
    GtkWidget *text_scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(text_scroll), tv);
    gtk_widget_set_vexpand(text_scroll, TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(text_scroll), 120);
    if (texture) { /* article text above, the image below */
        GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
        gtk_paned_set_start_child(GTK_PANED(paned), text_scroll);
        gtk_paned_set_end_child(GTK_PANED(paned), image_pane_new(aw->window, texture, image_bytes, image_name ? image_name : "image"));
        gtk_paned_set_position(GTK_PANED(paned), 150); gtk_widget_set_vexpand(paned, TRUE);
        gtk_box_append(GTK_BOX(box), paned);
    } else gtk_box_append(GTK_BOX(box), text_scroll);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_set_halign(bar, GTK_ALIGN_END);
    GtkWidget *reply = gtk_button_new_with_label("Reply"), *close = gtk_button_new_with_label("Close");
    gtk_widget_set_sensitive(reply, allow_reply);
    gtk_widget_set_tooltip_text(reply, allow_reply ? "Write a reply to this article" : "Replies are for articles, not files");
    g_signal_connect(reply, "clicked", G_CALLBACK(article_reply_clicked), aw);
    g_signal_connect(close, "clicked", G_CALLBACK(article_close_clicked), aw);
    gtk_box_append(GTK_BOX(bar), reply); gtk_box_append(GTK_BOX(bar), close); gtk_box_append(GTK_BOX(box), bar);
    gtk_window_set_child(aw->window, box);
    g_signal_connect(aw->window, "destroy", G_CALLBACK(article_window_destroy), aw);
    gtk_window_present(aw->window);
}
static void open_image_viewer(GdkTexture *texture, GBytes *bytes, const char *name) {
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    char *title = g_strdup_printf("Image \u2014 %s", name);
    gtk_window_set_title(window, title); g_free(title);
    gtk_window_set_default_size(window, 900, 700);
    gtk_window_set_child(window, image_pane_new(window, texture, bytes, name));
    gtk_window_present(window);
}
/* Case-insensitive filename check used before spending bandwidth on a non-image. */
static gboolean looks_like_image(const char *name) {
    static const char *exts[] = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".tif", ".tiff", ".avif", ".heic", NULL};
    for (int i = 0; name && exts[i]; ++i) if (ascii_icontains(name, exts[i])) return TRUE;
    return FALSE;
}
/* Unused name in dir for 'name'; an existing file of identical size counts as the same image. */
static char *image_destination(const char *dir, const char *name, gsize size) {
    const char *dot = strrchr(name, '.');
    char *stem = dot ? g_strndup(name, (gsize)(dot - name)) : g_strdup(name);
    for (int n = 0; n < 1000; ++n) {
        char *leaf = n ? g_strdup_printf("%s (%d)%s", stem, n, dot ? dot : "") : g_strdup(name);
        char *dest = g_build_filename(dir, leaf, NULL); g_free(leaf);
        GStatBuf st;
        if (g_stat(dest, &st) != 0 || (gsize)st.st_size == size) { g_free(stem); return dest; }
        g_free(dest);
    }
    g_free(stem);
    return NULL;
}
/* Loads the downloaded image, optionally keeps a copy, and opens the viewer.
 * Returns NULL on success or a malloc'd error message. 'r' is NULL when viewing an already-saved file. */
static char *show_image(App *app, const char *path, DownloadRequest *r, NewsFile *file) {
    GError *error = NULL; gchar *data = NULL; gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, &error)) {
        char *msg = g_strdup_printf("Cannot read image: %s", error->message); g_clear_error(&error); return msg;
    }
    GBytes *bytes = g_bytes_new_take(data, len);
    char *name = g_path_get_basename(path), *saved = NULL, *result = NULL;
    if (r) {
        if (r->save_copy && r->save_dir && *r->save_dir && g_mkdir_with_parents(r->save_dir, 0700) == 0) {
            char *dest = image_destination(r->save_dir, name, len);
            if (dest && (g_file_test(dest, G_FILE_TEST_EXISTS) || g_file_set_contents(dest, g_bytes_get_data(bytes, NULL), (gssize)len, NULL)))
                saved = dest;
            else g_free(dest);
        }
        g_unlink(path); /* the staging copy is never kept */
        if (file) {
            g_free(file->saved_path); file->saved_path = g_strdup(saved);
            g_free(file->status); file->status = g_strdup(saved ? "Complete — image saved" : "Image viewed (not saved)");
        }
    }
    GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
    if (texture) { /* shown in the article window, like an image found inside a text article */
        char *info = g_strdup_printf("%s%s%s", file ? file->title : name, file && file->group ? "\n" : "", file && file->group ? file->group : "");
        ArticleMeta meta = {file ? file->title : name, file ? file->author : NULL, NULL, file ? file->group : NULL, NULL, NULL};
        open_article_window(app, &meta, info, NULL, texture, bytes, name, FALSE, NULL);
        g_free(info); g_object_unref(texture);
    }
    else {
        result = g_strdup_printf("%s%s is not a supported image: %s", saved ? "Saved, but " : "", name, error->message);
        g_clear_error(&error);
    }
    g_free(saved); g_free(name); g_bytes_unref(bytes);
    return result;
}
static void transfer_finished(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; App *app = data;
    DownloadRequest *r = g_task_get_task_data(G_TASK(result));
    GError *error = NULL; char *output = g_task_propagate_pointer(G_TASK(result), &error);
    double elapsed = (g_get_monotonic_time() - r->started) / 1000000.0;
    if (r->files && r->files->len > 0) {
        refresh_rows(app);
    } else if (output) {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->details)),
                                 *output ? output : "(This article has an empty body.)", -1);
    } else if (error) {
        char *shown = g_strdup_printf("Could not read the article:\n%s", error->message);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->details)), shown, -1);
        g_free(shown);
    }
    char *message = g_strdup_printf("%s • %.2f seconds%s%s", output ? "Finished" : "Failed",
                                    elapsed, output && r->files ? " • " : "",
                                    output && r->files ? output : "");
    status(app, output ? message : error->message);
    if (output && r->view_image) {
        char *image_error = show_image(app, output, r, g_ptr_array_index(r->files, 0));
        if (image_error) { status(app, image_error); g_free(image_error); }
        refresh_rows(app);
    }
    gtk_label_set_text(GTK_LABEL(app->progress_label), message);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), output ? 1 : 0);
    g_free(message); g_free(output); g_clear_error(&error);
    app->active = NULL; set_busy(app, FALSE);
    if (app->closing) g_main_loop_quit(app->loop);
}
/* Collects all checked/queued files plus any multi-selected files in O(N) time. */
static GPtrArray *collect_target_files(App *app, gboolean *had_incomplete) {
    *had_incomplete = FALSE;
    if (!app->catalog) return NULL;
    GPtrArray *files = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < app->catalog->files->len; ++i) {
        NewsFile *f = g_ptr_array_index(app->catalog->files, i);
        if (f->queued) {
            if (file_has_headers(f)) {
                g_hash_table_add(seen, f);
                g_ptr_array_add(files, f);
            } else *had_incomplete = TRUE;
        }
    }
    GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(app->selection));
    GtkBitsetIter iter;
    guint idx = 0;
    if (gtk_bitset_iter_init_first(&iter, selected, &idx)) {
        do {
            ViewerRow *row = g_list_model_get_item(G_LIST_MODEL(app->store), idx);
            if (row) {
                NewsFile *f = row->file ? row->file : (row->article ? row->article->file : NULL);
                if (f && !g_hash_table_contains(seen, f)) {
                    if (file_has_headers(f)) {
                        g_hash_table_add(seen, f);
                        g_ptr_array_add(files, f);
                    } else *had_incomplete = TRUE;
                }
                g_object_unref(row);
            }
        } while (gtk_bitset_iter_next(&iter, &idx));
    }
    gtk_bitset_unref(selected);
    g_hash_table_destroy(seen);
    return files;
}
static void start_transfer(App *app, const char *directory, gboolean read_article) {
    if (app->busy) return;
    if (read_article) {
        ViewerRow *row = first_selected_row(app, NULL);
        if (!row) { status(app, "Select a file or article first."); return; }
        NewsArticle *article = row->article;
        g_object_unref(row);
        if (!article || article->yenc) { status(app, "Select a text article in Articles."); return; }
        if (!*entry_text(app->host)) { status(app, "Enter the NNTP server and download folder."); return; }
        DownloadRequest *r = g_new0(DownloadRequest, 1);
        r->app = app; r->article = article; r->started = g_get_monotonic_time();
        g_mutex_init(&r->progress.mutex);
        r->settings.host = g_strdup(entry_text(app->host));
        r->settings.user = g_strdup(entry_text(app->user)); r->settings.password = g_strdup(entry_text(app->password));
        r->settings.directory = g_canonicalize_filename(".", NULL);
        r->settings.port = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->port));
        r->settings.workers = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->workers));
        r->settings.security = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->security));
        r->settings.retries = 2; r->settings.timeout = 20;
        save_preferences(app);
        g_clear_object(&app->cancel); app->cancel = g_cancellable_new();
        GTask *task = g_task_new(NULL, app->cancel, transfer_finished, app);
        g_task_set_check_cancellable(task, FALSE);
        g_task_set_task_data(task, r, request_free);
        app->active = r; set_busy(app, TRUE); status(app, "Downloading…");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0);
        g_task_run_in_thread(task, transfer_worker); g_object_unref(task);
        return;
    }
    gboolean had_incomplete = FALSE;
    GPtrArray *files = collect_target_files(app, &had_incomplete);
    if (!files || files->len == 0) {
        if (files) g_ptr_array_unref(files);
        if (had_incomplete)
            status(app, "This file has missing or conflicting part headers. Import the missing headers before downloading.");
        else
            status(app, "Check or select one or more yEnc files first.");
        return;
    }
    if (!*entry_text(app->host) || !directory || !*directory) {
        g_ptr_array_unref(files);
        status(app, "Enter the NNTP server and download folder.");
        return;
    }
    DownloadRequest *r = g_new0(DownloadRequest, 1);
    r->app = app; r->files = files; r->started = g_get_monotonic_time();
    g_mutex_init(&r->progress.mutex);
    r->progress.files_total = files->len;
    r->settings.host = g_strdup(entry_text(app->host));
    r->settings.user = g_strdup(entry_text(app->user)); r->settings.password = g_strdup(entry_text(app->password));
    r->settings.directory = g_canonicalize_filename(directory, NULL);
    r->settings.port = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->port));
    r->settings.workers = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->workers));
    r->settings.security = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->security));
    r->settings.retries = 2; r->settings.timeout = 20;
    save_preferences(app);
    g_clear_object(&app->cancel); app->cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, app->cancel, transfer_finished, app);
    /* The coordinator checks cancellation before publication. A late cancellation
     * must not turn an already published, verified file into a reported failure. */
    g_task_set_check_cancellable(task, FALSE);
    g_task_set_task_data(task, r, request_free);
    app->active = r; set_busy(app, TRUE);
    char *start_msg = files->len > 1 ?
        g_strdup_printf("Downloading queue of %u files…", files->len) :
        g_strdup("Downloading…");
    status(app, start_msg); g_free(start_msg);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0);
    g_task_run_in_thread(task, transfer_worker); g_object_unref(task);
}
static gboolean tick(gpointer data) {
    App *app = data;
    if (app->active) {
        DownloadRequest *r = app->active;
        g_mutex_lock(&r->progress.mutex);
        guint done = r->progress.done, total = r->progress.total;
        guint fdone = r->progress.files_done, ftotal = r->progress.files_total;
        guint64 bytes = r->progress.bytes;
        g_mutex_unlock(&r->progress.mutex);
        char *text;
        double fraction = 0;
        if (ftotal > 1) {
            double file_frac = total ? (double)done / total : 0.0;
            fraction = ((double)fdone + file_frac) / (double)ftotal;
            text = g_strdup_printf("Queue: %u/%u files  •  %u/%u parts processed  •  %.2f MiB verified  •  %.1f seconds",
                fdone, ftotal, done, total, bytes / 1048576.0, (g_get_monotonic_time() - r->started) / 1000000.0);
        } else {
            fraction = total ? (double)done / total : 0.0;
            text = g_strdup_printf("%u/%u parts processed • %.2f MiB verified • %.1f seconds",
                done, total, bytes / 1048576.0, (g_get_monotonic_time() - r->started) / 1000000.0);
        }
        gtk_label_set_text(GTK_LABEL(app->progress_label), text); g_free(text);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), CLAMP(fraction, 0.0, 1.0));
    }
    return G_SOURCE_CONTINUE;
}
static void download_clicked(GtkButton *button, gpointer data) { (void)button; App *app = data; start_transfer(app, entry_text(app->directory), FALSE); }
/* ---- Read article: fetch the whole article and show it in its own window ---- */
typedef struct {
    App *app;
    TransferSettings settings;
    NewsArticle article;               /* identifies the article for the server; strings below are owned here */
    char *subject, *author, *date, *group, *message, *refs;
} ReadRequest;
static void read_request_free(gpointer data) {
    ReadRequest *r = data;
    settings_clear(&r->settings);
    g_free(r->subject); g_free(r->author); g_free(r->date); g_free(r->group); g_free(r->message); g_free(r->refs); g_free(r);
}
static void read_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; (void)cancel;
    ReadRequest *r = data; GError *error = NULL; NewsConnection n = {0}; gsize len = 0;
    char *raw = NULL;
    if (news_connect(&n, &r->settings, NULL, &error)) raw = news_fetch_article_raw(&n, &r->article, 16u * 1024 * 1024, &len, &error);
    if (n.stream) { news_command(&n, "QUIT", NULL); news_close(&n); }
    if (!raw) { g_task_return_error(task, error ? error : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read the article")); return; }
    ArticleContent *content = article_parse(raw, len);
    g_free(raw);
    g_task_return_pointer(task, content, article_content_free);
}
static void read_finished(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; (void)data;
    ReadRequest *r = g_task_get_task_data(G_TASK(result)); App *app = r->app;
    GError *error = NULL;
    ArticleContent *c = g_task_propagate_pointer(G_TASK(result), &error);
    if (!c) {
        status(app, error->message);
        char *shown = g_strdup_printf("Could not read the article:\n%s", error->message);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->details)), shown, -1); g_free(shown);
        g_clear_error(&error); return;
    }
    GdkTexture *texture = NULL; GError *image_error = NULL;
    if (c->image) {
        texture = gdk_texture_new_from_bytes(c->image, &image_error);
        if (!texture) { article_note(c, "[the attached image could not be displayed: %s]", image_error->message); g_clear_error(&image_error); }
    }
    ArticleMeta meta = {c->subject ? c->subject : r->subject, c->from ? c->from : r->author, c->date ? c->date : r->date,
                        c->newsgroups ? c->newsgroups : r->group, c->message_id ? c->message_id : r->message,
                        c->references ? c->references : r->refs};
    open_article_window(app, &meta, c->text, c->note, texture, texture ? c->image : NULL, c->image_name, TRUE, r->group);
    status(app, texture ? "Opened the article and its image in a new window." : "Opened the article in a new window.");
    if (texture) g_object_unref(texture);
    article_content_free(c);
}
static void start_read_window(App *app) {
    ViewerRow *row = first_selected_row(app, NULL);
    if (!row) { status(app, "Select an article first."); return; }
    NewsArticle *a = row->article;
    if (!a && row->file) { /* a file row: read its first part */
        a = g_tree_lookup(row->file->parts, GUINT_TO_POINTER(1U));
        if (!a) { GTreeNode *node = g_tree_node_first(row->file->parts); a = node ? g_tree_node_value(node) : NULL; }
    }
    if (!a) { g_object_unref(row); status(app, "Select an article (or switch to Articles or Text articles) first."); return; }
    if (!*entry_text(app->host)) { g_object_unref(row); status(app, "Enter the NNTP server (and login) first."); return; }
    ReadRequest *r = g_new0(ReadRequest, 1);
    r->app = app;
    r->subject = g_strdup(a->subject); r->author = g_strdup(a->author); r->date = g_strdup(a->date);
    r->group = g_strdup(a->group); r->message = g_strdup(a->message); r->refs = g_strdup(a->refs ? a->refs : "");
    r->article.number = a->number; r->article.group = r->group; r->article.message = r->message;
    r->settings.host = g_strdup(entry_text(app->host));
    r->settings.user = g_strdup(entry_text(app->user)); r->settings.password = g_strdup(entry_text(app->password));
    r->settings.directory = g_strdup("."); r->settings.timeout = 20;
    r->settings.port = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->port));
    r->settings.security = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->security));
    g_object_unref(row);
    save_preferences(app);
    status(app, "Opening the article…");
    GTask *task = g_task_new(NULL, NULL, read_finished, NULL);
    g_task_set_task_data(task, r, read_request_free);
    g_task_run_in_thread(task, read_worker); g_object_unref(task);
}
static void read_window_clicked(GtkButton *button, gpointer data) { (void)button; start_read_window(data); }

/* View image: reuses an already-saved copy, otherwise downloads the selected file (verified by the
 * normal yEnc/CRC path) into a private staging folder and opens it in the image viewer. */
static void start_image_view(App *app) {
    if (app->busy) return;
    ViewerRow *row = first_selected_row(app, NULL);
    if (!row) { status(app, "Select an image file first."); return; }
    NewsFile *file = row->file ? row->file : row->article ? row->article->file : NULL;
    g_object_unref(row);
    if (!file) { status(app, "Select a yEnc image file (not a text article)."); return; }
    if (file->saved_path && g_file_test(file->saved_path, G_FILE_TEST_IS_REGULAR)) {
        char *err = show_image(app, file->saved_path, NULL, NULL);
        status(app, err ? err : "Opened saved image."); g_free(err);
        return;
    }
    if (!looks_like_image(file->title)) { status(app, "This file does not look like an image (jpg, png, gif, webp, bmp, tiff…)."); return; }
    if (!file_has_headers(file)) { status(app, "This file has missing or conflicting part headers. Import the missing headers first."); return; }
    if (!*entry_text(app->host)) { status(app, "Enter the NNTP server first."); return; }
    GError *error = NULL;
    char *temp = g_dir_make_tmp("isotope-image-XXXXXX", &error);
    if (!temp) { status(app, error->message); g_clear_error(&error); return; }
    DownloadRequest *r = g_new0(DownloadRequest, 1);
    r->app = app; r->files = g_ptr_array_new(); g_ptr_array_add(r->files, file);
    r->started = g_get_monotonic_time(); r->view_image = TRUE; r->temp_dir = temp;
    r->save_copy = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->save_images));
    r->save_dir = g_canonicalize_filename(entry_text(app->directory), NULL);
    g_mutex_init(&r->progress.mutex);
    r->progress.files_total = 1;
    r->settings.host = g_strdup(entry_text(app->host));
    r->settings.user = g_strdup(entry_text(app->user)); r->settings.password = g_strdup(entry_text(app->password));
    r->settings.directory = g_strdup(temp);
    r->settings.port = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->port));
    r->settings.workers = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->workers));
    r->settings.security = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->security));
    r->settings.retries = 2; r->settings.timeout = 20;
    save_preferences(app);
    g_clear_object(&app->cancel); app->cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, app->cancel, transfer_finished, app);
    g_task_set_check_cancellable(task, FALSE);
    g_task_set_task_data(task, r, request_free);
    app->active = r; set_busy(app, TRUE); status(app, "Downloading image…");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0);
    g_task_run_in_thread(task, transfer_worker); g_object_unref(task);
}
static void view_image_clicked(GtkButton *button, gpointer data) { (void)button; start_image_view(data); }
static void cancel_clicked(GtkButton *button, gpointer data) {
    (void)button; App *app = data; if (app->cancel) g_cancellable_cancel(app->cancel);
}
static void folder_selected(GObject *source, GAsyncResult *result, gpointer data) {
    App *app = data; GError *error = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            if (g_object_get_data(source, "download")) start_transfer(app, path, FALSE);
            else { gtk_editable_set_text(GTK_EDITABLE(app->directory), path); save_preferences(app); }
        } else status(app, "Choose a local filesystem folder.");
        g_free(path); g_object_unref(file);
    }
    g_clear_error(&error);
}
static void choose_folder(GtkButton *button, gpointer data) {
    App *app = data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gboolean download = GTK_WIDGET(button) == app->download_to;
    gtk_file_dialog_set_title(dialog, download ? "Download file to folder" : "Default download folder");
    g_object_set_data(G_OBJECT(dialog), "download", GINT_TO_POINTER(download));
    if (*entry_text(app->directory)) {
        GFile *folder = g_file_new_for_path(entry_text(app->directory));
        gtk_file_dialog_set_initial_folder(dialog, folder); g_object_unref(folder);
    }
    gtk_file_dialog_select_folder(dialog, app->window, NULL, folder_selected, app); g_object_unref(dialog);
}
static gboolean close_window(GtkWindow *window, gpointer data) {
    (void)window; App *app = data;
    save_preferences(app);
    if (app->busy) {
        app->closing = TRUE; g_cancellable_cancel(app->cancel);
        status(app, "Cancelling active work before closing…");
    } else g_main_loop_quit(app->loop);
    return TRUE;
}
static GtkWidget *text_entry(const char *placeholder) {
    GtkWidget *entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    return entry;
}
static void grid_field(GtkWidget *grid, const char *label, GtkWidget *widget, int column, int row) {
    GtkWidget *text = gtk_label_new(label); gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_grid_attach(GTK_GRID(grid), text, column, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, column+1, row, 1, 1);
    gtk_widget_set_hexpand(widget, TRUE);
}
int main(int argc, char **argv) {
    if (argc != 1 && ((argc != 3 && argc != 4) || strcmp(argv[1], "--db-type"))) {
        g_printerr("Usage: isotope-viewer [--db-type sqlite|mysql|postgres [CONNECTION]]\n"); return 2;
    }
    mysql_library_init(0, NULL, NULL);
    app_icon_prepare("isotope-viewer", "Isotope");
    gtk_init();
    app_icon_setup(argv[0], "isotope-viewer");
    App app = {0}; app.loop = g_main_loop_new(NULL, FALSE);
    app.conn_by_type[0] = g_strdup(catalog_default_connection("sqlite"));
    app.conn_by_type[1] = g_strdup(catalog_default_connection("mysql"));
    app.conn_by_type[2] = g_strdup(catalog_default_connection("postgres"));
    app.preferences = g_build_filename(g_get_user_config_dir(), "nntp2sql", "viewer.ini", NULL);
    app.window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(app.window, "Isotope \u2014 Articles & Files");
    gtk_window_set_default_size(app.window, 1180, 850);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_window_set_child(app.window, box);
    GtkWidget *dbrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const char *dbtypes[] = {"SQLite", "MySQL / MariaDB", "PostgreSQL", NULL};
    app.dbtype = gtk_drop_down_new_from_strings(dbtypes);
    const char *initial_dbs[] = {"Detected DBs\u2026", NULL};
    app.db_list = gtk_string_list_new(initial_dbs);
    GtkExpression *db_expr = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
    app.db_dropdown = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(app.db_list)), db_expr);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(app.db_dropdown), TRUE);
    app.detect_dbs = gtk_button_new_with_label("Detect DBs");
    app.connection = text_entry(catalog_default_placeholder("sqlite"));
    gtk_editable_set_text(GTK_EDITABLE(app.connection), app.conn_by_type[0]);
    gtk_widget_set_hexpand(app.connection, TRUE);
    app.load = gtk_button_new_with_label("Open / Reload database");
    gtk_box_append(GTK_BOX(dbrow), app.dbtype);
    gtk_box_append(GTK_BOX(dbrow), gtk_label_new("Database"));
    gtk_box_append(GTK_BOX(dbrow), app.db_dropdown);
    gtk_box_append(GTK_BOX(dbrow), app.detect_dbs);
    gtk_box_append(GTK_BOX(dbrow), app.connection);
    gtk_box_append(GTK_BOX(dbrow), app.load);
    gtk_box_append(GTK_BOX(box), dbrow);
    GtkWidget *settings = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(settings), 10); gtk_grid_set_row_spacing(GTK_GRID(settings), 6);
    app.host = text_entry("news.example.com"); app.user = text_entry("Optional username");
    app.password = gtk_password_entry_new();
    app.port = gtk_spin_button_new_with_range(1, 65535, 1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.port), 563);
    app.workers = gtk_spin_button_new_with_range(1, 64, 1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.workers), 4);
    const char *security[] = {"TLS (certificate verified)", "STARTTLS (certificate verified)", "Plaintext", NULL};
    app.security = gtk_drop_down_new_from_strings(security);
    grid_field(settings, "NNTP server", app.host, 0, 0); grid_field(settings, "Port", app.port, 2, 0);
    grid_field(settings, "Connection", app.security, 4, 0); grid_field(settings, "Username", app.user, 0, 1);
    grid_field(settings, "Password", app.password, 2, 1); grid_field(settings, "Workers", app.workers, 4, 1);
    gtk_box_append(GTK_BOX(box), settings);
    GtkWidget *folderrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.directory = text_entry("Default download folder"); gtk_widget_set_hexpand(app.directory, TRUE);
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    char *folder = downloads ? g_build_filename(downloads, "nntp2sql", NULL) : g_build_filename(g_get_home_dir(), "Downloads", "nntp2sql", NULL);
    gtk_editable_set_text(GTK_EDITABLE(app.directory), folder); g_free(folder);
    app.browse = gtk_button_new_with_label("Set default folder\u2026");
    gtk_box_append(GTK_BOX(folderrow), gtk_label_new("Save to")); gtk_box_append(GTK_BOX(folderrow), app.directory); gtk_box_append(GTK_BOX(folderrow), app.browse);
    gtk_box_append(GTK_BOX(box), folderrow);
    GtkWidget *filterrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const char *modes[] = {"Files", "Articles", "Text articles", NULL}; app.mode = gtk_drop_down_new_from_strings(modes);
    const char *initial_groups[] = {"Select a group to load\u2026", "All groups (loads everything)", NULL};
    app.group_list = gtk_string_list_new(initial_groups);
    GtkExpression *group_expr = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
    app.group_dropdown = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(app.group_list)), group_expr);
    gtk_drop_down_set_enable_search(GTK_DROP_DOWN(app.group_dropdown), TRUE);
    block_scroll_changes(app.mode); block_scroll_changes(app.group_dropdown);
    block_scroll_changes(app.dbtype); block_scroll_changes(app.db_dropdown); block_scroll_changes(app.security);
    app.search = text_entry("Search subject, filename, author, group, date, message-ID, article #, status or 'queued'");
    gtk_widget_set_hexpand(app.search, TRUE);
    gtk_widget_set_tooltip_text(app.mode, "Files: yEnc files assembled from parts\nArticles: every article\nText articles: only text posts (no yEnc in the subject); selecting one shows its body");
    gtk_box_append(GTK_BOX(filterrow), app.mode);
    gtk_box_append(GTK_BOX(filterrow), gtk_label_new("Group"));
    gtk_box_append(GTK_BOX(filterrow), app.group_dropdown);
    app.hide_incomplete = gtk_check_button_new_with_label("Hide incomplete files");
    gtk_widget_set_tooltip_text(app.hide_incomplete, "Hide files (and their articles) that are missing part headers or are ambiguous");
    app.hide_text = gtk_check_button_new_with_label("Hide text articles");
    gtk_widget_set_tooltip_text(app.hide_text, "In Articles mode, hide text posts and show only yEnc/binary articles");
    gtk_box_append(GTK_BOX(filterrow), app.hide_text);
    gtk_box_append(GTK_BOX(filterrow), app.hide_incomplete);
    gtk_box_append(GTK_BOX(filterrow), app.search); gtk_box_append(GTK_BOX(box), filterrow);
    app.store = g_list_store_new(VIEWER_TYPE_ROW);
    app.selection = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(app.store)));
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(row_setup), &app); g_signal_connect(factory, "bind", G_CALLBACK(row_bind), &app);
    GtkWidget *list = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(app.selection)), factory);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE); gtk_widget_set_size_request(scroll, -1, 220);
    gtk_box_append(GTK_BOX(box), scroll);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.queue_selected = gtk_button_new_with_label("Queue selected");
    app.check_all = gtk_button_new_with_label("Check all shown");
    app.clear_queue = gtk_button_new_with_label("Clear queue");
    app.download = gtk_button_new_with_label("Download file / queue");
    app.download_to = gtk_button_new_with_label("Download to\u2026");
    app.view_image = gtk_button_new_with_label("View image");
    app.auto_text = gtk_check_button_new_with_label("Auto-load text articles");
    app.save_images = gtk_check_button_new_with_label("Save viewed images");
    gtk_widget_set_tooltip_text(app.auto_text, "Fetch and show a text article's body as soon as you select it in Articles mode");
    gtk_widget_set_tooltip_text(app.save_images, "Also keep a copy of each viewed image in the Save to folder");
    app.read = gtk_button_new_with_label("Read article"); app.cancel_button = gtk_button_new_with_label("Cancel");
    gtk_box_append(GTK_BOX(actions), app.queue_selected);
    gtk_box_append(GTK_BOX(actions), app.check_all);
    gtk_box_append(GTK_BOX(actions), app.clear_queue);
    gtk_box_append(GTK_BOX(actions), app.download); gtk_box_append(GTK_BOX(actions), app.download_to);
    gtk_box_append(GTK_BOX(actions), app.read); gtk_box_append(GTK_BOX(actions), app.view_image);
    gtk_box_append(GTK_BOX(actions), app.auto_text); gtk_box_append(GTK_BOX(actions), app.save_images);
    gtk_box_append(GTK_BOX(actions), app.cancel_button); gtk_box_append(GTK_BOX(box), actions);
    app.progress_bar = gtk_progress_bar_new(); app.progress_label = gtk_label_new("No active download");
    gtk_label_set_xalign(GTK_LABEL(app.progress_label), 0);
    gtk_box_append(GTK_BOX(box), app.progress_bar); gtk_box_append(GTK_BOX(box), app.progress_label);
    app.details = gtk_text_view_new(); gtk_text_view_set_editable(GTK_TEXT_VIEW(app.details), FALSE); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app.details), GTK_WRAP_WORD_CHAR);
    GtkWidget *detail_scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detail_scroll), app.details);
    gtk_widget_set_size_request(detail_scroll, -1, 160); gtk_box_append(GTK_BOX(box), detail_scroll);
    app.summary = gtk_label_new("Open an existing database to browse articles and yEnc files.");
    gtk_label_set_wrap(GTK_LABEL(app.summary), TRUE); gtk_label_set_xalign(GTK_LABEL(app.summary), 0);
    gtk_box_append(GTK_BOX(box), app.summary);
    load_preferences(&app);
    if (apply_nntp2sql_config(&app)) status(&app, "Defaults loaded from your nntp2sql config (~/.config/nntp2sql/nntp2sql.conf).");
    const char *init_type = backend_type_name(app.current_dbtype);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.connection), catalog_default_placeholder(init_type));
    gtk_editable_set_text(GTK_EDITABLE(app.connection),
        (app.conn_by_type[app.current_dbtype] && *app.conn_by_type[app.current_dbtype]) ?
        app.conn_by_type[app.current_dbtype] : catalog_default_connection(init_type));
    g_signal_connect(app.dbtype, "notify::selected", G_CALLBACK(dbtype_changed), &app);
    g_signal_connect(app.db_dropdown, "notify::selected", G_CALLBACK(db_dropdown_changed), &app);
    g_signal_connect(app.detect_dbs, "clicked", G_CALLBACK(detect_dbs_clicked), &app);
    g_signal_connect(app.load, "clicked", G_CALLBACK(start_load), &app);
    g_signal_connect(app.search, "changed", G_CALLBACK(search_changed), &app);
    g_signal_connect(app.mode, "notify::selected", G_CALLBACK(mode_changed), &app);
    g_signal_connect(app.hide_incomplete, "toggled", G_CALLBACK(hide_incomplete_toggled), &app);
    g_signal_connect(app.hide_text, "toggled", G_CALLBACK(hide_incomplete_toggled), &app);
    g_signal_connect(app.group_dropdown, "notify::selected", G_CALLBACK(group_changed), &app);
    g_signal_connect(app.selection, "selection-changed", G_CALLBACK(selection_changed), &app);
    g_signal_connect(app.queue_selected, "clicked", G_CALLBACK(queue_selected_clicked), &app);
    g_signal_connect(app.check_all, "clicked", G_CALLBACK(check_all_clicked), &app);
    g_signal_connect(app.clear_queue, "clicked", G_CALLBACK(clear_queue_clicked), &app);
    g_signal_connect(app.download, "clicked", G_CALLBACK(download_clicked), &app);
    g_signal_connect(app.download_to, "clicked", G_CALLBACK(choose_folder), &app);
    g_signal_connect(app.browse, "clicked", G_CALLBACK(choose_folder), &app);
    g_signal_connect(app.read, "clicked", G_CALLBACK(read_window_clicked), &app);
    g_signal_connect(app.view_image, "clicked", G_CALLBACK(view_image_clicked), &app);
    g_signal_connect(app.cancel_button, "clicked", G_CALLBACK(cancel_clicked), &app);
    g_signal_connect(app.window, "close-request", G_CALLBACK(close_window), &app);
    set_busy(&app, FALSE);
    if (argc >= 3) {
        guint type = !strcmp(argv[2], "sqlite") ? 0 : (!strcmp(argv[2], "mysql") || !strcmp(argv[2], "mariadb")) ? 1 : 2;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(app.dbtype), type);
        const char *cli_conn = (argc == 4 && argv[3] && *argv[3]) ? argv[3] : catalog_default_connection(backend_type_name(type));
        gtk_editable_set_text(GTK_EDITABLE(app.connection), cli_conn);
        start_discover_dbs(&app, FALSE);
        start_load(NULL, &app);
    } else {
        start_discover_dbs(&app, TRUE);
    }
    guint timer = g_timeout_add(200, tick, &app);
    gtk_window_present(app.window); g_main_loop_run(app.loop);
    g_source_remove(timer); gtk_window_destroy(app.window);
    for (int i = 0; i < 3; ++i) g_free(app.conn_by_type[i]);
    g_object_unref(app.db_list); g_object_unref(app.group_list); g_object_unref(app.selection); g_object_unref(app.store); catalog_free(app.catalog); if (app.all_groups) g_ptr_array_unref(app.all_groups); g_free(app.loaded_group); g_free(app.post_from);
    g_clear_object(&app.cancel); g_free(app.preferences); g_free(app.config_db); g_main_loop_unref(app.loop);
    mysql_library_end();
    return 0;
}
