#ifndef EXPORT_HTML_H
#define EXPORT_HTML_H
#include "db.h"
int export_group_to_html(DB *db, const char *group_name, const char *out_path);
int export_groups_from_file(DB *db, const char *group_list_path, const char *out_dir);
int export_group_to_html_paginated(DB *db, const char *group_name, const char *out_dir, int page_size);
#endif
