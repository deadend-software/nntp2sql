// Minimal HTML export utility
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"

static void write_html_header(FILE *fp, const char *title){
    fprintf(fp, "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>%s</title>\n", title ? title : "News Group Export");
    fprintf(fp, "<style>body{font-family:Helvetica,Arial,sans-serif;margin:20px}h1{font-size:18px}nav a{margin-right:8px}table{border-collapse:collapse;width:100%%}th,td{border:1px solid #ddd;padding:6px}th{background:#f7f7f7}</style>\n");
    fprintf(fp, "</head><body><h1>%s</h1>\n", title ? title : "News Group Export");
}

static void write_html_footer(FILE *fp){
    fprintf(fp, "</body></html>\n");
}

int export_group_to_html(DB *db, const char *group_name, const char *out_path){
    if (!db || !group_name || !out_path) return -1;
    FILE *fp = fopen(out_path, "w");
    if (!fp) return -2;
    write_html_header(fp, group_name);
    fprintf(fp, "<nav>\n");
    fprintf(fp, "</nav>\n");
    fprintf(fp, "<table><thead><tr><th>ArtNum</th><th>Subject</th><th>From</th><th>Date</th></tr></thead><tbody>\n");

    DBRow row;
    int rc = db_query_articles_begin(db, group_name);
    while (rc && db_query_articles_next(db, &row)){
        fprintf(fp, "<tr><td>%lld</td><td>%s</td><td>%s</td><td>%s</td></tr>\n",
                (long long)row.artnum,
                row.subject ? row.subject : "",
                row.author ? row.author : "",
                row.date ? row.date : "");
    }
    db_query_articles_end(db);
    fprintf(fp, "</tbody></table>\n");
    write_html_footer(fp);
    fclose(fp);
    return 0;
}

int export_groups_from_file(DB *db, const char *group_list_path, const char *out_dir){
    if (!db || !group_list_path || !out_dir) return -1;
    FILE *fp = fopen(group_list_path, "r");
    if (!fp) return -2;
    char buf[512];
    /* simple index page */
    char idx_path[1024]; snprintf(idx_path, sizeof(idx_path), "%s/index.html", out_dir);
    FILE *idx = fopen(idx_path, "w"); if (idx){ write_html_header(idx, "Group Index"); fprintf(idx, "<ul>\n"); }
    while (fgets(buf, sizeof(buf), fp)){
        char *nl = strchr(buf, '\n'); if (nl) *nl = '\0';
        if (*buf == '\0') continue;
        char out_path[1024];
        snprintf(out_path, sizeof(out_path), "%s/%s.html", out_dir, buf);
        export_group_to_html(db, buf, out_path);
        if (idx) fprintf(idx, "<li><a href=\"%s.html\">%s</a></li>\n", buf, buf);
    }
    fclose(fp);
    if (idx){ fprintf(idx, "</ul>\n"); write_html_footer(idx); fclose(idx); }
    return 0;
}

/* Optional pagination: generate pages of size page_size; filenames group-N.html */
int export_group_to_html_paginated(DB *db, const char *group_name, const char *out_dir, int page_size){
    if (!db || !group_name || !out_dir || page_size <= 0) return -1;
    int rc = db_query_articles_begin(db, group_name); if (!rc) return -3;
    int page = 1, count = 0;
    FILE *fp = NULL; char path[1024];
    DBRow row;
    while (db_query_articles_next(db, &row)){
        if (count % page_size == 0){
            if (fp) { fprintf(fp, "</tbody></table>\n"); write_html_footer(fp); fclose(fp); }
            snprintf(path, sizeof(path), "%s/%s-%d.html", out_dir, group_name, page);
            fp = fopen(path, "w"); if (!fp) break;
            char title[256]; snprintf(title, sizeof(title), "%s (page %d)", group_name, page);
            write_html_header(fp, title);
            fprintf(fp, "<nav>"); if (page>1) fprintf(fp, "<a href=\"%s-%d.html\">Prev</a>", group_name, page-1);
            fprintf(fp, "</nav>");
            fprintf(fp, "<table><thead><tr><th>ArtNum</th><th>Subject</th><th>From</th><th>Date</th></tr></thead><tbody>\n");
            page++;
        }
        fprintf(fp, "<tr><td>%lld</td><td>%s</td><td>%s</td><td>%s</td></tr>\n",
                (long long)row.artnum,
                row.subject ? row.subject : "",
                row.author ? row.author : "",
                row.date ? row.date : "");
        count++;
    }
    db_query_articles_end(db);
    if (fp) { fprintf(fp, "</tbody></table>\n"); write_html_footer(fp); fclose(fp); }
    return 0;
}
