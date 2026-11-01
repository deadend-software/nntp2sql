/* hydrogen-group-viewer: browse every newsgroup stored in an nntp2sql database.
 * Works with SQLite, MySQL/MariaDB and PostgreSQL, detects nntp2sql-schema databases
 * (news0, news1, ...) automatically, and starts with default connection strings for a
 * stock local install of each backend. Populate the groups table with
 * `nntp2sql --list-groups`. */
#include <gtk/gtk.h>
#include "catalog.h"
#include "icon.h"

#define GROUP_TYPE_ROW (group_row_get_type())
G_DECLARE_FINAL_TYPE(GroupRow, group_row, GROUP, ROW, GObject)
struct _GroupRow {
    GObject parent_instance;
    char *name;
    gint64 count, first, last;
};
G_DEFINE_FINAL_TYPE(GroupRow, group_row, G_TYPE_OBJECT)
static void group_row_finalize(GObject *object) {
    g_free(GROUP_ROW(object)->name);
    G_OBJECT_CLASS(group_row_parent_class)->finalize(object);
}
static void group_row_init(GroupRow *self) { (void)self; }
static void group_row_class_init(GroupRowClass *klass) { G_OBJECT_CLASS(klass)->finalize = group_row_finalize; }

typedef struct {
    GtkWindow *window;
    GtkWidget *dbtype, *db_dropdown, *detect, *connection, *open, *search, *summary, *view;
    GtkStringList *db_list;
    GListStore *store;
    GtkFilter *filter;
    GtkFilterListModel *filtered;
    GMainLoop *loop;
    char *query;
    char *conn_by_type[3];
    guint current_dbtype;
    gboolean busy, updating_dbs;
} App;

static const char *backend_type_name(guint idx) {
    static const char *types[] = {"sqlite", "mysql", "postgres"};
    return types[idx <= 2 ? idx : 0];
}
static const char *entry_text(GtkWidget *entry) { return gtk_editable_get_text(GTK_EDITABLE(entry)); }
static void status(App *app, const char *message) { gtk_label_set_text(GTK_LABEL(app->summary), message); }

static void set_busy(App *app, gboolean busy) {
    app->busy = busy;
    gtk_widget_set_sensitive(app->open, !busy);
    gtk_widget_set_sensitive(app->detect, !busy);
}

/* ---- loading groups from the database (worker thread) ---- */

typedef struct {
    GPtrArray *rows;       /* GroupRow* */
    char *resolved_connection, *resolved_database;
} LoadResult;

static void load_result_free(gpointer data) {
    LoadResult *r = data;
    if (!r) return;
    if (r->rows) g_ptr_array_unref(r->rows);
    g_free(r->resolved_connection);
    g_free(r->resolved_database);
    g_free(r);
}

static void add_row(GPtrArray *rows, const char *name, const char *count, const char *first, const char *last) {
    if (!name || !*name) return;
    GroupRow *row = g_object_new(GROUP_TYPE_ROW, NULL);
    row->name = g_strdup(name);
    row->count = count ? g_ascii_strtoll(count, NULL, 10) : 0;
    row->first = first ? g_ascii_strtoll(first, NULL, 10) : 0;
    row->last = last ? g_ascii_strtoll(last, NULL, 10) : 0;
    g_ptr_array_add(rows, row);
}

static LoadResult *load_groups(const char *type, const char *connection, GError **error) {
    const char *eff_conn = (connection && *connection) ? connection : catalog_default_connection(type);
    LoadResult *res = g_new0(LoadResult, 1);
    res->rows = g_ptr_array_new_with_free_func(g_object_unref);

    if (!strcmp(type, "sqlite")) {
        char *path = g_strdup(eff_conn);
        g_strstrip(path);
        if (g_str_has_prefix(path, "sqlite://")) {
            char *clean = g_strdup(path + 9);
            g_free(path);
            path = clean;
        }
        /* Default file missing: fall back to the first nntp2sql database found alongside it. */
        if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            GPtrArray *found = catalog_discover_databases("sqlite", path, NULL);
            if (found && found->len > 0) {
                g_free(path);
                path = g_strdup(g_ptr_array_index(found, 0));
            }
            if (found) g_ptr_array_unref(found);
        }
        res->resolved_connection = g_strdup(path);
        res->resolved_database = g_strdup(path);
        sqlite3 *db = NULL;
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(db, "SELECT name, article_count, first, last FROM groups ORDER BY name", -1, &stmt, NULL) != SQLITE_OK) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "SQLite (%s): %s", path,
                        db ? sqlite3_errmsg(db) : "cannot open database");
        } else {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                add_row(res->rows, (const char *)sqlite3_column_text(stmt, 0), (const char *)sqlite3_column_text(stmt, 1),
                        (const char *)sqlite3_column_text(stmt, 2), (const char *)sqlite3_column_text(stmt, 3));
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        if (db) sqlite3_close(db);
        g_free(path);
    } else if (!strcmp(type, "mysql") || !strcmp(type, "mariadb")) {
        CatalogConnParams p;
        catalog_parse_server_connection(type, eff_conn, &p);
        GError *conn_err = NULL;
        MYSQL *db = catalog_mysql_connect(&p, p.database, &conn_err);
        /* Default database name missing on this server: open the first nntp2sql-schema database instead. */
        if (!db && p.is_default_db) {
            GPtrArray *found = catalog_discover_databases("mysql", eff_conn, NULL);
            if (found && found->len > 0) {
                g_clear_error(&conn_err);
                g_free(p.database);
                p.database = g_strdup(g_ptr_array_index(found, 0));
                db = catalog_mysql_connect(&p, p.database, &conn_err);
            }
            if (found) g_ptr_array_unref(found);
        }
        if (!db) {
            g_propagate_error(error, conn_err ? conn_err : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "MySQL: connection failed"));
        } else {
            res->resolved_database = g_strdup(p.database);
            res->resolved_connection = g_strdup_printf("%s,%s,%s,%s,%u", p.host, p.database, p.user, p.password, p.port);
            MYSQL_RES *qr = NULL;
            if (mysql_query(db, "SELECT `name`, `article_count`, `first`, `last` FROM `groups` ORDER BY `name`") ||
                !(qr = mysql_store_result(db))) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "MySQL (%s): %s", p.database, mysql_error(db));
            } else {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(qr))) add_row(res->rows, row[0], row[1], row[2], row[3]);
                mysql_free_result(qr);
            }
            mysql_close(db);
        }
        mysql_thread_end();
        catalog_conn_params_clear(&p);
    } else if (!strcmp(type, "postgres") || !strcmp(type, "pgsql")) {
        PgApi api;
        if (catalog_load_pg_api(&api, error)) {
            CatalogConnParams p;
            catalog_parse_server_connection(type, eff_conn, &p);
            GError *conn_err = NULL;
            PGconn_t *db = catalog_pg_connect(&api, &p, p.database, &conn_err);
            if (!db && p.is_default_db) {
                GPtrArray *found = catalog_discover_databases("postgres", eff_conn, NULL);
                if (found && found->len > 0) {
                    g_clear_error(&conn_err);
                    g_free(p.database);
                    p.database = g_strdup(g_ptr_array_index(found, 0));
                    db = catalog_pg_connect(&api, &p, p.database, &conn_err);
                }
                if (found) g_ptr_array_unref(found);
            }
            if (!db) {
                g_propagate_error(error, conn_err ? conn_err : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "PostgreSQL: connection failed"));
            } else {
                res->resolved_database = g_strdup(p.database);
                res->resolved_connection = g_strdup_printf("%s,%s,%s,%s,%u", p.host, p.database, p.user, p.password, p.port);
                PGresult_t *qr = api.exec(db, "SELECT name, article_count, first, last FROM groups ORDER BY name");
                if (!qr || api.resultStatus(qr) != 2 /* PGRES_TUPLES_OK */) {
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "PostgreSQL (%s): %s", p.database,
                                qr ? api.resultErrorMessage(qr) : api.errorMessage(db));
                } else {
                    for (int i = 0; i < api.ntuples(qr); ++i)
                        add_row(res->rows, api.getvalue(qr, i, 0), api.getvalue(qr, i, 1),
                                api.getvalue(qr, i, 2), api.getvalue(qr, i, 3));
                }
                if (qr) api.clear(qr);
                api.finish(db);
            }
            catalog_conn_params_clear(&p);
        }
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported database backend");
    }

    if (error && *error) { load_result_free(res); return NULL; }
    return res;
}

typedef struct { char *type, *connection; } LoadRequest;
static void load_request_free(gpointer data) {
    LoadRequest *r = data;
    g_free(r->type); g_free(r->connection); g_free(r);
}
static void load_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; (void)cancel;
    LoadRequest *r = data;
    GError *error = NULL;
    LoadResult *res = load_groups(r->type, r->connection, &error);
    if (!res) g_task_return_error(task, error);
    else g_task_return_pointer(task, res, load_result_free);
}

static void update_summary(App *app) {
    guint shown = g_list_model_get_n_items(G_LIST_MODEL(app->filtered));
    guint total = g_list_model_get_n_items(G_LIST_MODEL(app->store));
    char *msg;
    if (total == 0)
        msg = g_strdup("No groups in this database yet. Run `nntp2sql --list-groups` to download the server's group list.");
    else if (app->query && *app->query)
        msg = g_strdup_printf("%u of %u groups match “%s”.", shown, total, app->query);
    else
        msg = g_strdup_printf("%u groups.", total);
    status(app, msg);
    g_free(msg);
}

static void select_db_in_dropdown(App *app, const char *name) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(app->db_list));
    guint idx = 0;
    for (guint i = 1; i < n; ++i)
        if (!g_strcmp0(gtk_string_list_get_string(app->db_list, i), name)) { idx = i; break; }
    app->updating_dbs = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->db_dropdown), idx);
    app->updating_dbs = FALSE;
}

static void load_finished(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    App *app = data;
    GError *error = NULL;
    LoadResult *res = g_task_propagate_pointer(G_TASK(result), &error);
    set_busy(app, FALSE);
    if (!res) {
        status(app, error ? error->message : "Failed to load groups");
        g_clear_error(&error);
        return;
    }
    guint cur = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    if (res->resolved_connection && *res->resolved_connection) {
        gtk_editable_set_text(GTK_EDITABLE(app->connection), res->resolved_connection);
        if (cur <= 2) { g_free(app->conn_by_type[cur]); app->conn_by_type[cur] = g_strdup(res->resolved_connection); }
    }
    if (res->resolved_database) select_db_in_dropdown(app, res->resolved_database);
    /* One bulk splice: per-item signals would stall the UI on 100k+ groups. */
    guint old_n = g_list_model_get_n_items(G_LIST_MODEL(app->store));
    g_list_store_splice(app->store, 0, old_n, res->rows->pdata, res->rows->len);
    char *title = g_strdup_printf("Hydrogen — Groups%s%s", res->resolved_database ? " — " : "",
                                  res->resolved_database ? res->resolved_database : "");
    gtk_window_set_title(app->window, title);
    g_free(title);
    update_summary(app);
    load_result_free(res);
}

static void start_load(GtkWidget *widget, gpointer data) {
    (void)widget;
    App *app = data;
    if (app->busy) return;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    const char *type = backend_type_name(idx);
    if (!*entry_text(app->connection))
        gtk_editable_set_text(GTK_EDITABLE(app->connection), catalog_default_connection(type));
    LoadRequest *r = g_new0(LoadRequest, 1);
    r->type = g_strdup(type);
    r->connection = g_strdup(entry_text(app->connection));
    set_busy(app, TRUE);
    status(app, "Loading groups…");
    GTask *task = g_task_new(NULL, NULL, load_finished, app);
    g_task_set_task_data(task, r, load_request_free);
    g_task_run_in_thread(task, load_worker);
    g_object_unref(task);
}

/* ---- detecting nntp2sql databases ---- */

typedef struct { char *type, *connection; gboolean auto_load; } DiscoverRequest;
static void discover_request_free(gpointer data) {
    DiscoverRequest *r = data;
    g_free(r->type); g_free(r->connection); g_free(r);
}
static void discover_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; (void)cancel;
    DiscoverRequest *r = data;
    GError *error = NULL;
    GPtrArray *dbs = catalog_discover_databases(r->type, r->connection, &error);
    if (error && dbs->len == 0) { g_ptr_array_unref(dbs); g_task_return_error(task, error); }
    else { g_clear_error(&error); g_task_return_pointer(task, dbs, (GDestroyNotify)g_ptr_array_unref); }
}
static void discover_finished(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    App *app = data;
    DiscoverRequest *req = g_task_get_task_data(G_TASK(result));
    GError *error = NULL;
    GPtrArray *dbs = g_task_propagate_pointer(G_TASK(result), &error);
    set_busy(app, FALSE);
    app->updating_dbs = TRUE;
    guint old_n = g_list_model_get_n_items(G_LIST_MODEL(app->db_list));
    if (old_n > 1) gtk_string_list_splice(app->db_list, 1, old_n - 1, NULL);
    guint cur_type = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    const char *type_name = backend_type_name(cur_type);

    if (dbs && dbs->len > 0) {
        const char **items = g_new0(const char *, dbs->len + 1);
        for (guint i = 0; i < dbs->len; ++i) items[i] = g_ptr_array_index(dbs, i);
        gtk_string_list_splice(app->db_list, 1, 0, (const char * const *)items);
        g_free(items);

        const char *cur_conn = entry_text(app->connection);
        char *cur_db = NULL;
        if (!strcmp(type_name, "sqlite")) cur_db = g_strdup(cur_conn);
        else {
            CatalogConnParams p;
            catalog_parse_server_connection(type_name, cur_conn, &p);
            cur_db = g_strdup(p.database);
            catalog_conn_params_clear(&p);
        }
        guint match = 0;
        for (guint i = 0; i < dbs->len; ++i)
            if (!g_strcmp0(cur_db, g_ptr_array_index(dbs, i))) { match = i + 1; break; }
        if (match == 0 && req->auto_load) {
            match = 1;
            char *updated = catalog_format_connection_for_db(type_name, cur_conn, g_ptr_array_index(dbs, 0));
            gtk_editable_set_text(GTK_EDITABLE(app->connection), updated);
            g_free(app->conn_by_type[cur_type]);
            app->conn_by_type[cur_type] = updated;
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(app->db_dropdown), match);
        g_free(cur_db);
        char *msg = g_strdup_printf("Detected %u nntp2sql database(s).", dbs->len);
        status(app, msg);
        g_free(msg);
        app->updating_dbs = FALSE;
        if (req->auto_load) start_load(NULL, app);
    } else {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(app->db_dropdown), 0);
        app->updating_dbs = FALSE;
        status(app, error ? error->message : "No nntp2sql databases detected on this connection.");
        /* Nothing detected: still try the connection as typed, so a custom string works. */
        if (req->auto_load && !error) start_load(NULL, app);
    }
    if (dbs) g_ptr_array_unref(dbs);
    g_clear_error(&error);
}
static void start_discover(App *app, gboolean auto_load) {
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    const char *type = backend_type_name(idx);
    if (!*entry_text(app->connection))
        gtk_editable_set_text(GTK_EDITABLE(app->connection), catalog_default_connection(type));
    DiscoverRequest *r = g_new0(DiscoverRequest, 1);
    r->type = g_strdup(type);
    r->connection = g_strdup(entry_text(app->connection));
    r->auto_load = auto_load;
    set_busy(app, TRUE);
    status(app, "Detecting nntp2sql databases…");
    GTask *task = g_task_new(NULL, NULL, discover_finished, app);
    g_task_set_task_data(task, r, discover_request_free);
    g_task_run_in_thread(task, discover_worker);
    g_object_unref(task);
}
static void detect_clicked(GtkButton *button, gpointer data) { (void)button; start_discover(data, TRUE); }

static void dbtype_changed(GObject *object, GParamSpec *pspec, gpointer data) {
    (void)object; (void)pspec;
    App *app = data;
    guint new_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    if (new_idx > 2) return;
    if (app->current_dbtype <= 2 && app->current_dbtype != new_idx && *entry_text(app->connection)) {
        g_free(app->conn_by_type[app->current_dbtype]);
        app->conn_by_type[app->current_dbtype] = g_strdup(entry_text(app->connection));
    }
    app->current_dbtype = new_idx;
    const char *type = backend_type_name(new_idx);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->connection), catalog_default_placeholder(type));
    gtk_editable_set_text(GTK_EDITABLE(app->connection),
        (app->conn_by_type[new_idx] && *app->conn_by_type[new_idx]) ? app->conn_by_type[new_idx] : catalog_default_connection(type));
    g_list_store_remove_all(app->store);
    start_discover(app, TRUE);
}
static void db_dropdown_changed(GObject *object, GParamSpec *pspec, gpointer data) {
    (void)object; (void)pspec;
    App *app = data;
    if (app->updating_dbs) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->db_dropdown));
    if (sel == 0 || sel == GTK_INVALID_LIST_POSITION) return;
    const char *db_name = gtk_string_list_get_string(app->db_list, sel);
    if (!db_name || !*db_name) return;
    guint type_idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dbtype));
    char *updated = catalog_format_connection_for_db(backend_type_name(type_idx), entry_text(app->connection), db_name);
    gtk_editable_set_text(GTK_EDITABLE(app->connection), updated);
    g_free(app->conn_by_type[type_idx]);
    app->conn_by_type[type_idx] = updated;
    start_load(NULL, app);
}

/* ---- list UI ---- */

static gboolean row_filter(gpointer item, gpointer data) {
    App *app = data;
    if (!app->query || !*app->query) return TRUE;
    return ascii_icontains(GROUP_ROW(item)->name, app->query);
}
static void search_changed(GtkSearchEntry *entry, gpointer data) {
    App *app = data;
    g_free(app->query);
    app->query = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    g_strstrip(app->query);
    gtk_filter_changed(app->filter, GTK_FILTER_CHANGE_DIFFERENT);
    update_summary(app);
}

enum { COL_NAME, COL_COUNT, COL_FIRST, COL_LAST };
static void cell_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer col) {
    (void)factory;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), GPOINTER_TO_INT(col) == COL_NAME ? 0.0f : 1.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(label), FALSE);
    gtk_list_item_set_child(item, label);
}
static void cell_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer col) {
    (void)factory;
    GroupRow *row = gtk_list_item_get_item(item);
    GtkLabel *label = GTK_LABEL(gtk_list_item_get_child(item));
    switch (GPOINTER_TO_INT(col)) {
        case COL_NAME: gtk_label_set_text(label, row->name); break;
        default: {
            gint64 v = GPOINTER_TO_INT(col) == COL_COUNT ? row->count : GPOINTER_TO_INT(col) == COL_FIRST ? row->first : row->last;
            char buf[32];
            g_snprintf(buf, sizeof(buf), "%" G_GINT64_FORMAT, v);
            gtk_label_set_text(label, buf);
        }
    }
}
static int cmp64(gint64 a, gint64 b) { return (a > b) - (a < b); }
static int compare_name(gconstpointer a, gconstpointer b, gpointer u) { (void)u; return g_ascii_strcasecmp(GROUP_ROW((gpointer)a)->name, GROUP_ROW((gpointer)b)->name); }
static int compare_count(gconstpointer a, gconstpointer b, gpointer u) { (void)u; return cmp64(GROUP_ROW((gpointer)a)->count, GROUP_ROW((gpointer)b)->count); }
static int compare_first(gconstpointer a, gconstpointer b, gpointer u) { (void)u; return cmp64(GROUP_ROW((gpointer)a)->first, GROUP_ROW((gpointer)b)->first); }
static int compare_last(gconstpointer a, gconstpointer b, gpointer u) { (void)u; return cmp64(GROUP_ROW((gpointer)a)->last, GROUP_ROW((gpointer)b)->last); }

static GtkColumnViewColumn *add_column(GtkWidget *view, const char *title, int col, GCompareDataFunc cmp, gboolean expand) {
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(cell_setup), GINT_TO_POINTER(col));
    g_signal_connect(factory, "bind", G_CALLBACK(cell_bind), GINT_TO_POINTER(col));
    GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);
    gtk_column_view_column_set_sorter(column, GTK_SORTER(gtk_custom_sorter_new(cmp, NULL, NULL)));
    gtk_column_view_column_set_resizable(column, TRUE);
    gtk_column_view_column_set_expand(column, expand);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), column);
    g_object_unref(column);
    return column;
}

static gboolean close_window(GtkWindow *window, gpointer data) {
    (void)window;
    g_main_loop_quit(((App *)data)->loop);
    return FALSE;
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: hydrogen-group-viewer [--db-type sqlite|mysql|postgres [CONNECTION]]\n"
               "  SQLite:     path to a database file or a directory of them (default %s)\n"
               "  MySQL:      host,database,user,password,port (default %s)\n"
               "  PostgreSQL: host,database,user,password,port (default %s)\n",
               catalog_default_connection("sqlite"), catalog_default_connection("mysql"), catalog_default_connection("postgres"));
        return 0;
    }
    if (argc != 1 && !(argc >= 3 && argc <= 4 && !strcmp(argv[1], "--db-type") &&
                       (!strcmp(argv[2], "sqlite") || !strcmp(argv[2], "mysql") || !strcmp(argv[2], "mariadb") ||
                        !strcmp(argv[2], "postgres") || !strcmp(argv[2], "pgsql")))) {
        g_printerr("Usage: hydrogen-group-viewer [--db-type sqlite|mysql|postgres [CONNECTION]]\n");
        return 2;
    }
    mysql_library_init(0, NULL, NULL);
    app_icon_prepare("hydrogen-group-viewer", "Hydrogen Groups");
    gtk_init();
    app_icon_setup(argv[0], "hydrogen-group-viewer");
    App app = {0};
    app.loop = g_main_loop_new(NULL, FALSE);
    app.conn_by_type[0] = g_strdup(catalog_default_connection("sqlite"));
    app.conn_by_type[1] = g_strdup(catalog_default_connection("mysql"));
    app.conn_by_type[2] = g_strdup(catalog_default_connection("postgres"));

    app.window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(app.window, "Hydrogen — Groups");
    gtk_window_set_default_size(app.window, 980, 720);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);

    GtkWidget *dbrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const char *dbtypes[] = {"SQLite", "MySQL / MariaDB", "PostgreSQL", NULL};
    app.dbtype = gtk_drop_down_new_from_strings(dbtypes);
    const char *initial_dbs[] = {"Detected databases…", NULL};
    app.db_list = gtk_string_list_new(initial_dbs);
    app.db_dropdown = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(app.db_list)), NULL);
    app.detect = gtk_button_new_with_label("Detect databases");
    app.connection = gtk_entry_new();
    gtk_widget_set_hexpand(app.connection, TRUE);
    app.open = gtk_button_new_with_label("Open / Reload");
    gtk_box_append(GTK_BOX(dbrow), app.dbtype);
    gtk_box_append(GTK_BOX(dbrow), gtk_label_new("Database"));
    gtk_box_append(GTK_BOX(dbrow), app.db_dropdown);
    gtk_box_append(GTK_BOX(dbrow), app.detect);
    gtk_box_append(GTK_BOX(dbrow), app.connection);
    gtk_box_append(GTK_BOX(dbrow), app.open);
    gtk_box_append(GTK_BOX(box), dbrow);

    app.search = gtk_search_entry_new();
    gtk_widget_set_hexpand(app.search, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(app.search), "Filter groups by name (e.g. alt.binaries)");
    gtk_box_append(GTK_BOX(box), app.search);

    app.store = g_list_store_new(GROUP_TYPE_ROW);
    app.filter = GTK_FILTER(gtk_custom_filter_new(row_filter, &app, NULL));
    app.filtered = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(app.store)), g_object_ref(app.filter));
    app.view = gtk_column_view_new(NULL);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(app.view), TRUE);
    GtkColumnViewColumn *name_col = add_column(app.view, "Group", COL_NAME, compare_name, TRUE);
    add_column(app.view, "Articles (est.)", COL_COUNT, compare_count, FALSE);
    add_column(app.view, "First", COL_FIRST, compare_first, FALSE);
    add_column(app.view, "Last", COL_LAST, compare_last, FALSE);
    GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(app.filtered)),
                                                       g_object_ref(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(app.view))));
    GtkSingleSelection *selection = gtk_single_selection_new(G_LIST_MODEL(sorted));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(app.view), GTK_SELECTION_MODEL(selection));
    g_object_unref(selection);
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(app.view), name_col, GTK_SORT_ASCENDING);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app.view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);

    app.summary = gtk_label_new("Open a database to list its newsgroups.");
    gtk_label_set_xalign(GTK_LABEL(app.summary), 0);
    gtk_label_set_wrap(GTK_LABEL(app.summary), TRUE);
    gtk_box_append(GTK_BOX(box), app.summary);
    gtk_window_set_child(app.window, box);

    guint init_type = 0;
    if (argc >= 3) {
        init_type = !strcmp(argv[2], "sqlite") ? 0 : (!strcmp(argv[2], "mysql") || !strcmp(argv[2], "mariadb")) ? 1 : 2;
        if (argc == 4 && *argv[3]) { g_free(app.conn_by_type[init_type]); app.conn_by_type[init_type] = g_strdup(argv[3]); }
    }
    app.current_dbtype = init_type;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app.dbtype), init_type);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.connection), catalog_default_placeholder(backend_type_name(init_type)));
    gtk_editable_set_text(GTK_EDITABLE(app.connection), app.conn_by_type[init_type]);

    g_signal_connect(app.dbtype, "notify::selected", G_CALLBACK(dbtype_changed), &app);
    g_signal_connect(app.db_dropdown, "notify::selected", G_CALLBACK(db_dropdown_changed), &app);
    g_signal_connect(app.detect, "clicked", G_CALLBACK(detect_clicked), &app);
    g_signal_connect(app.open, "clicked", G_CALLBACK(start_load), &app);
    g_signal_connect(app.connection, "activate", G_CALLBACK(start_load), &app);
    g_signal_connect(app.search, "search-changed", G_CALLBACK(search_changed), &app);
    g_signal_connect(app.window, "close-request", G_CALLBACK(close_window), &app);

    /* Click-and-go: detect databases on the default connection and open the first nntp2sql one. */
    start_discover(&app, TRUE);
    gtk_window_present(app.window);
    g_main_loop_run(app.loop);

    for (int i = 0; i < 3; ++i) g_free(app.conn_by_type[i]);
    g_free(app.query);
    g_main_loop_unref(app.loop);
    mysql_library_end();
    return 0;
}
