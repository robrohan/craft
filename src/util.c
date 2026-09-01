/* craft - basic utilities: allocation, strings, vectors, hashmap */
#include "craft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

int error_count = 0;

/* --------------------------------------------------------------- allocation */
void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* --------------------------------------------------------------- diagnostics */
void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: *** ", program_name ? program_name : "craft");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputs("  Stop.\n", stderr);
    exit(2);
}

void warn(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", program_name ? program_name : "craft");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void craft_error(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    if (file)
        fprintf(stderr, "%s:%d: ", file, line);
    else
        fprintf(stderr, "%s: ", program_name ? program_name : "craft");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    error_count++;
}

/* --------------------------------------------------------------- string bits */
int is_blank(const char *s)
{
    if (!s) return 1;
    while (*s) {
        if (!isspace((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

char *lstrip(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

char *trim(char *s)
{
    s = lstrip(s);
    rstrip(s);
    return s;
}

const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* --------------------------------------------------------------- sbuf */
void sb_init(struct sbuf *b)
{
    b->data = xmalloc(16);
    b->data[0] = '\0';
    b->len = 0;
    b->cap = 16;
}

void sb_free(struct sbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void sb_grow(struct sbuf *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return;
    while (b->len + need + 1 > b->cap) b->cap *= 2;
    b->data = xrealloc(b->data, b->cap);
}

void sb_addch(struct sbuf *b, char c)
{
    sb_grow(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void sb_addn(struct sbuf *b, const char *s, size_t n)
{
    sb_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_add(struct sbuf *b, const char *s)
{
    sb_addn(b, s, strlen(s));
}

char *sb_detach(struct sbuf *b)
{
    char *r = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return r;
}

/* --------------------------------------------------------------- strvec */
void sv_init(struct strvec *v)
{
    v->items = NULL;
    v->len = v->cap = 0;
}

void sv_free(struct strvec *v)
{
    for (size_t i = 0; i < v->len; i++) free(v->items[i]);
    free(v->items);
    v->items = NULL;
    v->len = v->cap = 0;
}

void sv_push(struct strvec *v, char *s)
{
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, v->cap * sizeof *v->items);
    }
    v->items[v->len++] = s;
}

void sv_push_dup(struct strvec *v, const char *s)
{
    sv_push(v, xstrdup(s));
}

void sv_push_words(struct strvec *v, const char *s)
{
    if (!s) return;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        const char *start = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        sv_push(v, xstrndup(start, (size_t)(s - start)));
    }
}

void sv_push_words_uniq(struct strvec *v, const char *s)
{
    struct strvec tmp;
    sv_init(&tmp);
    sv_push_words(&tmp, s);
    for (size_t i = 0; i < tmp.len; i++) {
        if (!sv_contains(v, tmp.items[i]))
            sv_push_dup(v, tmp.items[i]);
    }
    sv_free(&tmp);
}

int sv_contains(const struct strvec *v, const char *s)
{
    for (size_t i = 0; i < v->len; i++)
        if (strcmp(v->items[i], s) == 0) return 1;
    return 0;
}

char *sv_join(const struct strvec *v, const char *sep)
{
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v->len; i++) {
        if (i) sb_add(&b, sep);
        sb_add(&b, v->items[i]);
    }
    return sb_detach(&b);
}

/* --------------------------------------------------------------- ivec */
void iv_push(struct ivec *v, int n)
{
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, v->cap * sizeof *v->items);
    }
    v->items[v->len++] = n;
}

/* --------------------------------------------------------------- hashmap */
void hm_init(struct hmap *m)
{
    m->nbuckets = 64;
    m->count = 0;
    m->buckets = xmalloc(m->nbuckets * sizeof *m->buckets);
    for (size_t i = 0; i < m->nbuckets; i++) m->buckets[i] = NULL;
}

static size_t hm_hash(const char *s)
{
    size_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static void hm_maybe_grow(struct hmap *m)
{
    if (m->count < m->nbuckets * 3 / 4) return;
    size_t nb = m->nbuckets * 2;
    struct hentry **b = xmalloc(nb * sizeof *b);
    for (size_t i = 0; i < nb; i++) b[i] = NULL;
    for (size_t i = 0; i < m->nbuckets; i++) {
        struct hentry *e = m->buckets[i];
        while (e) {
            struct hentry *nx = e->next;
            size_t j = hm_hash(e->key) & (nb - 1);
            e->next = b[j];
            b[j] = e;
            e = nx;
        }
    }
    free(m->buckets);
    m->buckets = b;
    m->nbuckets = nb;
}

void *hm_get(const struct hmap *m, const char *key)
{
    if (!m->buckets) return NULL;
    size_t i = hm_hash(key) & (m->nbuckets - 1);
    for (struct hentry *e = m->buckets[i]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e->val;
    return NULL;
}

void hm_put(struct hmap *m, const char *key, void *val)
{
    if (!m->buckets) hm_init(m);
    size_t i = hm_hash(key) & (m->nbuckets - 1);
    for (struct hentry *e = m->buckets[i]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) { e->val = val; return; }
    }
    struct hentry *e = xmalloc(sizeof *e);
    e->key = xstrdup(key);
    e->val = val;
    e->next = m->buckets[i];
    m->buckets[i] = e;
    m->count++;
    hm_maybe_grow(m);
}
