/* craft - variable and function expansion ($(...), ${...}, $x) */
#include "craft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int g_depth = 0;

/* ------------------------------------------------------------ pattern utils */

/* Match STR against PAT which contains at most one '%'.  On success returns 1
   and, when STEM is non-NULL, stores the substring that '%' captured. */
int pattern_match(const char *pat, const char *str, char **stem)
{
    const char *pct = strchr(pat, '%');
    if (!pct) {
        if (strcmp(pat, str) == 0) {
            if (stem) *stem = xstrdup("");
            return 1;
        }
        return 0;
    }
    size_t plen = strlen(pat), slen = strlen(str);
    size_t pre = (size_t)(pct - pat);
    size_t suf = plen - pre - 1;
    if (slen < pre + suf) return 0;
    if (strncmp(str, pat, pre) != 0) return 0;
    if (strncmp(str + slen - suf, pct + 1, suf) != 0) return 0;
    if (stem) *stem = xstrndup(str + pre, slen - pre - suf);
    return 1;
}

/* Apply a patsubst-style replacement of PAT->REPL to STR (single word). */
char *pattern_subst_word(const char *pat, const char *repl, const char *str)
{
    char *stem = NULL;
    if (!pattern_match(pat, str, &stem))
        return xstrdup(str);

    const char *rpct = strchr(repl, '%');
    if (!rpct) {
        free(stem);
        return xstrdup(repl);
    }
    struct sbuf b;
    sb_init(&b);
    sb_addn(&b, repl, (size_t)(rpct - repl));
    sb_add(&b, stem ? stem : "");
    sb_add(&b, rpct + 1);
    free(stem);
    return sb_detach(&b);
}

/* ------------------------------------------------------------ automatic vars */

static char *dir_part(const char *w)
{
    const char *b = base_name(w);
    if (b == w) return xstrdup(".");
    size_t n = (size_t)(b - w);
    while (n > 1 && (w[n - 1] == '/' || w[n - 1] == '\\')) n--;
    return xstrndup(w, n);
}

static char *apply_per_word(const char *list, char *(*f)(const char *))
{
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, list);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        if (i) sb_addch(&b, ' ');
        char *r = f(v.items[i]);
        sb_add(&b, r);
        free(r);
    }
    sv_free(&v);
    return sb_detach(&b);
}

static char *notdir_word(const char *w) { return xstrdup(base_name(w)); }

static char *compute_auto(const char *name)
{
    if (!g_auto) return NULL;
    size_t n = strlen(name);
    if (n != 1 && n != 2) return NULL;

    char base = name[0];
    char mod = (n == 2) ? name[1] : 0;
    if (mod && mod != 'D' && mod != 'F') return NULL;
    if (!strchr("@<^?*+", base)) return NULL;

    char *val = NULL;
    switch (base) {
    case '@': val = xstrdup(g_auto->target ? g_auto->target : ""); break;
    case '*': val = xstrdup(g_auto->stem ? g_auto->stem : ""); break;
    case '<':
        val = xstrdup((g_auto->prereqs_all && g_auto->prereqs_all->len)
                      ? g_auto->prereqs_all->items[0] : "");
        break;
    case '^':
        val = g_auto->prereqs ? sv_join(g_auto->prereqs, " ") : xstrdup("");
        break;
    case '+':
        val = g_auto->prereqs_all ? sv_join(g_auto->prereqs_all, " ") : xstrdup("");
        break;
    case '?':
        val = g_auto->prereqs_newer ? sv_join(g_auto->prereqs_newer, " ")
                                    : xstrdup("");
        break;
    }
    if (!val) return NULL;
    if (!mod) return val;

    char *r = (mod == 'D') ? apply_per_word(val, dir_part)
                           : apply_per_word(val, notdir_word);
    free(val);
    return r;
}

/* ------------------------------------------------------------ arg splitting */

/* Split RAW on top-level commas.  If LIMIT > 0, stop after LIMIT-1 splits so
   the remainder becomes the final argument. */
static void split_args(const char *raw, int limit, struct strvec *out)
{
    int depth = 0;
    const char *start = raw;
    for (const char *p = raw; ; p++) {
        char c = *p;
        if (c == '\0') {
            sv_push(out, xstrndup(start, (size_t)(p - start)));
            break;
        }
        if (c == '(' || c == '{') depth++;
        else if (c == ')' || c == '}') depth--;
        else if (c == ',' && depth == 0 &&
                 (limit <= 0 || (int)out->len < limit - 1)) {
            sv_push(out, xstrndup(start, (size_t)(p - start)));
            start = p + 1;
        }
    }
}

/* ------------------------------------------------------------ function table */

typedef char *(*fnimpl)(struct strvec *raw);

struct fnentry { const char *name; fnimpl fn; int split; int minargs; };

static char *expand_arg(struct strvec *raw, size_t i)
{
    if (i >= raw->len) return xstrdup("");
    return expand(raw->items[i]);
}

/* strip surrounding whitespace and expand */
static char *expand_arg_trim(struct strvec *raw, size_t i)
{
    if (i >= raw->len) return xstrdup("");
    char *dup = xstrdup(raw->items[i]);
    char *t = trim(dup);
    char *r = expand(t);
    free(dup);
    return r;
}

static int wordcmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static char *join_words_uniq_sorted(char *text)
{
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    if (v.len > 1)
        qsort(v.items, v.len, sizeof v.items[0], wordcmp);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        if (i && strcmp(v.items[i], v.items[i - 1]) == 0) continue;
        if (b.len) sb_addch(&b, ' ');
        sb_add(&b, v.items[i]);
    }
    sv_free(&v);
    return sb_detach(&b);
}

static char *fn_subst(struct strvec *raw)
{
    char *from = expand_arg(raw, 0);
    char *to   = expand_arg(raw, 1);
    char *text = expand_arg(raw, 2);
    struct sbuf b;
    sb_init(&b);
    if (*from) {
        size_t fl = strlen(from);
        for (const char *p = text; *p; ) {
            if (strncmp(p, from, fl) == 0) { sb_add(&b, to); p += fl; }
            else sb_addch(&b, *p++);
        }
    } else {
        sb_add(&b, text);
    }
    free(from); free(to); free(text);
    return sb_detach(&b);
}

static char *fn_patsubst(struct strvec *raw)
{
    char *pat  = expand_arg(raw, 0);
    char *repl = expand_arg(raw, 1);
    char *text = expand_arg(raw, 2);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        if (i) sb_addch(&b, ' ');
        char *r = pattern_subst_word(pat, repl, v.items[i]);
        sb_add(&b, r);
        free(r);
    }
    sv_free(&v);
    free(pat); free(repl); free(text);
    return sb_detach(&b);
}

static char *fn_strip(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    char *r = sv_join(&v, " ");
    sv_free(&v);
    free(text);
    return r;
}

static char *fn_findstring(struct strvec *raw)
{
    char *a = expand_arg(raw, 0);
    char *b = expand_arg(raw, 1);
    char *r = xstrdup(strstr(b, a) ? a : "");
    free(a); free(b);
    return r;
}

static char *filter_common(struct strvec *raw, int keep)
{
    char *pats = expand_arg(raw, 0);
    char *text = expand_arg(raw, 1);
    struct strvec pv, tv;
    sv_init(&pv);
    sv_init(&tv);
    sv_push_words(&pv, pats);
    sv_push_words(&tv, text);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < tv.len; i++) {
        int m = 0;
        for (size_t j = 0; j < pv.len && !m; j++)
            m = pattern_match(pv.items[j], tv.items[i], NULL);
        if (m == keep) {
            if (b.len) sb_addch(&b, ' ');
            sb_add(&b, tv.items[i]);
        }
    }
    sv_free(&pv); sv_free(&tv);
    free(pats); free(text);
    return sb_detach(&b);
}
static char *fn_filter(struct strvec *raw)      { return filter_common(raw, 1); }
static char *fn_filter_out(struct strvec *raw)  { return filter_common(raw, 0); }

static char *fn_sort(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    char *r = join_words_uniq_sorted(text);
    free(text);
    return r;
}

static char *fn_word(struct strvec *raw)
{
    char *ns = expand_arg(raw, 0);
    char *text = expand_arg(raw, 1);
    long n = strtol(trim(ns), NULL, 10);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    char *r = xstrdup((n >= 1 && (size_t)n <= v.len) ? v.items[n - 1] : "");
    sv_free(&v);
    free(ns); free(text);
    return r;
}

static char *fn_wordlist(struct strvec *raw)
{
    char *ss = expand_arg(raw, 0);
    char *es = expand_arg(raw, 1);
    char *text = expand_arg(raw, 2);
    long s = strtol(trim(ss), NULL, 10);
    long e = strtol(trim(es), NULL, 10);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    struct sbuf b;
    sb_init(&b);
    if (s < 1) s = 1;
    for (long i = s; i <= e && (size_t)i <= v.len; i++) {
        if (b.len) sb_addch(&b, ' ');
        sb_add(&b, v.items[i - 1]);
    }
    sv_free(&v);
    free(ss); free(es); free(text);
    return sb_detach(&b);
}

static char *fn_words(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    char buf[24];
    snprintf(buf, sizeof buf, "%zu", v.len);
    sv_free(&v);
    free(text);
    return xstrdup(buf);
}

static char *fn_firstword(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    char *r = xstrdup(v.len ? v.items[0] : "");
    sv_free(&v);
    free(text);
    return r;
}

static char *fn_lastword(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    char *r = xstrdup(v.len ? v.items[v.len - 1] : "");
    sv_free(&v);
    free(text);
    return r;
}

static char *fn_dir(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        if (i) sb_addch(&b, ' ');
        const char *w = v.items[i];
        const char *bn = base_name(w);
        if (bn == w) sb_add(&b, "./");
        else sb_addn(&b, w, (size_t)(bn - w));
    }
    sv_free(&v);
    free(text);
    return sb_detach(&b);
}

static char *fn_notdir(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    char *r = apply_per_word(text, notdir_word);
    free(text);
    return r;
}

static char *suffix_word(const char *w)
{
    const char *bn = base_name(w);
    const char *dot = strrchr(bn, '.');
    return xstrdup(dot ? dot : "");
}
static char *fn_suffix(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        char *s = suffix_word(v.items[i]);
        if (*s) { if (b.len) sb_addch(&b, ' '); sb_add(&b, s); }
        free(s);
    }
    sv_free(&v);
    free(text);
    return sb_detach(&b);
}

static char *basename_word(const char *w)
{
    const char *bn = base_name(w);
    const char *dot = strrchr(bn, '.');
    if (!dot) return xstrdup(w);
    return xstrndup(w, (size_t)(dot - w));
}
static char *fn_basename(struct strvec *raw)
{
    char *text = expand_arg(raw, 0);
    char *r = apply_per_word(text, basename_word);
    free(text);
    return r;
}

static char *fn_addsuffix(struct strvec *raw)
{
    char *sfx = expand_arg(raw, 0);
    char *text = expand_arg(raw, 1);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        if (i) sb_addch(&b, ' ');
        sb_add(&b, v.items[i]);
        sb_add(&b, sfx);
    }
    sv_free(&v);
    free(sfx); free(text);
    return sb_detach(&b);
}

static char *fn_addprefix(struct strvec *raw)
{
    char *pfx = expand_arg(raw, 0);
    char *text = expand_arg(raw, 1);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        if (i) sb_addch(&b, ' ');
        sb_add(&b, pfx);
        sb_add(&b, v.items[i]);
    }
    sv_free(&v);
    free(pfx); free(text);
    return sb_detach(&b);
}

static char *fn_join(struct strvec *raw)
{
    char *a = expand_arg(raw, 0);
    char *c = expand_arg(raw, 1);
    struct strvec av, cv;
    sv_init(&av);
    sv_init(&cv);
    sv_push_words(&av, a);
    sv_push_words(&cv, c);
    size_t n = av.len > cv.len ? av.len : cv.len;
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < n; i++) {
        if (i) sb_addch(&b, ' ');
        if (i < av.len) sb_add(&b, av.items[i]);
        if (i < cv.len) sb_add(&b, cv.items[i]);
    }
    sv_free(&av); sv_free(&cv);
    free(a); free(c);
    return sb_detach(&b);
}

char *glob_expand(const char *pattern);  /* shell.c */

static char *fn_wildcard(struct strvec *raw)
{
    char *pat = expand_arg(raw, 0);
    struct strvec pv;
    sv_init(&pv);
    sv_push_words(&pv, pat);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < pv.len; i++) {
        char *g = glob_expand(pv.items[i]);
        if (*g) { if (b.len) sb_addch(&b, ' '); sb_add(&b, g); }
        free(g);
    }
    sv_free(&pv);
    free(pat);
    return sb_detach(&b);
}

char *os_abspath(const char *p, int must_exist);  /* shell.c */

static char *abspath_common(struct strvec *raw, int must_exist)
{
    char *text = expand_arg(raw, 0);
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, text);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        char *a = os_abspath(v.items[i], must_exist);
        if (*a) { if (b.len) sb_addch(&b, ' '); sb_add(&b, a); }
        free(a);
    }
    sv_free(&v);
    free(text);
    return sb_detach(&b);
}
static char *fn_abspath(struct strvec *raw)  { return abspath_common(raw, 0); }
static char *fn_realpath(struct strvec *raw) { return abspath_common(raw, 1); }

/* ---- control functions ---- */

struct var *push_temp_var(const char *name, const char *value);
void        pop_temp_var(const char *name, struct var *old);

static char *fn_foreach(struct strvec *raw)
{
    char *var  = expand_arg_trim(raw, 0);
    char *list = expand_arg(raw, 1);
    const char *body = raw->len > 2 ? raw->items[2] : "";
    struct strvec v;
    sv_init(&v);
    sv_push_words(&v, list);
    struct sbuf b;
    sb_init(&b);
    for (size_t i = 0; i < v.len; i++) {
        struct var *old = push_temp_var(var, v.items[i]);
        char *piece = expand(body);
        pop_temp_var(var, old);
        if (i) sb_addch(&b, ' ');
        sb_add(&b, piece);
        free(piece);
    }
    sv_free(&v);
    free(var); free(list);
    return sb_detach(&b);
}

static char *fn_if(struct strvec *raw)
{
    char *cond = expand_arg_trim(raw, 0);
    int t = *cond != '\0';
    free(cond);
    if (t) return expand_arg(raw, 1);
    return raw->len > 2 ? expand_arg(raw, 2) : xstrdup("");
}

static char *fn_or(struct strvec *raw)
{
    for (size_t i = 0; i < raw->len; i++) {
        char *v = expand_arg_trim(raw, i);
        if (*v) return v;
        free(v);
    }
    return xstrdup("");
}

static char *fn_and(struct strvec *raw)
{
    char *last = xstrdup("");
    for (size_t i = 0; i < raw->len; i++) {
        free(last);
        last = expand_arg_trim(raw, i);
        if (!*last) { free(last); return xstrdup(""); }
    }
    return last;
}

static char *fn_call(struct strvec *raw)
{
    if (raw->len == 0) return xstrdup("");
    char *name = expand_arg_trim(raw, 0);
    struct var *v = var_lookup(name);

    struct var *saved[16];
    char pname[4];
    int nparams = (int)raw->len - 1;
    if (nparams > 15) nparams = 15;
    for (int i = 0; i <= nparams; i++) {
        snprintf(pname, sizeof pname, "%d", i);
        char *pv = (i == 0) ? xstrdup(name) : expand_arg(raw, (size_t)i);
        saved[i] = push_temp_var(pname, pv);
        free(pv);
    }
    char *result = v ? (v->flavor == F_RECUR ? expand(v->value)
                                             : xstrdup(v->value))
                     : xstrdup("");
    for (int i = 0; i <= nparams; i++) {
        snprintf(pname, sizeof pname, "%d", i);
        pop_temp_var(pname, saved[i]);
    }
    free(name);
    return result;
}

static char *fn_value(struct strvec *raw)
{
    char *name = expand_arg_trim(raw, 0);
    struct var *v = var_lookup(name);
    char *r = xstrdup(v ? v->value : "");
    free(name);
    return r;
}

static char *fn_origin(struct strvec *raw)
{
    char *name = expand_arg_trim(raw, 0);
    struct var *v = var_lookup(name);
    char *r = xstrdup(v ? var_origin_name(v->origin) : "undefined");
    free(name);
    return r;
}

static char *fn_flavor(struct strvec *raw)
{
    char *name = expand_arg_trim(raw, 0);
    struct var *v = var_lookup(name);
    const char *f = !v ? "undefined"
                       : (v->flavor == F_RECUR ? "recursive" : "simple");
    free(name);
    return xstrdup(f);
}

extern const char *g_cur_file;
extern int g_cur_line;

static char *fn_error(struct strvec *raw)
{
    char *m = expand_arg(raw, 0);
    if (g_cur_file) fprintf(stderr, "%s:%d: *** %s.  Stop.\n", g_cur_file, g_cur_line, m);
    else            fprintf(stderr, "%s: *** %s.  Stop.\n", program_name, m);
    exit(2);
    return NULL;
}

static char *fn_warning(struct strvec *raw)
{
    char *m = expand_arg(raw, 0);
    if (g_cur_file) fprintf(stderr, "%s:%d: %s\n", g_cur_file, g_cur_line, m);
    else            fprintf(stderr, "%s: %s\n", program_name, m);
    free(m);
    return xstrdup("");
}

static char *fn_info(struct strvec *raw)
{
    char *m = expand_arg(raw, 0);
    printf("%s\n", m);
    free(m);
    return xstrdup("");
}

static char *fn_shell(struct strvec *raw)
{
    char *cmd = expand_arg(raw, 0);
    char *out = run_shell_capture(cmd);
    free(cmd);
    return out;
}

static const struct fnentry FUNCS[] = {
    { "subst",      fn_subst,      3, 3 },
    { "patsubst",   fn_patsubst,   3, 3 },
    { "strip",      fn_strip,      1, 1 },
    { "findstring", fn_findstring, 2, 2 },
    { "filter",     fn_filter,     2, 2 },
    { "filter-out", fn_filter_out, 2, 2 },
    { "sort",       fn_sort,       1, 1 },
    { "word",       fn_word,       2, 2 },
    { "wordlist",   fn_wordlist,   3, 3 },
    { "words",      fn_words,      1, 1 },
    { "firstword",  fn_firstword,  1, 1 },
    { "lastword",   fn_lastword,   1, 1 },
    { "dir",        fn_dir,        1, 1 },
    { "notdir",     fn_notdir,     1, 1 },
    { "suffix",     fn_suffix,     1, 1 },
    { "basename",   fn_basename,   1, 1 },
    { "addsuffix",  fn_addsuffix,  2, 2 },
    { "addprefix",  fn_addprefix,  2, 2 },
    { "join",       fn_join,       2, 2 },
    { "wildcard",   fn_wildcard,   1, 1 },
    { "realpath",   fn_realpath,   1, 1 },
    { "abspath",    fn_abspath,    1, 1 },
    { "foreach",    fn_foreach,    3, 3 },
    { "if",         fn_if,         3, 2 },
    { "or",         fn_or,         0, 1 },
    { "and",        fn_and,        0, 1 },
    { "call",       fn_call,       0, 1 },
    { "value",      fn_value,      1, 1 },
    { "origin",     fn_origin,     1, 1 },
    { "flavor",     fn_flavor,     1, 1 },
    { "error",      fn_error,      1, 1 },
    { "warning",    fn_warning,    1, 1 },
    { "info",       fn_info,       1, 1 },
    { "shell",      fn_shell,      1, 1 },
    { NULL, NULL, 0, 0 }
};

static const struct fnentry *find_func(const char *name)
{
    for (const struct fnentry *f = FUNCS; f->name; f++)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

int is_function_name(const char *name)
{
    return find_func(name) != NULL;
}

/* ------------------------------------------------------------ core expander */

static void emit_var_value(struct sbuf *out, const char *varname)
{
    char *am = compute_auto(varname);
    if (am) { sb_add(out, am); free(am); return; }

    struct var *v = var_lookup(varname);
    if (!v) return;
    if (v->flavor == F_SIMPLE) {
        sb_add(out, v->value);
    } else {
        expand_into(out, v->value);
    }
}

/* handle the text between $( and ) */
static void handle_ref(struct sbuf *out, const char *ref)
{
    /* function?  first token followed by whitespace and known name */
    const char *p = ref;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p && isspace((unsigned char)*p)) {
        char *tok = xstrndup(ref, (size_t)(p - ref));
        const struct fnentry *fe = find_func(tok);
        if (fe) {
            while (*p && isspace((unsigned char)*p)) p++;
            struct strvec args;
            sv_init(&args);
            split_args(p, fe->split, &args);
            char *r = fe->fn(&args);
            sb_add(out, r);
            free(r);
            sv_free(&args);
            free(tok);
            return;
        }
        free(tok);
    }

    /* variable reference, possibly with a substitution suffix */
    char *name = expand(ref);
    char *colon = NULL;
    for (char *q = name; *q; q++) {
        if (*q == ':') { colon = q; break; }
    }
    if (colon) {
        *colon = '\0';
        char *spec = colon + 1;
        char *eq = strchr(spec, '=');
        struct sbuf tmp;
        sb_init(&tmp);
        emit_var_value(&tmp, name);
        struct strvec v;
        sv_init(&v);
        sv_push_words(&v, tmp.data);
        sb_free(&tmp);

        char *from = eq ? xstrndup(spec, (size_t)(eq - spec)) : xstrdup(spec);
        char *to   = eq ? xstrdup(eq + 1) : xstrdup("");

        int has_pct = strchr(from, '%') != NULL;
        struct sbuf b;
        sb_init(&b);
        for (size_t i = 0; i < v.len; i++) {
            if (i) sb_addch(&b, ' ');
            char *r;
            if (has_pct) {
                r = pattern_subst_word(from, to, v.items[i]);
            } else {
                size_t wl = strlen(v.items[i]), fl = strlen(from);
                if (fl && wl >= fl && strcmp(v.items[i] + wl - fl, from) == 0) {
                    r = xmalloc(wl - fl + strlen(to) + 1);
                    memcpy(r, v.items[i], wl - fl);
                    strcpy(r + wl - fl, to);
                } else {
                    r = xstrdup(v.items[i]);
                }
            }
            sb_add(&b, r);
            free(r);
        }
        sv_free(&v);
        free(from); free(to);
        char *res = sb_detach(&b);
        sb_add(out, res);
        free(res);
    } else {
        emit_var_value(out, name);
    }
    free(name);
}

void expand_into(struct sbuf *out, const char *s)
{
    if (!s) return;
    if (++g_depth > 1000)
        die("variable expansion nested too deeply (recursive reference?)");

    for (const char *p = s; *p; ) {
        if (*p != '$') { sb_addch(out, *p++); continue; }
        p++;
        char c = *p;
        if (c == '\0') { sb_addch(out, '$'); break; }
        if (c == '$')  { sb_addch(out, '$'); p++; continue; }
        if (c == '(' || c == '{') {
            char open = c, close = (c == '(') ? ')' : '}';
            (void)close;
            int depth = 1;
            const char *start = ++p;
            while (*p && depth) {
                if (*p == '(' || *p == '{') depth++;
                else if (*p == ')' || *p == '}') depth--;
                if (depth == 0) break;
                p++;
            }
            char *ref = xstrndup(start, (size_t)(p - start));
            if (*p) p++;            /* skip closing */
            (void)open;
            handle_ref(out, ref);
            free(ref);
        } else {
            char nm[2] = { c, 0 };
            emit_var_value(out, nm);
            p++;
        }
    }
    g_depth--;
}

char *expand(const char *s)
{
    struct sbuf b;
    sb_init(&b);
    expand_into(&b, s);
    return sb_detach(&b);
}

/* temp-variable helpers shared with control functions */
struct var *push_temp_var(const char *name, const char *value)
{
    struct var *old = hm_get(&g_vars, name);
    struct var *nv = xmalloc(sizeof *nv);
    nv->name = xstrdup(name);
    nv->value = xstrdup(value);
    nv->flavor = F_SIMPLE;
    nv->origin = OR_AUTO;
    nv->exported = 0;
    hm_put(&g_vars, name, nv);
    return old;
}

void pop_temp_var(const char *name, struct var *old)
{
    struct var *cur = hm_get(&g_vars, name);
    if (cur && cur != old) {
        free(cur->name);
        free(cur->value);
        free(cur);
    }
    hm_put(&g_vars, name, old);
}
