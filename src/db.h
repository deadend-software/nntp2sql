#ifndef DB_H
#define DB_H
#include <sqlite3.h>
#if defined(__has_include) && __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif
#ifdef HAVE_PQ
#include <libpq-fe.h>
#endif

typedef enum { DB_SQLITE, DB_MYSQL, DB_POSTGRES } DBType;

typedef struct {
    DBType type;
    sqlite3 *sqlite;
    MYSQL *mysql;
    void *impl; /* Postgres state pointer */
    sqlite3_stmt *sqlite_insert_article;
    sqlite3_stmt *sqlite_insert_group;
    MYSQL_STMT *mysql_insert_article;
    sqlite3_stmt *sqlite_article_ins;
    sqlite3_stmt *sqlite_group_ins;
    MYSQL_STMT *mysql_article_ins;
} DB;

typedef struct {
    long long artnum;
    const char *subject;
    const char *author;
    const char *date;
} DBRow;

void db_close(DB *db);
void db_init_schema(DB *db);
void db_insert_group(DB *db, const char *name, long long count, long long first, long long last);
void db_insert_article(DB *db, const char *group, long long artnum, const char *subject,
                       const char *author, const char *date, const char *message_id,
                       const char *references, long long bytes, int lines);
char *db_escape(DB *db, const char *s);

/* Iteration helpers used by export_html */
int db_query_articles_begin(DB *db, const char *group_name);
int db_query_articles_next(DB *db, DBRow *out);
void db_query_articles_end(DB *db);

#endif