// PostgreSQL adapter (stubbed minimal integration)
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#include "db.h"

struct PGState {
    PGconn *conn;
    PGresult *res;
    int row_idx;
};

int db_pg_connect(DB *db, const char *conninfo){
    struct PGState *pg = (struct PGState*)calloc(1, sizeof(struct PGState));
    if (!pg) return -1;
    pg->conn = PQconnectdb(conninfo);
    if (PQstatus(pg->conn) != CONNECTION_OK){
        PQfinish(pg->conn);
        free(pg);
        return -2;
    }
    db->impl = pg;
    db->type = DB_POSTGRES;
    return 0;
}

void db_pg_close(DB *db){
    struct PGState *pg = (struct PGState*)db->impl;
    if (!pg) return;
    if (pg->res) PQclear(pg->res);
    if (pg->conn) PQfinish(pg->conn);
    free(pg);
    db->impl = NULL;
}

int db_pg_query_articles_begin(DB *db, const char *group){
    struct PGState *pg = (struct PGState*)db->impl;
    if (!pg || !group) return -1;
    const char *paramValues[1] = { group };
    pg->res = PQexecParams(pg->conn,
        "SELECT artnum, subject, author, date FROM articles WHERE group_name = $1 ORDER BY artnum",
        1, NULL, paramValues, NULL, NULL, 0);
    if (PQresultStatus(pg->res) != PGRES_TUPLES_OK){
        return -2;
    }
    pg->row_idx = 0;
    return 0;
}

int db_pg_query_articles_next(DB *db, DBRow *out){
    struct PGState *pg = (struct PGState*)db->impl;
    if (!pg || !pg->res) return 0;
    int rows = PQntuples(pg->res);
    if (pg->row_idx >= rows) return 0;
    int i = pg->row_idx++;
    out->artnum = atoll(PQgetvalue(pg->res, i, 0));
    out->subject = PQgetvalue(pg->res, i, 1);
    out->author  = PQgetvalue(pg->res, i, 2);
    out->date    = PQgetvalue(pg->res, i, 3);
    return 1;
}

void db_pg_query_articles_end(DB *db){
    struct PGState *pg = (struct PGState*)db->impl;
    if (!pg) return;
    if (pg->res) { PQclear(pg->res); pg->res = NULL; }
}
