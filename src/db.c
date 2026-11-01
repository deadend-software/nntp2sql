/*
 * db.c - Database abstraction (beta 0.42)
 */
#include "db.h"
#include "logging.h"
#include <string.h>
#include <stdlib.h>

void db_close(DB *db) {
    if (!db) return;
    if (db->type == DB_SQLITE) {
        if (db->sqlite_insert_article) sqlite3_finalize(db->sqlite_insert_article);
        if (db->sqlite_insert_group) sqlite3_finalize(db->sqlite_insert_group);
        if (db->sqlite_article_ins) sqlite3_finalize(db->sqlite_article_ins);
        if (db->sqlite_group_ins) sqlite3_finalize(db->sqlite_group_ins);
    } else if (db->type == DB_MYSQL) {
        if (db->mysql_insert_article) mysql_stmt_close(db->mysql_insert_article);
        if (db->mysql_article_ins) mysql_stmt_close(db->mysql_article_ins);
    }
    if (db->sqlite) sqlite3_close(db->sqlite);
    if (db->mysql) mysql_close(db->mysql);
    memset(db, 0, sizeof(*db));
}

static void db_exec(DB *db, const char *sql) {
    if (db->type == DB_SQLITE) {
        char *err = NULL;
        if (sqlite3_exec(db->sqlite, sql, NULL, NULL, &err) != SQLITE_OK) { warnf("sqlite exec error: %s", err ? err : "unknown"); if (err) sqlite3_free(err); }
    } else if (db->type == DB_MYSQL) {
        if (mysql_query(db->mysql, sql)) { warnf("mysql exec error: %s", mysql_error(db->mysql)); }
    }
}

char *db_escape(DB *db, const char *s) {
    if (!s) return strdup("");
    if (db->type == DB_SQLITE) {
        size_t n = strlen(s); size_t cap = n * 2 + 3; char *out = malloc(cap); char *p = out; *p++ = '\'';
        for (; *s; s++) { if (*s == '\'') { *p++ = '\''; *p++ = '\''; } else *p++ = *s; }
        *p++ = '\''; *p++ = '\0'; return out;
    } else if (db->type == DB_MYSQL) {
        unsigned long len = (unsigned long)strlen(s); char *buf = malloc(len*2 + 3);
        mysql_real_escape_string(db->mysql, buf, s, len); char *out = malloc(strlen(buf) + 3);
        sprintf(out, "'%s'", buf); free(buf); return out;
    }
    return strdup("''");
}

void db_init_schema(DB *db) {
    if (db->type == DB_SQLITE) {
        db_exec(db, "CREATE TABLE IF NOT EXISTS groups (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE, article_count INTEGER, first INTEGER, last INTEGER);");
        db_exec(db, "CREATE TABLE IF NOT EXISTS articles (id INTEGER PRIMARY KEY AUTOINCREMENT, artnum INTEGER, subject TEXT, author TEXT, date TEXT, message_id TEXT, refs TEXT, bytes INTEGER, line_count INTEGER, group_name TEXT);");
        db_exec(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_articles_group_artnum ON articles(group_name, artnum);");
        if (sqlite3_prepare_v2(db->sqlite, "UPDATE articles SET subject=?, author=?, date=?, message_id=?, refs=?, bytes=?, line_count=? WHERE group_name=? AND artnum=?", -1, &db->sqlite_insert_article, NULL) != SQLITE_OK) {
            warnf("sqlite prepare article-update failed: %s", sqlite3_errmsg(db->sqlite));
        }
        if (sqlite3_prepare_v2(db->sqlite, "INSERT INTO articles (artnum, subject, author, date, message_id, refs, bytes, line_count, group_name) VALUES (?,?,?,?,?,?,?,?,?)", -1, &db->sqlite_article_ins, NULL) != SQLITE_OK) {
            warnf("sqlite prepare article-insert failed: %s", sqlite3_errmsg(db->sqlite));
        }
        if (sqlite3_prepare_v2(db->sqlite, "UPDATE groups SET article_count=?, first=?, last=? WHERE name=?", -1, &db->sqlite_insert_group, NULL) != SQLITE_OK) {
            warnf("sqlite prepare group-update failed: %s", sqlite3_errmsg(db->sqlite));
        }
        if (sqlite3_prepare_v2(db->sqlite, "INSERT INTO groups (name, article_count, first, last) VALUES (?,?,?,?)", -1, &db->sqlite_group_ins, NULL) != SQLITE_OK) {
            warnf("sqlite prepare group-insert failed: %s", sqlite3_errmsg(db->sqlite));
        }
    } else if (db->type == DB_MYSQL) {
        if (mysql_query(db->mysql, "CREATE TABLE IF NOT EXISTS `groups` (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(255) UNIQUE, article_count INT, first INT, last INT) ENGINE=InnoDB;")) {
            fatal(ERR_DB_SCHEMA, "mysql schema error (groups): %s", mysql_error(db->mysql));
        }
        if (mysql_query(db->mysql, "CREATE TABLE IF NOT EXISTS `articles` (id INT AUTO_INCREMENT PRIMARY KEY, `artnum` INT, `subject` TEXT, `author` TEXT, `date` TEXT, `message_id` TEXT, `refs` TEXT, `bytes` INT, `line_count` INT, `group_name` VARCHAR(255), UNIQUE KEY `idx_articles_group_artnum` (`group_name`,`artnum`)) ENGINE=InnoDB;")) {
            fatal(ERR_DB_SCHEMA, "mysql schema error (articles): %s", mysql_error(db->mysql));
        }
        if (mysql_query(db->mysql, "ALTER TABLE `articles` ADD UNIQUE KEY `idx_articles_group_artnum` (`group_name`,`artnum`);")) {
            const char *err = mysql_error(db->mysql); if (err && *err) infof("mysql index add note: %s", err);
        }
        db->mysql_insert_article = mysql_stmt_init(db->mysql);
        if (!db->mysql_insert_article) warnf("mysql_stmt_init failed for article");
        const char *art_sql = "UPDATE `articles` SET `subject`=?, `author`=?, `date`=?, `message_id`=?, `refs`=?, `bytes`=?, `line_count`=? WHERE `group_name`=? AND `artnum`=?";
        if (mysql_stmt_prepare(db->mysql_insert_article, art_sql, (unsigned long)strlen(art_sql))) {
            fatal(ERR_DB_PREPARE, "mysql prepare article-update failed: %s", mysql_error(db->mysql));
        }
        db->mysql_article_ins = mysql_stmt_init(db->mysql);
        if (!db->mysql_article_ins) warnf("mysql_stmt_init failed for article-insert");
        const char *art_ins = "INSERT INTO `articles` (`artnum`, `subject`, `author`, `date`, `message_id`, `refs`, `bytes`, `line_count`, `group_name`) VALUES (?,?,?,?,?,?,?,?,?)";
        if (db->mysql_article_ins && mysql_stmt_prepare(db->mysql_article_ins, art_ins, (unsigned long)strlen(art_ins))) {
            warnf("mysql prepare article-insert failed: %s", mysql_error(db->mysql)); mysql_stmt_close(db->mysql_article_ins); db->mysql_article_ins = NULL;
        }
    }
}

void db_insert_group(DB *db, const char *name, long long count, long long first, long long last) {
    if (db->type == DB_SQLITE && db->sqlite_insert_group) {
        if (sqlite3_bind_int64(db->sqlite_insert_group, 1, count) != SQLITE_OK ||
            sqlite3_bind_int64(db->sqlite_insert_group, 2, first) != SQLITE_OK ||
            sqlite3_bind_int64(db->sqlite_insert_group, 3, last) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_group, 4, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            warnf("sqlite bind group-update failed: %s", sqlite3_errmsg(db->sqlite)); sqlite3_reset(db->sqlite_insert_group); sqlite3_clear_bindings(db->sqlite_insert_group); return;
        }
        if (sqlite3_step(db->sqlite_insert_group) != SQLITE_DONE) { warnf("sqlite group update step failed: %s", sqlite3_errmsg(db->sqlite)); }
        int ch = sqlite3_changes(db->sqlite); sqlite3_reset(db->sqlite_insert_group); sqlite3_clear_bindings(db->sqlite_insert_group);
        if (ch == 0) {
            if (g_upsert && db->sqlite_group_ins) {
                if (sqlite3_bind_text(db->sqlite_group_ins, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_int64(db->sqlite_group_ins, 2, count) != SQLITE_OK ||
                    sqlite3_bind_int64(db->sqlite_group_ins, 3, first) != SQLITE_OK ||
                    sqlite3_bind_int64(db->sqlite_group_ins, 4, last) != SQLITE_OK) {
                    warnf("sqlite bind group-insert failed: %s", sqlite3_errmsg(db->sqlite)); sqlite3_reset(db->sqlite_group_ins); sqlite3_clear_bindings(db->sqlite_group_ins);
                } else {
                    if (sqlite3_step(db->sqlite_group_ins) != SQLITE_DONE) { warnf("sqlite group insert step failed: %s", sqlite3_errmsg(db->sqlite)); }
                    sqlite3_reset(db->sqlite_group_ins); sqlite3_clear_bindings(db->sqlite_group_ins); infof("group inserted: %s", name);
                }
            } else { warnf("group not found for update: %s", name); }
        }
        return;
    }
    if (db->type == DB_MYSQL) {
        char esc_name[512]; mysql_real_escape_string(db->mysql, esc_name, name, (unsigned long)strlen(name)); char sql[1024];
        snprintf(sql, sizeof(sql), "UPDATE `groups` SET article_count=%lld, first=%lld, last=%lld WHERE name='%s'", count, first, last, esc_name);
        if (mysql_query(db->mysql, sql)) { warnf("mysql group update error: %s", mysql_error(db->mysql)); }
        my_ulonglong ch = mysql_affected_rows(db->mysql);
        if (ch == 0) {
            if (g_upsert) {
                snprintf(sql, sizeof(sql), "INSERT INTO `groups` (name,article_count,first,last) VALUES ('%s',%lld,%lld,%lld)", esc_name, count, first, last);
                if (mysql_query(db->mysql, sql)) { warnf("mysql group insert error: %s", mysql_error(db->mysql)); } else { infof("group inserted: %s", name); }
            } else { warnf("group not found for update: %s", name); }
        }
        return;
    }
}

void db_insert_article(DB *db, const char *group, long long artnum, const char *subject,
                       const char *author, const char *date, const char *message_id,
                       const char *references, long long bytes, int lines) {
    if (db->type == DB_SQLITE && db->sqlite_insert_article) {
        if (sqlite3_bind_text(db->sqlite_insert_article, 1, subject, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 2, author, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 3, date, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 4, message_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 5, references, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int64(db->sqlite_insert_article, 6, bytes) != SQLITE_OK ||
            sqlite3_bind_int(db->sqlite_insert_article, 7, lines) != SQLITE_OK ||
            sqlite3_bind_text(db->sqlite_insert_article, 8, group, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_int64(db->sqlite_insert_article, 9, artnum) != SQLITE_OK) {
            warnf("sqlite bind article-update failed: %s", sqlite3_errmsg(db->sqlite)); sqlite3_reset(db->sqlite_insert_article); sqlite3_clear_bindings(db->sqlite_insert_article); return;
        }
        if (sqlite3_step(db->sqlite_insert_article) != SQLITE_DONE) { warnf("sqlite article update step failed: %s", sqlite3_errmsg(db->sqlite)); }
        int ch = sqlite3_changes(db->sqlite); sqlite3_reset(db->sqlite_insert_article); sqlite3_clear_bindings(db->sqlite_insert_article);
        if (ch == 0) {
            if (g_upsert && db->sqlite_article_ins) {
                if (sqlite3_bind_int64(db->sqlite_article_ins, 1, artnum) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 2, subject, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 3, author, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 4, date, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 5, message_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 6, references, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_int64(db->sqlite_article_ins, 7, bytes) != SQLITE_OK ||
                    sqlite3_bind_int(db->sqlite_article_ins, 8, lines) != SQLITE_OK ||
                    sqlite3_bind_text(db->sqlite_article_ins, 9, group, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
                    warnf("sqlite bind article-insert failed: %s", sqlite3_errmsg(db->sqlite)); sqlite3_reset(db->sqlite_article_ins); sqlite3_clear_bindings(db->sqlite_article_ins);
                } else {
                    if (sqlite3_step(db->sqlite_article_ins) != SQLITE_DONE) { warnf("sqlite article insert step failed: %s", sqlite3_errmsg(db->sqlite)); }
                    sqlite3_reset(db->sqlite_article_ins); sqlite3_clear_bindings(db->sqlite_article_ins); infof("article inserted: %s #%lld", group, artnum);
                }
            } else { warnf("article not found for update: %s #%lld", group, artnum); }
        }
        return;
    }
    if (db->type == DB_MYSQL && db->mysql_insert_article) {
        MYSQL_BIND b[9]; memset(b, 0, sizeof(b)); unsigned long sl=(unsigned long)strlen(subject), al=(unsigned long)strlen(author), dl=(unsigned long)strlen(date), ml=(unsigned long)strlen(message_id), rl=(unsigned long)strlen(references), gl=(unsigned long)strlen(group);
        b[0].buffer_type = MYSQL_TYPE_STRING; b[0].buffer=(void*)subject; b[0].buffer_length=sl;
        b[1].buffer_type = MYSQL_TYPE_STRING; b[1].buffer=(void*)author; b[1].buffer_length=al;
        b[2].buffer_type = MYSQL_TYPE_STRING; b[2].buffer=(void*)date; b[2].buffer_length=dl;
        b[3].buffer_type = MYSQL_TYPE_STRING; b[3].buffer=(void*)message_id; b[3].buffer_length=ml;
        b[4].buffer_type = MYSQL_TYPE_STRING; b[4].buffer=(void*)references; b[4].buffer_length=rl;
        b[5].buffer_type = MYSQL_TYPE_LONGLONG; b[5].buffer=(void*)&bytes;
        b[6].buffer_type = MYSQL_TYPE_LONG; b[6].buffer=(void*)&lines;
        b[7].buffer_type = MYSQL_TYPE_STRING; b[7].buffer=(void*)group; b[7].buffer_length=gl;
        b[8].buffer_type = MYSQL_TYPE_LONGLONG; b[8].buffer=(void*)&artnum;
        if (mysql_stmt_bind_param(db->mysql_insert_article, b)) { warnf("mysql bind article-update failed: %s", mysql_stmt_error(db->mysql_insert_article)); mysql_stmt_reset(db->mysql_insert_article); return; }
        if (mysql_stmt_execute(db->mysql_insert_article)) { warnf("mysql execute article-update failed: %s", mysql_stmt_error(db->mysql_insert_article)); }
        my_ulonglong ch = mysql_stmt_affected_rows(db->mysql_insert_article); mysql_stmt_reset(db->mysql_insert_article);
        if (ch == 0) {
            if (g_upsert && db->mysql_article_ins) {
                MYSQL_BIND bi[9]; memset(bi, 0, sizeof(bi));
                bi[0].buffer_type = MYSQL_TYPE_LONGLONG; bi[0].buffer=(void*)&artnum;
                bi[1].buffer_type = MYSQL_TYPE_STRING; bi[1].buffer=(void*)subject; bi[1].buffer_length=sl;
                bi[2].buffer_type = MYSQL_TYPE_STRING; bi[2].buffer=(void*)author; bi[2].buffer_length=al;
                bi[3].buffer_type = MYSQL_TYPE_STRING; bi[3].buffer=(void*)date; bi[3].buffer_length=dl;
                bi[4].buffer_type = MYSQL_TYPE_STRING; bi[4].buffer=(void*)message_id; bi[4].buffer_length=ml;
                bi[5].buffer_type = MYSQL_TYPE_STRING; bi[5].buffer=(void*)references; bi[5].buffer_length=rl;
                bi[6].buffer_type = MYSQL_TYPE_LONGLONG; bi[6].buffer=(void*)&bytes;
                bi[7].buffer_type = MYSQL_TYPE_LONG; bi[7].buffer=(void*)&lines;
                bi[8].buffer_type = MYSQL_TYPE_STRING; bi[8].buffer=(void*)group; bi[8].buffer_length=gl;
                if (mysql_stmt_bind_param(db->mysql_article_ins, bi)) { warnf("mysql bind article-insert failed: %s", mysql_stmt_error(db->mysql_article_ins)); mysql_stmt_reset(db->mysql_article_ins); }
                else if (mysql_stmt_execute(db->mysql_article_ins)) { warnf("mysql execute article-insert failed: %s", mysql_stmt_error(db->mysql_article_ins)); mysql_stmt_reset(db->mysql_article_ins); }
                else { mysql_stmt_reset(db->mysql_article_ins); infof("article inserted: %s #%lld", group, artnum); }
            } else { warnf("article not found for update: %s #%lld", group, artnum); }
        }
        return;
    }
    char *g = db_escape(db, group), *s = db_escape(db, subject), *a = db_escape(db, author), *d = db_escape(db, date), *m = db_escape(db, message_id), *r = db_escape(db, references);
    char sql[4096]; snprintf(sql, sizeof(sql), "UPDATE `articles` SET `subject`=%s, `author`=%s, `date`=%s, `message_id`=%s, `refs`=%s, `bytes`=%lld, `line_count`=%d WHERE `group_name`=%s AND `artnum`=%lld;", s,a,d,m,r,bytes,lines,g,artnum); db_exec(db, sql);
    if (g_upsert) { snprintf(sql, sizeof(sql), "INSERT INTO `articles` (`artnum`, `subject`, `author`, `date`, `message_id`, `refs`, `bytes`, `line_count`, `group_name`) VALUES (%lld,%s,%s,%s,%s,%s,%lld,%d,%s);", artnum,s,a,d,m,r,bytes,lines,g); db_exec(db, sql); }
    free(g); free(s); free(a); free(d); free(m); free(r);
}
