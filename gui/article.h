/* Reading and replying to articles: fetching a whole article, extracting an image from it (MIME base64
 * attachments or an inline yEnc body), composing a reply, and posting it with NNTP POST. No UI here. */
#ifndef VIEWER_ARTICLE_H
#define VIEWER_ARTICLE_H
#include "transfer.h"

typedef struct {
    char *subject, *from, *date, *newsgroups, *message_id, *references; /* from the article's own headers */
    char *headers;      /* raw header block, for display */
    char *text;         /* body as valid UTF-8 (text parts only) */
    GBytes *image;      /* first image found (raw file bytes), or NULL */
    char *image_name;   /* file name of that image, or NULL */
    char *note;         /* e.g. attachments that were skipped */
} ArticleContent;

static void article_content_free(gpointer value) {
    ArticleContent *a = value;
    if (!a) return;
    g_free(a->subject); g_free(a->from); g_free(a->date); g_free(a->newsgroups); g_free(a->message_id);
    g_free(a->references); g_free(a->headers); g_free(a->text); g_free(a->image_name); g_free(a->note);
    if (a->image) g_bytes_unref(a->image);
    g_free(a);
}

/* ---- network ---- */

/* Fetches the whole article (headers and body, dot-unstuffed, LF line ends). Returns the raw bytes or NULL. */
static char *news_fetch_article_raw(NewsConnection *n, NewsArticle *a, gsize limit, gsize *out_len, GError **error) {
    char *command = NULL;
    if (*a->message) {
        gsize length = strlen(a->message);
        if (length < 3 || length > 998 || a->message[0] != '<' || a->message[length-1] != '>' || strpbrk(a->message, "\r\n \t"))
            { yenc_error(error, "Invalid Message-ID in database"); return NULL; }
        command = g_strconcat("ARTICLE ", a->message, NULL);
    } else {
        if (!*a->group || a->number < 1 || strspn(a->group, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-") != strlen(a->group))
            { yenc_error(error, "No valid Message-ID or group/article number"); return NULL; }
        char *group_cmd = g_strconcat("GROUP ", a->group, NULL);
        int gcode = news_command(n, group_cmd, error); g_free(group_cmd);
        if (gcode != 211) { if (!error || !*error) g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "NNTP group unavailable (%d)", gcode); return NULL; }
        command = g_strdup_printf("ARTICLE %" G_GINT64_FORMAT, a->number);
    }
    int code = news_command(n, command, error); g_free(command);
    if (code != 220) {
        if (!error || !*error) g_set_error(error, G_IO_ERROR, code == 430 || code == 423 ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_FAILED,
                                           code == 430 || code == 423 ? "The server no longer has this article (%d)" : "Article unavailable (%d)", code);
        return NULL;
    }
    GString *all = g_string_new(NULL);
    for (;;) {
        GByteArray *line = news_line(n, error);
        if (!line) { g_string_free(all, TRUE); return NULL; }
        if (line->len == 1 && line->data[0] == '.') { g_byte_array_unref(line); break; }
        gsize skip = line->len && line->data[0] == '.' ? 1 : 0;
        if (all->len + line->len > limit) {
            yenc_error(error, "Article exceeds the size limit for viewing"); g_byte_array_unref(line); g_string_free(all, TRUE); return NULL;
        }
        g_string_append_len(all, (const char *)line->data + skip, (gssize)(line->len - skip));
        g_string_append_c(all, '\n'); g_byte_array_unref(line);
    }
    if (out_len) *out_len = all->len;
    return g_string_free(all, FALSE);
}

/* Sends a message with POST. 'headers' are LF- or CRLF-separated header lines (no blank line); 'body' is text. */
static gboolean news_post(NewsConnection *n, const char *headers, const char *body, GError **error) {
    int code = news_command(n, "POST", error);
    if (code != 340) {
        if (!error || !*error) g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                           code == 440 ? "This server does not allow posting (440)" : "The server refused POST (%d)", code);
        return FALSE;
    }
    GString *wire = g_string_new(NULL);
    const char *blocks[2] = {headers, body};
    for (int part = 0; part < 2; ++part) {
        char **lines = g_strsplit(blocks[part], "\n", -1);
        guint count = g_strv_length(lines);
        if (count && !*lines[count - 1]) count--; /* trailing newline */
        for (guint i = 0; i < count; ++i) {
            char *line = lines[i];
            gsize len = strlen(line);
            if (len && line[len - 1] == '\r') line[--len] = '\0';
            if (*line == '.') g_string_append_c(wire, '.'); /* dot-stuffing */
            g_string_append(wire, line); g_string_append(wire, "\r\n");
        }
        g_strfreev(lines);
        if (part == 0) g_string_append(wire, "\r\n"); /* blank line ends the headers */
    }
    g_string_append(wire, ".\r\n");
    gboolean ok = g_output_stream_write_all(g_io_stream_get_output_stream(n->stream), wire->str, wire->len, NULL, n->cancel, error);
    g_string_free(wire, TRUE);
    if (!ok) return FALSE;
    code = news_response(n, error);
    if (code != 240) {
        if (!error || !*error) g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The server did not accept the article (%d)", code);
        return FALSE;
    }
    return TRUE;
}

/* ---- parsing ---- */

/* Value of a header (case-insensitive name), with folded continuation lines joined. Caller frees. */
static char *article_header(const char *headers, const char *name) {
    gsize nlen = strlen(name);
    const char *p = headers;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        gsize linelen = eol ? (gsize)(eol - p) : strlen(p);
        if (linelen > nlen && p[nlen] == ':' && g_ascii_strncasecmp(p, name, nlen) == 0) {
            GString *v = g_string_new(NULL);
            g_string_append_len(v, p + nlen + 1, (gssize)(linelen - nlen - 1));
            const char *q = eol ? eol + 1 : NULL;
            while (q && (*q == ' ' || *q == '\t')) { /* folded continuation */
                const char *e2 = strchr(q, '\n');
                gsize l2 = e2 ? (gsize)(e2 - q) : strlen(q);
                g_string_append_c(v, ' '); g_string_append_len(v, q, (gssize)l2);
                q = e2 ? e2 + 1 : NULL;
            }
            char *out = g_strstrip(g_string_free(v, FALSE));
            return out;
        }
        p = eol ? eol + 1 : NULL;
    }
    return NULL;
}

/* A parameter of a structured header such as Content-Type: boundary="x", charset=..., name=... Caller frees. */
static char *article_param(const char *value, const char *param) {
    if (!value) return NULL;
    gsize plen = strlen(param);
    for (const char *p = value; (p = strchr(p, ';')); ) {
        ++p; while (*p == ' ' || *p == '\t') ++p;
        if (g_ascii_strncasecmp(p, param, plen) == 0 && p[plen] == '=') {
            const char *v = p + plen + 1;
            if (*v == '"') { ++v; const char *e = strchr(v, '"'); return e ? g_strndup(v, (gsize)(e - v)) : g_strdup(v); }
            gsize l = strcspn(v, "; \t"); return g_strndup(v, l);
        }
    }
    return NULL;
}

static GBytes *qp_decode_bytes(const char *s, gsize len) {
    GByteArray *out = g_byte_array_new();
    for (gsize i = 0; i < len; ++i) {
        if (s[i] == '=' && i + 1 < len && s[i + 1] == '\n') { ++i; continue; } /* soft line break */
        if (s[i] == '=' && i + 2 < len && g_ascii_isxdigit(s[i + 1]) && g_ascii_isxdigit(s[i + 2])) {
            guint8 b = (guint8)((g_ascii_xdigit_value(s[i + 1]) << 4) | g_ascii_xdigit_value(s[i + 2]));
            g_byte_array_append(out, &b, 1); i += 2; continue;
        }
        g_byte_array_append(out, (const guint8 *)s + i, 1);
    }
    return g_byte_array_free_to_bytes(out);
}

/* Applies a Content-Transfer-Encoding to a body section. */
static GBytes *article_decode_cte(const char *cte, const char *body, gsize len) {
    if (cte && g_ascii_strncasecmp(cte, "base64", 6) == 0) {
        gsize n = 0; char *copy = g_strndup(body, len);
        guchar *raw = g_base64_decode(copy, &n); g_free(copy);
        return g_bytes_new_take(raw, n);
    }
    if (cte && g_ascii_strncasecmp(cte, "quoted-printable", 16) == 0) return qp_decode_bytes(body, len);
    return g_bytes_new(body, len);
}

/* Text to valid UTF-8: honour the declared charset, otherwise treat stray bytes as Windows-1252. */
static char *article_text_utf8(GBytes *bytes, const char *charset) {
    gsize len = 0; const char *data = g_bytes_get_data(bytes, &len);
    if (!len) return g_strdup("");
    if (charset && g_ascii_strcasecmp(charset, "utf-8") != 0 && g_ascii_strcasecmp(charset, "us-ascii") != 0) {
        char *converted = g_convert(data, (gssize)len, "UTF-8", charset, NULL, NULL, NULL);
        if (converted) return converted;
    }
    if (g_utf8_validate(data, (gssize)len, NULL)) return g_strndup(data, len);
    char *converted = g_convert(data, (gssize)len, "UTF-8", "CP1252", NULL, NULL, NULL);
    return converted ? converted : g_utf8_make_valid(data, (gssize)len);
}

/* Minimal single-part yEnc decoder for an inline image: returns the decoded bytes or NULL. */
static GBytes *article_decode_yenc(const char *body, char **name_out, gboolean *crc_ok) {
    const char *begin = strstr(body, "=ybegin ");
    if (!begin || (begin != body && begin[-1] != '\n')) return NULL;
    const char *eol = strchr(begin, '\n');
    if (!eol) return NULL;
    char *header = g_strndup(begin, (gsize)(eol - begin));
    const char *name = strstr(header, " name=");
    if (name_out) *name_out = name ? g_strstrip(g_strdup(name + 6)) : NULL;
    const char *data = eol + 1;
    if (g_str_has_prefix(data, "=ypart ")) { const char *e2 = strchr(data, '\n'); if (!e2) { g_free(header); return NULL; } data = e2 + 1; }
    GByteArray *out = g_byte_array_new();
    guint32 expected_crc = 0; gboolean have_crc = FALSE;
    for (const char *p = data; *p; ) {
        const char *le = strchr(p, '\n'); gsize ll = le ? (gsize)(le - p) : strlen(p);
        if (g_str_has_prefix(p, "=yend")) {
            char *end_line = g_strndup(p, ll);
            const char *c = strstr(end_line, "crc32=");
            if (c) { expected_crc = (guint32)g_ascii_strtoull(c + 6, NULL, 16); have_crc = TRUE; }
            g_free(end_line); break;
        }
        for (gsize i = 0; i < ll; ++i) {
            guint8 ch = (guint8)p[i];
            if (ch == '\r') continue;
            if (ch == '=' && i + 1 < ll) ch = (guint8)(p[++i] - 64);
            ch = (guint8)(ch - 42);
            g_byte_array_append(out, &ch, 1);
        }
        p = le ? le + 1 : p + ll;
    }
    g_free(header);
    if (crc_ok) *crc_ok = !have_crc || (guint32)crc32(crc32(0, NULL, 0), out->data, out->len) == expected_crc;
    return g_byte_array_free_to_bytes(out);
}

static gboolean article_looks_like_image(GBytes *bytes) {
    gsize n = 0; const guint8 *d = g_bytes_get_data(bytes, &n);
    return n > 4 && ((d[0] == 0xFF && d[1] == 0xD8) || !memcmp(d, "\x89PNG", 4) || !memcmp(d, "GIF8", 4) ||
                     !memcmp(d, "BM", 2) || !memcmp(d, "II*\0", 4) || !memcmp(d, "MM\0*", 4) ||
                     (n > 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)));
}

static void article_note(ArticleContent *a, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void article_note(ArticleContent *a, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); char *msg = g_strdup_vprintf(fmt, ap); va_end(ap);
    char *joined = a->note ? g_strconcat(a->note, "\n", msg, NULL) : g_strdup(msg);
    g_free(msg); g_free(a->note); a->note = joined;
}

/* Handles one MIME entity (headers + body) and adds its text or its first image to 'out'. Depth-limited. */
static void article_walk_entity(ArticleContent *out, const char *headers, const char *body, gsize body_len, int depth) {
    char *ctype = article_header(headers, "content-type");
    char *cte = article_header(headers, "content-transfer-encoding");
    char *disp = article_header(headers, "content-disposition");
    gboolean multipart = ctype && g_ascii_strncasecmp(ctype, "multipart/", 10) == 0;
    if (multipart && depth < 4) {
        char *boundary = article_param(ctype, "boundary");
        if (boundary) {
            char *marker = g_strconcat("\n--", boundary, NULL);
            char *raw_body = g_strndup(body, body_len);
            char *copy = g_strconcat("\n", raw_body, NULL);
            g_free(raw_body);
            const char *p = strstr(copy, marker);
            while (p) {
                p += strlen(marker);
                if (p[0] == '-' && p[1] == '-') break; /* closing delimiter */
                const char *start = strchr(p, '\n'); if (!start) break; ++start;
                const char *next = strstr(start, marker);
                gsize plen = next ? (gsize)(next - start) : strlen(start);
                char *part = g_strndup(start, plen);
                const char *sep = strstr(part, "\n\n");
                if (part[0] == '\n') article_walk_entity(out, "", part + 1, plen - 1, depth + 1);
                else if (sep) { char *h = g_strndup(part, (gsize)(sep - part)); article_walk_entity(out, h, sep + 2, strlen(sep + 2), depth + 1); g_free(h); }
                g_free(part);
                p = next;
            }
            g_free(copy); g_free(marker);
        }
        g_free(boundary);
    } else if (ctype && g_ascii_strncasecmp(ctype, "image/", 6) == 0) {
        GBytes *bytes = article_decode_cte(cte, body, body_len);
        if (!out->image && article_looks_like_image(bytes)) {
            out->image = bytes;
            char *name = article_param(ctype, "name"); if (!name) name = article_param(disp, "filename");
            out->image_name = name ? name : g_strdup("image");
        } else { article_note(out, "[another image attachment was not shown]"); g_bytes_unref(bytes); }
    } else if (!ctype || g_ascii_strncasecmp(ctype, "text/", 5) == 0) {
        char *charset = article_param(ctype, "charset");
        GBytes *bytes = article_decode_cte(cte, body, body_len);
        char *text = article_text_utf8(bytes, charset);
        char *joined = out->text && *out->text ? g_strconcat(out->text, "\n", text, NULL) : g_strdup(text);
        g_free(out->text); out->text = joined; g_free(text); g_bytes_unref(bytes); g_free(charset);
    } else {
        char *name = article_param(ctype, "name"); if (!name) name = article_param(disp, "filename");
        article_note(out, "[attachment %s (%s) not shown]", name ? name : "(unnamed)", ctype ? ctype : "unknown type");
        g_free(name);
    }
    g_free(ctype); g_free(cte); g_free(disp);
}

/* Splits a raw article into headers and body and extracts text and the first image. */
static ArticleContent *article_parse(const char *raw, gsize len) {
    ArticleContent *a = g_new0(ArticleContent, 1);
    const char *sep = strstr(raw, "\n\n");
    gsize hlen = sep ? (gsize)(sep - raw) : len;
    a->headers = g_strndup(raw, hlen);
    const char *body = sep ? sep + 2 : raw + len;
    a->subject = article_header(a->headers, "subject"); a->from = article_header(a->headers, "from");
    a->date = article_header(a->headers, "date"); a->newsgroups = article_header(a->headers, "newsgroups");
    a->message_id = article_header(a->headers, "message-id"); a->references = article_header(a->headers, "references");
    a->text = g_strdup("");
    gboolean crc_ok = TRUE; char *yname = NULL;
    GBytes *yimg = strstr(body, "=ybegin ") ? article_decode_yenc(body, &yname, &crc_ok) : NULL;
    if (yimg) { /* a yEnc body: the file, if it is an image, is the content */
        if (article_looks_like_image(yimg)) {
            a->image = yimg; a->image_name = yname ? yname : g_strdup("image");
            if (!crc_ok) article_note(a, "[warning: the yEnc checksum did not match; the image may be damaged]");
        } else {
            article_note(a, "[yEnc data%s%s is not an image or is only part of a file; use Download file / View image]", yname ? ": " : "", yname ? yname : "");
            g_bytes_unref(yimg); g_free(yname);
        }
    } else article_walk_entity(a, a->headers, body, strlen(body), 0);
    if (!a->text) a->text = g_strdup("");
    return a;
}

/* ---- composing a reply ---- */

/* RFC 2047 encoded-word form for header text that is not plain ASCII; plain ASCII is returned unchanged. */
static char *article_encode_header(const char *text) {
    gboolean ascii = TRUE;
    for (const char *p = text; *p; ++p) if ((guchar)*p >= 0x80) { ascii = FALSE; break; }
    if (ascii) return g_strdup(text);
    GString *out = g_string_new(NULL);
    const char *p = text;
    while (*p) {
        const char *q = p; gsize raw = 0;
        while (*q && raw < 36) { const char *nx = g_utf8_next_char(q); raw += (gsize)(nx - q); q = nx; } /* whole characters only */
        char *b64 = g_base64_encode((const guchar *)p, (gsize)(q - p));
        if (out->len) g_string_append_c(out, ' ');
        g_string_append_printf(out, "=?UTF-8?B?%s?=", b64); g_free(b64);
        p = q;
    }
    return g_string_free(out, FALSE);
}

/* "Name <addr>" with only the display name encoded. */
static char *article_encode_from(const char *from) {
    const char *lt = strrchr(from, '<');
    if (!lt) return article_encode_header(from);
    char *phrase = g_strstrip(g_strndup(from, (gsize)(lt - from)));
    char *enc = article_encode_header(phrase);
    char *out = *enc ? g_strdup_printf("%s %s", enc, lt) : g_strdup(lt);
    g_free(phrase); g_free(enc); return out;
}

static char *article_single_line(const char *s) { /* a header value must never contain a line break */
    char *copy = g_strdup(s ? s : "");
    for (char *p = copy; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
    return g_strstrip(copy);
}

/* "Re: " + subject, without stacking Re: Re: */
static char *article_reply_subject(const char *subject) {
    const char *s = subject ? subject : "";
    while (*s == ' ') ++s;
    if (g_ascii_strncasecmp(s, "re:", 3) == 0) return g_strdup(s);
    return g_strconcat("Re: ", s, NULL);
}

/* References for a reply: the parent's References followed by the parent's Message-ID (trimmed to 25 ids). */
static char *article_reply_references(const char *parent_refs, const char *parent_id) {
    GString *r = g_string_new(parent_refs ? parent_refs : "");
    g_strstrip(r->str); g_string_set_size(r, strlen(r->str));
    if (parent_id && *parent_id) { if (r->len) g_string_append_c(r, ' '); g_string_append(r, parent_id); }
    guint ids = 0; for (const char *p = r->str; *p; ++p) if (*p == '<') ids++;
    while (ids > 25) { const char *second = strchr(r->str + 1, '<'); if (!second) break; g_string_erase(r, 0, (gssize)(second - r->str)); ids--; }
    return g_string_free(r, FALSE);
}

/* The quoted original, ready for the reply editor. */
static char *article_reply_body(const char *author, const char *date, const char *original) {
    GString *b = g_string_new(NULL);
    g_string_append_printf(b, "On %s, %s wrote:\n", date && *date ? date : "(unknown date)", author && *author ? author : "someone");
    char **lines = g_strsplit(original ? original : "", "\n", -1);
    guint count = 0;
    for (char **l = lines; *l && count < 300; ++l, ++count) {
        if (!strcmp(*l, "-- ")) break; /* stop at the signature */
        g_string_append(b, **l == '>' ? ">" : "> "); g_string_append(b, *l); g_string_append_c(b, '\n');
    }
    g_strfreev(lines);
    g_string_append(b, "\n");
    return g_string_free(b, FALSE);
}

/* Header block for POST (LF-separated, no trailing blank line). Returns NULL and sets 'problem' if invalid. */
static char *article_build_headers(const char *from, const char *newsgroups, const char *subject, const char *references, char **problem) {
    char *f = article_single_line(from), *g = article_single_line(newsgroups), *s = article_single_line(subject);
    char *out = NULL;
    if (!*f || !strchr(f, '@')) *problem = g_strdup("Enter your From address (name <you@example.com>).");
    else if (!*g) *problem = g_strdup("Enter at least one newsgroup.");
    else if (!*s) *problem = g_strdup("Enter a subject.");
    else {
        char *ef = article_encode_from(f), *es = article_encode_header(s);
        GString *h = g_string_new(NULL);
        g_string_append_printf(h, "From: %s\nNewsgroups: %s\nSubject: %s\n", ef, g, es);
        if (references && *references) { char *r = article_single_line(references); g_string_append_printf(h, "References: %s\n", r); g_free(r); }
        g_string_append(h, "MIME-Version: 1.0\nContent-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\nUser-Agent: isotope-viewer (nntp2sql)\n");
        out = g_string_free(h, FALSE); g_free(ef); g_free(es);
    }
    g_free(f); g_free(g); g_free(s);
    return out;
}
#endif
