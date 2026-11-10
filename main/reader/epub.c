#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "doc_internal.h"
#include "esp_log.h"
#include "rmem.h"
#include "text.h"
#include "zip.h"

static const char *TAG = "epub";

#define EPUB_MAX_MANIFEST 1024
#define EPUB_MAX_CHAPTERS 512
#define EPUB_MAX_PATH     256

typedef struct {
    char id[64];
    char href[EPUB_MAX_PATH];
    char media_type[64];
    char properties[80];
} manifest_item_t;

typedef struct {
    char href[EPUB_MAX_PATH]; /* full path inside the ZIP */
    char title[160];
} epub_chapter_t;

typedef struct {
    zip_t *zip;
    epub_chapter_t *chapters;
    int chapter_count;
} epub_impl_t;

/* ------------------------------------------------------------- strings --- */

static const char *find_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0) {
        return hay;
    }
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return p;
        }
    }
    return NULL;
}

/* Find an opening tag by name, requiring whitespace, '>' or '/' after it so
 * "<item" does not match "<itemref". */
static const char *find_tag(const char *p, const char *name)
{
    size_t n = strlen(name);
    while ((p = find_ci(p, name)) != NULL) {
        const char *q = p + n;
        if (*q == '\0' || isspace((unsigned char)*q) || *q == '>' || *q == '/') {
            return p;
        }
        p = q;
    }
    return NULL;
}

static const char *find_tag_end(const char *p)
{
    char quote = 0;
    while (*p) {
        if (quote) {
            if (*p == quote) {
                quote = 0;
            }
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '>') {
            return p;
        }
        p++;
    }
    return NULL;
}

static bool attr_value(const char *tag, const char *tag_end, const char *name, char *out,
                       size_t out_len)
{
    size_t n = strlen(name);
    const char *p = tag;
    while (p < tag_end) {
        const char *hit = find_ci(p, name);
        if (!hit || hit >= tag_end) {
            return false;
        }
        const char *q = hit + n;
        while (q < tag_end && isspace((unsigned char)*q)) {
            q++;
        }
        if (q < tag_end && *q == '=') {
            q++;
            while (q < tag_end && isspace((unsigned char)*q)) {
                q++;
            }
            if (q < tag_end && (*q == '"' || *q == '\'')) {
                char quote = *q++;
                const char *val = q;
                while (q < tag_end && *q != quote) {
                    q++;
                }
                size_t len = (size_t)(q - val);
                if (len >= out_len) {
                    len = out_len - 1;
                }
                memcpy(out, val, len);
                out[len] = '\0';
                return true;
            }
        }
        p = hit + n;
    }
    return false;
}

static void xml_unescape(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '&') {
            char *semi = strchr(r, ';');
            if (semi && semi - r < 12) {
                char ent[16];
                size_t n = (size_t)(semi - r - 1);
                memcpy(ent, r + 1, n);
                ent[n] = '\0';
                const char *rep = NULL;
                char numbuf[8];
                if (strcmp(ent, "amp") == 0) {
                    rep = "&";
                } else if (strcmp(ent, "lt") == 0) {
                    rep = "<";
                } else if (strcmp(ent, "gt") == 0) {
                    rep = ">";
                } else if (strcmp(ent, "quot") == 0) {
                    rep = "\"";
                } else if (strcmp(ent, "apos") == 0) {
                    rep = "'";
                } else if (strcmp(ent, "nbsp") == 0) {
                    rep = " ";
                } else if (ent[0] == '#') {
                    long cp = 0;
                    if (ent[1] == 'x' || ent[1] == 'X') {
                        cp = strtol(ent + 2, NULL, 16);
                    } else {
                        cp = strtol(ent + 1, NULL, 10);
                    }
                    if (cp > 0 && cp <= 0x10FFFF) {
                        if (cp <= 0x7F) {
                            numbuf[0] = (char)cp;
                            numbuf[1] = '\0';
                        } else if (cp <= 0x7FF) {
                            numbuf[0] = (char)(0xC0 | (cp >> 6));
                            numbuf[1] = (char)(0x80 | (cp & 0x3F));
                            numbuf[2] = '\0';
                        } else if (cp <= 0xFFFF) {
                            numbuf[0] = (char)(0xE0 | (cp >> 12));
                            numbuf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            numbuf[2] = (char)(0x80 | (cp & 0x3F));
                            numbuf[3] = '\0';
                        } else {
                            numbuf[0] = (char)(0xF0 | (cp >> 18));
                            numbuf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            numbuf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            numbuf[3] = (char)(0x80 | (cp & 0x3F));
                            numbuf[4] = '\0';
                        }
                        rep = numbuf;
                    }
                }
                if (rep) {
                    size_t rl = strlen(rep);
                    memcpy(w, rep, rl);
                    w += rl;
                    r = semi + 1;
                    continue;
                }
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static void trim(char *s)
{
    if (!s) {
        return;
    }
    char *p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

/* ------------------------------------------------------------- paths ---- */

static void url_decode(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void path_dirname(const char *path, char *out, size_t out_len)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        out[0] = '\0';
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static void resolve_href(const char *base_dir, const char *href, char *out, size_t out_len)
{
    char ref[EPUB_MAX_PATH];
    snprintf(ref, sizeof(ref), "%s", href);
    char *frag = strpbrk(ref, "#?");
    if (frag) {
        *frag = '\0';
    }
    url_decode(ref);

    char combined[EPUB_MAX_PATH * 2];
    if (ref[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", ref + 1);
    } else if (base_dir[0]) {
        snprintf(combined, sizeof(combined), "%s/%s", base_dir, ref);
    } else {
        snprintf(combined, sizeof(combined), "%s", ref);
    }

    /* Collapse ".", ".." and duplicate slashes. */
    char temp[EPUB_MAX_PATH * 2];
    snprintf(temp, sizeof(temp), "%s", combined);
    char *parts[128];
    int nparts = 0;
    char *save = NULL;
    for (char *tok = strtok_r(temp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (nparts > 0) {
                nparts--;
            }
            continue;
        }
        if (nparts < 128) {
            parts[nparts++] = tok;
        }
    }
    out[0] = '\0';
    for (int i = 0; i < nparts; i++) {
        size_t used = strlen(out);
        snprintf(out + used, out_len - used, "%s%s", i ? "/" : "", parts[i]);
    }
}

/* ------------------------------------------------------------- html ----- */

static bool is_block_tag(const char *name, size_t n)
{
    static const char *blocks[] = {
        "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr", "blockquote",
        "section", "article", "header", "footer", "nav", "table", "ul", "ol", "dl", "dt",
        "dd", "pre", "hr", "figure", "figcaption", "aside", "main", "title",
    };
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        if (strlen(blocks[i]) == n && strncasecmp(name, blocks[i], n) == 0) {
            return true;
        }
    }
    return false;
}

static void out_trim_spaces(char *out, size_t *len)
{
    while (*len > 0 && (out[*len - 1] == ' ' || out[*len - 1] == '\t')) {
        (*len)--;
    }
    out[*len] = '\0';
}

static void out_sep(char *out, size_t *len, size_t cap, int n)
{
    out_trim_spaces(out, len);
    int have = 0;
    for (size_t i = *len; i > 0 && out[i - 1] == '\n'; i--) {
        have++;
    }
    while (have < n && *len + 1 < cap) {
        out[(*len)++] = '\n';
        have++;
    }
    out[*len] = '\0';
}

static void out_char(char *out, size_t *len, size_t cap, char c)
{
    if (*len + 1 < cap) {
        out[(*len)++] = c;
        out[*len] = '\0';
    }
}

static char *html_to_text(const char *html, size_t html_len)
{
    if (html_len >= 3 && (uint8_t)html[0] == 0xEF && (uint8_t)html[1] == 0xBB &&
        (uint8_t)html[2] == 0xBF) {
        html += 3;
        html_len -= 3;
    }
    size_t cap = html_len * 2 + 2048;
    char *out = rmalloc(cap);
    if (!out) {
        return NULL;
    }
    size_t olen = 0;
    out[0] = '\0';
    bool pending_space = false;
    const char *p = html;
    const char *end = html + html_len;

    while (p < end) {
        if (*p != '<') {
            unsigned char c = (unsigned char)*p++;
            if (c == '&') {
                /* Decode one entity. */
                const char *semi = memchr(p, ';', (size_t)(end - p));
                if (semi && semi - p < 12) {
                    char ent[16];
                    size_t n = (size_t)(semi - p);
                    memcpy(ent, p, n);
                    ent[n] = '\0';
                    char decoded[8] = {0};
                    if (strcmp(ent, "amp") == 0) {
                        strcpy(decoded, "&");
                    } else if (strcmp(ent, "lt") == 0) {
                        strcpy(decoded, "<");
                    } else if (strcmp(ent, "gt") == 0) {
                        strcpy(decoded, ">");
                    } else if (strcmp(ent, "quot") == 0) {
                        strcpy(decoded, "\"");
                    } else if (strcmp(ent, "apos") == 0) {
                        strcpy(decoded, "'");
                    } else if (strcmp(ent, "nbsp") == 0) {
                        strcpy(decoded, " ");
                    } else if (strcmp(ent, "mdash") == 0) {
                        strcpy(decoded, "—");
                    } else if (strcmp(ent, "ndash") == 0) {
                        strcpy(decoded, "–");
                    } else if (strcmp(ent, "hellip") == 0) {
                        strcpy(decoded, "…");
                    } else if (strcmp(ent, "ldquo") == 0) {
                        strcpy(decoded, "“");
                    } else if (strcmp(ent, "rdquo") == 0) {
                        strcpy(decoded, "”");
                    } else if (strcmp(ent, "lsquo") == 0) {
                        strcpy(decoded, "‘");
                    } else if (strcmp(ent, "rsquo") == 0) {
                        strcpy(decoded, "’");
                    } else if (strcmp(ent, "bull") == 0) {
                        strcpy(decoded, "•");
                    } else if (ent[0] == '#') {
                        long cp = (ent[1] == 'x' || ent[1] == 'X') ? strtol(ent + 2, NULL, 16)
                                                                   : strtol(ent + 1, NULL, 10);
                        if (cp > 0 && cp <= 0x10FFFF) {
                            if (cp <= 0x7F) {
                                decoded[0] = (char)cp;
                            } else if (cp <= 0x7FF) {
                                decoded[0] = (char)(0xC0 | (cp >> 6));
                                decoded[1] = (char)(0x80 | (cp & 0x3F));
                            } else if (cp <= 0xFFFF) {
                                decoded[0] = (char)(0xE0 | (cp >> 12));
                                decoded[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                decoded[2] = (char)(0x80 | (cp & 0x3F));
                            } else {
                                decoded[0] = (char)(0xF0 | (cp >> 18));
                                decoded[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                                decoded[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                decoded[3] = (char)(0x80 | (cp & 0x3F));
                            }
                        }
                    }
                    if (decoded[0]) {
                        for (char *d = decoded; *d; d++) {
                            if (*d == ' ') {
                                pending_space = true;
                            } else {
                                if (pending_space && olen > 0 && out[olen - 1] != '\n' &&
                                    out[olen - 1] != ' ') {
                                    out_char(out, &olen, cap, ' ');
                                }
                                pending_space = false;
                                out_char(out, &olen, cap, *d);
                            }
                        }
                        p = semi + 1;
                        continue;
                    }
                }
                /* Unknown entity: keep the ampersand literally. */
                if (pending_space && olen > 0 && out[olen - 1] != '\n') {
                    out_char(out, &olen, cap, ' ');
                }
                pending_space = false;
                out_char(out, &olen, cap, '&');
                continue;
            }
            if (isspace(c)) {
                pending_space = true;
                continue;
            }
            if (pending_space && olen > 0 && out[olen - 1] != '\n' && out[olen - 1] != ' ') {
                out_char(out, &olen, cap, ' ');
            }
            pending_space = false;
            out_char(out, &olen, cap, (char)c);
            continue;
        }

        /* A tag. */
        if (end - p >= 4 && strncmp(p, "<!--", 4) == 0) {
            const char *close = strstr(p + 4, "-->");
            p = close ? close + 3 : end;
            continue;
        }
        if (end - p >= 2 && strncmp(p, "<?", 2) == 0) {
            const char *close = strstr(p + 2, "?>");
            p = close ? close + 2 : end;
            continue;
        }
        const char *tag_end = find_tag_end(p);
        if (!tag_end) {
            break;
        }
        bool closing = (p[1] == '/');
        const char *name = p + (closing ? 2 : 1);
        while (name < tag_end && !isalnum((unsigned char)*name) && *name != ':' && *name != '_') {
            name++;
        }
        const char *name_end = name;
        while (name_end < tag_end &&
               (isalnum((unsigned char)*name_end) || *name_end == ':' || *name_end == '-' ||
                *name_end == '_')) {
            name_end++;
        }
        size_t name_len = (size_t)(name_end - name);
        const char *colon = memchr(name, ':', name_len);
        if (colon) {
            name = colon + 1;
            name_len = (size_t)(name_end - name);
        }

        if (!closing && name_len == 4 && strncasecmp(name, "head", 4) == 0) {
            const char *close = find_ci(tag_end, "</head");
            p = close ? close + 7 : tag_end + 1;
            continue;
        }
        if (!closing && name_len == 6 && strncasecmp(name, "script", 6) == 0) {
            const char *close = find_ci(tag_end, "</script");
            p = close ? close + 9 : tag_end + 1;
            continue;
        }
        if (!closing && name_len == 5 && strncasecmp(name, "style", 5) == 0) {
            const char *close = find_ci(tag_end, "</style");
            p = close ? close + 8 : tag_end + 1;
            continue;
        }

        if (!closing && name_len == 2 && strncasecmp(name, "br", 2) == 0) {
            out_sep(out, &olen, cap, 1);
        } else if (!closing && name_len == 2 && strncasecmp(name, "hr", 2) == 0) {
            out_sep(out, &olen, cap, 1);
            for (int i = 0; i < 12; i++) {
                out_char(out, &olen, cap, '-');
            }
            out_sep(out, &olen, cap, 1);
        } else if (!closing && name_len == 2 && strncasecmp(name, "li", 2) == 0) {
            out_sep(out, &olen, cap, 1);
            const char *bullet = "• ";
            for (const char *b = bullet; *b; b++) {
                out_char(out, &olen, cap, *b);
            }
        } else if (closing && name_len == 2 && strncasecmp(name, "li", 2) == 0) {
            out_sep(out, &olen, cap, 1);
        } else if (!closing && name_len == 2 && strncasecmp(name, "td", 2) == 0) {
            out_char(out, &olen, cap, ' ');
        } else if (!closing && name_len == 2 && strncasecmp(name, "tr", 2) == 0) {
            out_sep(out, &olen, cap, 1);
        } else if (is_block_tag(name, name_len)) {
            out_sep(out, &olen, cap, 2);
        }

        p = tag_end + 1;
    }

    out_trim_spaces(out, &olen);
    while (olen > 0 && out[olen - 1] == '\n') {
        olen--;
    }
    out[olen] = '\0';
    return out;
}

/* ------------------------------------------------------------- toc ------ */

static int find_chapter_by_href(const epub_impl_t *impl, const char *href)
{
    for (int i = 0; i < impl->chapter_count; i++) {
        if (strcmp(impl->chapters[i].href, href) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_chapter_title(epub_impl_t *impl, const char *href, const char *title)
{
    int idx = find_chapter_by_href(impl, href);
    if (idx < 0) {
        return;
    }
    char clean[160];
    snprintf(clean, sizeof(clean), "%s", title);
    xml_unescape(clean);
    trim(clean);
    if (clean[0]) {
        snprintf(impl->chapters[idx].title, sizeof(impl->chapters[idx].title), "%s", clean);
    }
}

static void parse_ncx(epub_impl_t *impl, const char *ncx, size_t len, const char *ncx_dir)
{
    char *copy = rmalloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, ncx, len);
    copy[len] = '\0';

    const char *p = copy;
    while ((p = find_tag(p, "<navpoint")) != NULL) {
        const char *tag_end = find_tag_end(p);
        if (!tag_end) {
            break;
        }
        const char *nav_end = find_ci(tag_end, "</navpoint");
        if (!nav_end) {
            nav_end = copy + len;
        }
        const char *text = find_tag(tag_end, "<text");
        const char *content = find_tag(tag_end, "<content");
        if (text && content && text < nav_end) {
            const char *t_end = find_tag_end(text);
            const char *t_close = find_ci(t_end ? t_end : text, "</text");
            if (t_end && t_close) {
                char title[192];
                size_t n = (size_t)(t_close - (t_end + 1));
                if (n >= sizeof(title)) {
                    n = sizeof(title) - 1;
                }
                memcpy(title, t_end + 1, n);
                title[n] = '\0';

                const char *c_end = find_tag_end(content);
                char src[EPUB_MAX_PATH];
                if (c_end && attr_value(content, c_end, "src", src, sizeof(src))) {
                    char full[EPUB_MAX_PATH];
                    resolve_href(ncx_dir, src, full, sizeof(full));
                    set_chapter_title(impl, full, title);
                }
            }
        }
        p = nav_end + 10;
    }
    rfree(copy);
}

static void parse_nav(epub_impl_t *impl, const char *nav, size_t len, const char *nav_dir)
{
    char *copy = rmalloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, nav, len);
    copy[len] = '\0';

    const char *p = copy;
    while ((p = find_tag(p, "<a")) != NULL) {
        const char *tag_end = find_tag_end(p);
        if (!tag_end) {
            break;
        }
        const char *text_end = find_ci(tag_end, "</a");
        char href[EPUB_MAX_PATH];
        if (!text_end || !attr_value(p, tag_end, "href", href, sizeof(href))) {
            p = tag_end + 1;
            continue;
        }
        char title[192];
        size_t n = (size_t)(text_end - (tag_end + 1));
        if (n >= sizeof(title)) {
            n = sizeof(title) - 1;
        }
        memcpy(title, tag_end + 1, n);
        title[n] = '\0';
        /* Drop nested tags in the label. */
        for (char *t = title; *t; t++) {
            if (*t == '<') {
                char *e = strchr(t, '>');
                if (!e) {
                    *t = '\0';
                    break;
                }
                memmove(t, e + 1, strlen(e + 1) + 1);
                t--;
            }
        }
        char full[EPUB_MAX_PATH];
        resolve_href(nav_dir, href, full, sizeof(full));
        set_chapter_title(impl, full, title);
        p = text_end + 3;
    }
    rfree(copy);
}

/* ------------------------------------------------------------- open ------ */

static esp_err_t read_zip_text(zip_t *zip, const char *name, char **out, size_t *len)
{
    uint8_t *data = NULL;
    size_t size = 0;
    esp_err_t err = zip_read(zip, name, &data, &size);
    if (err != ESP_OK) {
        return err;
    }
    char *copy = rmalloc(size + 1);
    if (!copy) {
        rfree(data);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, data, size);
    copy[size] = '\0';
    rfree(data);
    *out = copy;
    *len = size;
    return ESP_OK;
}

static void free_chapters(epub_impl_t *impl)
{
    free(impl->chapters);
    impl->chapters = NULL;
    impl->chapter_count = 0;
}

static esp_err_t parse_opf(epub_impl_t *impl, const char *opf, size_t opf_len,
                           const char *opf_path, doc_info_t *info)
{
    char opf_dir[EPUB_MAX_PATH];
    path_dirname(opf_path, opf_dir, sizeof(opf_dir));

    manifest_item_t *manifest = calloc(EPUB_MAX_MANIFEST, sizeof(manifest_item_t));
    if (!manifest) {
        return ESP_ERR_NO_MEM;
    }
    int manifest_count = 0;
    const char *p = opf;
    while ((p = find_tag(p, "<item")) != NULL) {
        const char *tag_end = find_tag_end(p);
        if (!tag_end) {
            break;
        }
        if (p > opf && p[-1] == '/') { /* </item> */
            p = tag_end + 1;
            continue;
        }
        manifest_item_t *it = &manifest[manifest_count];
        if (attr_value(p, tag_end, "id", it->id, sizeof(it->id)) &&
            attr_value(p, tag_end, "href", it->href, sizeof(it->href))) {
            attr_value(p, tag_end, "media-type", it->media_type, sizeof(it->media_type));
            attr_value(p, tag_end, "properties", it->properties, sizeof(it->properties));
            manifest_count++;
            if (manifest_count >= EPUB_MAX_MANIFEST) {
                break;
            }
        }
        p = tag_end + 1;
    }

    /* Metadata. */
    const char *title = find_tag(opf, "<dc:title");
    if (title) {
        const char *te = find_tag_end(title);
        const char *tc = te ? find_ci(te, "</dc:title") : NULL;
        if (te && tc) {
            size_t n = (size_t)(tc - (te + 1));
            if (n >= sizeof(info->title)) {
                n = sizeof(info->title) - 1;
            }
            memcpy(info->title, te + 1, n);
            info->title[n] = '\0';
            xml_unescape(info->title);
            trim(info->title);
        }
    }
    const char *creator = find_tag(opf, "<dc:creator");
    if (creator) {
        const char *ce = find_tag_end(creator);
        const char *cc = ce ? find_ci(ce, "</dc:creator") : NULL;
        if (ce && cc) {
            size_t n = (size_t)(cc - (ce + 1));
            if (n >= sizeof(info->author)) {
                n = sizeof(info->author) - 1;
            }
            memcpy(info->author, ce + 1, n);
            info->author[n] = '\0';
            xml_unescape(info->author);
            trim(info->author);
        }
    }

    /* TOC id from the spine element. */
    char toc_id[64] = {0};
    const char *spine = find_tag(opf, "<spine");
    if (spine) {
        const char *se = find_tag_end(spine);
        if (se) {
            attr_value(spine, se, "toc", toc_id, sizeof(toc_id));
        }
    }

    /* Chapters, in spine order. */
    impl->chapters = calloc(EPUB_MAX_CHAPTERS, sizeof(epub_chapter_t));
    if (!impl->chapters) {
        free(manifest);
        return ESP_ERR_NO_MEM;
    }

    p = spine ? spine : opf;
    while ((p = find_tag(p, "<itemref")) != NULL) {
        const char *tag_end = find_tag_end(p);
        if (!tag_end) {
            break;
        }
        char idref[64];
        if (attr_value(p, tag_end, "idref", idref, sizeof(idref))) {
            bool linear = true;
            char linear_str[8];
            if (attr_value(p, tag_end, "linear", linear_str, sizeof(linear_str)) &&
                strcasecmp(linear_str, "no") == 0) {
                linear = false;
            }
            for (int i = 0; i < manifest_count; i++) {
                if (strcmp(manifest[i].id, idref) != 0) {
                    continue;
                }
                const char *mt = manifest[i].media_type;
                bool is_html = strstr(mt, "xhtml") || strstr(mt, "html") ||
                               strstr(manifest[i].href, ".xhtml") || strstr(manifest[i].href, ".html");
                if (!linear || !is_html) {
                    break;
                }
                char full[EPUB_MAX_PATH];
                resolve_href(opf_dir, manifest[i].href, full, sizeof(full));
                if (impl->chapter_count < EPUB_MAX_CHAPTERS) {
                    snprintf(impl->chapters[impl->chapter_count].href,
                             sizeof(impl->chapters[impl->chapter_count].href), "%s", full);
                    snprintf(impl->chapters[impl->chapter_count].title,
                             sizeof(impl->chapters[impl->chapter_count].title), "%s",
                             manifest[i].id);
                    impl->chapter_count++;
                }
                break;
            }
        }
        p = tag_end + 1;
    }

    /* If every itemref was linear="no" or non-HTML, fall back to HTML items. */
    if (impl->chapter_count == 0) {
        for (int i = 0; i < manifest_count && impl->chapter_count < EPUB_MAX_CHAPTERS; i++) {
            const char *mt = manifest[i].media_type;
            if (strstr(mt, "xhtml") || strstr(mt, "html")) {
                char full[EPUB_MAX_PATH];
                resolve_href(opf_dir, manifest[i].href, full, sizeof(full));
                snprintf(impl->chapters[impl->chapter_count].href,
                         sizeof(impl->chapters[impl->chapter_count].href), "%s", full);
                snprintf(impl->chapters[impl->chapter_count].title,
                         sizeof(impl->chapters[impl->chapter_count].title), "%s", manifest[i].id);
                impl->chapter_count++;
            }
        }
    }

    /* Table of contents: EPUB2 NCX and EPUB3 nav. */
    for (int i = 0; i < manifest_count; i++) {
        bool is_ncx = strstr(manifest[i].media_type, "dtbncx") ||
                      (toc_id[0] && strcmp(manifest[i].id, toc_id) == 0);
        bool is_nav = strstr(manifest[i].properties, "nav") != NULL;
        if (!is_ncx && !is_nav) {
            continue;
        }
        char ncx_path[EPUB_MAX_PATH];
        resolve_href(opf_dir, manifest[i].href, ncx_path, sizeof(ncx_path));
        char *ncx = NULL;
        size_t ncx_len = 0;
        if (read_zip_text(impl->zip, ncx_path, &ncx, &ncx_len) != ESP_OK) {
            continue;
        }
        char ncx_dir[EPUB_MAX_PATH];
        path_dirname(ncx_path, ncx_dir, sizeof(ncx_dir));
        if (is_ncx) {
            parse_ncx(impl, ncx, ncx_len, ncx_dir);
        }
        if (is_nav) {
            parse_nav(impl, ncx, ncx_len, ncx_dir);
        }
        rfree(ncx);
    }

    /* Sanitize titles and fill placeholders. */
    for (int i = 0; i < impl->chapter_count; i++) {
        trim(impl->chapters[i].title);
        if (!impl->chapters[i].title[0] || strcmp(impl->chapters[i].title, "nav") == 0) {
            snprintf(impl->chapters[i].title, sizeof(impl->chapters[i].title), "第 %d 章", i + 1);
        }
    }

    info->chapter_count = impl->chapter_count;
    info->reflow = true;
    free(manifest);
    return ESP_OK;
}

/* ------------------------------------------------------------- doc ------- */

static esp_err_t epub_get_title(const doc_t *doc, int index, char *buf, size_t buf_len)
{
    epub_impl_t *impl = doc->impl;
    if (index < 0 || index >= impl->chapter_count) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(buf, buf_len, "%s", impl->chapters[index].title);
    return ESP_OK;
}

static esp_err_t epub_get_text(const doc_t *doc, int index, char **text, size_t *len)
{
    epub_impl_t *impl = doc->impl;
    if (index < 0 || index >= impl->chapter_count) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *html = NULL;
    size_t html_len = 0;
    esp_err_t err = zip_read(impl->zip, impl->chapters[index].href, &html, &html_len);
    if (err != ESP_OK) {
        return err;
    }
    char *out = html_to_text((const char *)html, html_len);
    rfree(html);
    if (!out) {
        return ESP_ERR_NO_MEM;
    }
    /* Normalize again (collapses blank lines, trims). */
    size_t out_len = strlen(out);
    char *norm = text_normalize(out, out_len, &out_len);
    if (!norm) {
        return ESP_ERR_NO_MEM;
    }
    if (out_len == 0) {
        rfree(norm);
        const char *empty = "（本章没有可显示的文字）";
        norm = rmalloc(strlen(empty) + 1);
        if (!norm) {
            return ESP_ERR_NO_MEM;
        }
        strcpy(norm, empty);
        out_len = strlen(norm);
    }
    *text = norm;
    *len = out_len;
    return ESP_OK;
}

static void epub_close(doc_t *doc)
{
    epub_impl_t *impl = doc->impl;
    if (!impl) {
        return;
    }
    free_chapters(impl);
    zip_close(impl->zip);
    free(impl);
}

esp_err_t epub_doc_open(const char *path, doc_t **out)
{
    *out = NULL;
    zip_t *zip = NULL;
    esp_err_t err = zip_open(path, &zip);
    if (err != ESP_OK) {
        return err;
    }

    char *container = NULL;
    size_t container_len = 0;
    char opf_path[EPUB_MAX_PATH] = {0};
    if (read_zip_text(zip, "META-INF/container.xml", &container, &container_len) == ESP_OK) {
        const char *rf = find_tag(container, "<rootfile");
        if (rf) {
            const char *rfe = find_tag_end(rf);
            if (rfe) {
                attr_value(rf, rfe, "full-path", opf_path, sizeof(opf_path));
            }
        }
        rfree(container);
    }
    if (opf_path[0] == '\0') {
        /* Damaged container: use the first OPF in the archive. */
        for (int i = 0; i < zip_count(zip); i++) {
            const zip_entry_t *e = zip_entry_at(zip, i);
            size_t n = strlen(e->name);
            if (n > 4 && strcasecmp(e->name + n - 4, ".opf") == 0) {
                snprintf(opf_path, sizeof(opf_path), "%s", e->name);
                break;
            }
        }
    }
    if (opf_path[0] == '\0') {
        zip_close(zip);
        return ESP_ERR_NOT_FOUND;
    }

    char *opf = NULL;
    size_t opf_len = 0;
    err = read_zip_text(zip, opf_path, &opf, &opf_len);
    if (err != ESP_OK) {
        zip_close(zip);
        return err;
    }

    epub_impl_t *impl = calloc(1, sizeof(*impl));
    doc_t *doc = calloc(1, sizeof(*doc));
    if (!impl || !doc) {
        free(impl);
        free(doc);
        rfree(opf);
        zip_close(zip);
        return ESP_ERR_NO_MEM;
    }
    impl->zip = zip;
    doc->impl = impl;
    doc->info.type = DOC_TYPE_EPUB;
    doc->get_title = epub_get_title;
    doc->get_text = epub_get_text;
    doc->close = epub_close;

    err = parse_opf(impl, opf, opf_len, opf_path, &doc->info);
    rfree(opf);
    if (err != ESP_OK) {
        doc_close(doc);
        return err;
    }
    if (doc->info.title[0] == '\0') {
        text_path_stem(path, doc->info.title, sizeof(doc->info.title));
    }
    if (impl->chapter_count == 0) {
        doc_close(doc);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "%s: %d chapters, title=%s", path, impl->chapter_count, doc->info.title);
    *out = doc;
    return ESP_OK;
}
