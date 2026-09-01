/* craft - makefile lexer: physical lines -> logical lines
 *
 * A logical line is one of:
 *   - a recipe line  (physical line began with a TAB); the leading TAB is
 *     removed, backslash-newline continuations are kept verbatim, and '#'
 *     is NOT treated as a comment.
 *   - a regular line (assignment / rule / directive); comments are stripped
 *     per physical line, then backslash-newline continuations are folded to
 *     a single space.
 */
#include "craft.h"
#include "lex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int lex_open(struct lexer *lx, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    lx->buf = xmalloc((size_t)sz + 2);
    size_t got = fread(lx->buf, 1, (size_t)sz, f);
    fclose(f);
    lx->buf[got] = '\0';
    lx->len = got;
    lx->pos = 0;
    lx->fname = xstrdup(path);
    lx->lineno = 1;
    return 1;
}

void lex_close(struct lexer *lx)
{
    free(lx->buf);
    free(lx->fname);
    lx->buf = lx->fname = NULL;
}

/* read one physical line (without the trailing newline); returns 0 at EOF.
   Sets *out to a malloc'd copy, *had_nl to whether a newline terminated it. */
static int phys_line(struct lexer *lx, char **out)
{
    if (lx->pos >= lx->len) return 0;
    size_t start = lx->pos;
    while (lx->pos < lx->len && lx->buf[lx->pos] != '\n') lx->pos++;
    size_t end = lx->pos;
    if (end > start && lx->buf[end - 1] == '\r') end--;
    *out = xstrndup(lx->buf + start, end - start);
    if (lx->pos < lx->len) lx->pos++;   /* skip '\n' */
    lx->lineno++;
    return 1;
}

/* number of trailing backslashes */
static int trailing_backslashes(const char *s)
{
    size_t n = strlen(s);
    int c = 0;
    while (n && s[n - 1] == '\\') { c++; n--; }
    return c;
}

/* strip an unescaped '#' comment in place; convert "\#" -> "#" */
static void strip_comment(char *s)
{
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1] == '#') { *w++ = '#'; p++; continue; }
        if (*p == '#') { break; }
        *w++ = *p;
    }
    *w = '\0';
}

char *lex_next(struct lexer *lx, int *is_recipe, int *lineno_out)
{
again:
    if (lx->pos >= lx->len) return NULL;

    int this_line = lx->lineno;
    int recipe = (lx->buf[lx->pos] == '\t');

    char *phys = NULL;
    if (!phys_line(lx, &phys)) return NULL;

    if (recipe) {
        struct sbuf b;
        sb_init(&b);
        sb_add(&b, phys + 1);          /* drop leading TAB */
        free(phys);
        /* keep continuations verbatim (backslash + newline preserved) */
        while (trailing_backslashes(b.data) % 2 == 1) {
            char *cont = NULL;
            if (!phys_line(lx, &cont)) break;
            sb_addch(&b, '\n');
            const char *c = cont;
            if (*c == '\t') c++;
            sb_add(&b, c);
            free(cont);
        }
        if (is_blank(b.data)) { sb_free(&b); goto again; }
        *is_recipe = 1;
        if (lineno_out) *lineno_out = this_line;
        return sb_detach(&b);
    }

    /* regular line: comment-strip this physical line, then fold continuations */
    strip_comment(phys);
    struct sbuf b;
    sb_init(&b);
    sb_add(&b, phys);
    free(phys);

    while (trailing_backslashes(b.data) % 2 == 1) {
        b.data[--b.len] = '\0';                 /* drop the backslash */
        rstrip(b.data); b.len = strlen(b.data);
        char *cont = NULL;
        if (!phys_line(lx, &cont)) break;
        strip_comment(cont);
        char *t = lstrip(cont);
        if (b.len) sb_addch(&b, ' ');
        sb_add(&b, t);
        free(cont);
    }

    if (is_blank(b.data)) { sb_free(&b); goto again; }
    *is_recipe = 0;
    if (lineno_out) *lineno_out = this_line;
    return sb_detach(&b);
}
