/* craft - makefile parser: assignments, rules, conditionals, include */
#include "craft.h"
#include "lex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct strvec g_include_dirs;
const char *g_cur_file = NULL;
int         g_cur_line = 0;

/* ------------------------------------------------------------ conditionals */
struct cond_frame {
    int parent_active;
    int any_taken;
    int active;
    int seen_else;
};
static struct cond_frame condstack[64];
static int condsp = 0;

static int cond_active(void)
{
    return condsp == 0 ? 1 : condstack[condsp - 1].active;
}

/* ------------------------------------------------------------ helpers */

/* find leftmost top-level occurrence of any assignment operator; returns
   pointer to it and sets *oplen, or NULL.  '=' inside $() is ignored. */
static char *find_assign_op(char *s, int *oplen)
{
    int depth = 0;
    for (char *p = s; *p; p++) {
        if (*p == '(' || *p == '{') depth++;
        else if (*p == ')' || *p == '}') { if (depth) depth--; }
        else if (depth == 0) {
            if (p[0] == ':' && p[1] == ':' && p[2] == '=') { *oplen = 3; return p; }
            if (p[0] == ':' && p[1] == '=') { *oplen = 2; return p; }
            if (p[0] == '+' && p[1] == '=') { *oplen = 2; return p; }
            if (p[0] == '?' && p[1] == '=') { *oplen = 2; return p; }
            if (p[0] == '!' && p[1] == '=') { *oplen = 2; return p; }
            if (p[0] == '=')               { *oplen = 1; return p; }
            if (p[0] == ':') return NULL;  /* a rule, not an assignment */
        }
    }
    return NULL;
}

/* find the rule separator ':' (single or '::'), skipping $() and a leading
   drive letter like C:\ ; returns pointer or NULL, sets *dcolon */
static char *find_rule_colon(char *s, int *dcolon)
{
    int depth = 0;
    for (char *p = s; *p; p++) {
        if (*p == '(' || *p == '{') depth++;
        else if (*p == ')' || *p == '}') { if (depth) depth--; }
        else if (*p == ':' && depth == 0) {
            if (p[1] == '=') return NULL;
            /* drive letter: <alpha>:[/\] at start of a word */
            if (p == s + 1 && isalpha((unsigned char)s[0]) &&
                (p[1] == '/' || p[1] == '\\'))
                continue;
            *dcolon = (p[1] == ':');
            return p;
        }
    }
    return NULL;
}

static int starts_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    return strncmp(s, w, n) == 0 && (s[n] == '\0' || isspace((unsigned char)s[n]));
}

/* ------------------------------------------------------------ special targets */

static void note_special(const char *name, struct strvec *prereqs)
{
    if (strcmp(name, ".PHONY") == 0) {
        for (size_t i = 0; i < prereqs->len; i++)
            hm_put(&g_phony, prereqs->items[i], (void *)1);
    } else if (strcmp(name, ".PRECIOUS") == 0) {
        for (size_t i = 0; i < prereqs->len; i++)
            hm_put(&g_precious, prereqs->items[i], (void *)1);
    } else if (strcmp(name, ".SECONDARY") == 0) {
        for (size_t i = 0; i < prereqs->len; i++)
            hm_put(&g_secondary, prereqs->items[i], (void *)1);
    } else if (strcmp(name, ".DELETE_ON_ERROR") == 0) {
        g_delete_on_error = 1;
    } else if (strcmp(name, ".ONESHELL") == 0) {
        g_oneshell = 1;
    } else if (strcmp(name, ".SILENT") == 0) {
        if (prereqs->len == 0) opt_silent = 1;
    } else if (strcmp(name, ".IGNORE") == 0) {
        if (prereqs->len == 0) opt_ignore_errors = 1;
    } else if (strcmp(name, ".EXPORT_ALL_VARIABLES") == 0) {
        g_export_all = 1;
    } else if (strcmp(name, ".SUFFIXES") == 0) {
        /* suffix rules are not implemented; accept and ignore */
    }
}

static int is_special_target(const char *n)
{
    return n[0] == '.' && strchr(n, '/') == NULL && isupper((unsigned char)n[1]);
}

/* ------------------------------------------------------------ assignments */

static void do_assign(char *line, int is_override, int is_export)
{
    int oplen = 0;
    char *op = find_assign_op(line, &oplen);
    if (!op) return;

    char opbuf[4] = {0};
    memcpy(opbuf, op, (size_t)oplen);
    *op = '\0';
    char *rawname = trim(line);
    char *value = trim(op + oplen);

    char *name = expand(rawname);
    enum var_origin origin = is_override ? OR_OVERRIDE : OR_FILE;

    if (strcmp(opbuf, "=") == 0) {
        var_set(name, value, F_RECUR, origin);
    } else if (strcmp(opbuf, ":=") == 0 || strcmp(opbuf, "::=") == 0) {
        char *e = expand(value);
        var_set(name, e, F_SIMPLE, origin);
        free(e);
    } else if (strcmp(opbuf, "?=") == 0) {
        if (!var_lookup(name))
            var_set(name, value, F_RECUR, origin);
    } else if (strcmp(opbuf, "+=") == 0) {
        var_append(name, value, origin);
    } else if (strcmp(opbuf, "!=") == 0) {
        char *e = expand(value);
        char *out = run_shell_capture(e);
        var_set(name, out, F_SIMPLE, origin);
        free(e); free(out);
    }

    if (is_export || g_export_all) {
        struct var *v = var_lookup(name);
        if (v) v->exported = 1;
    }
    free(name);
}

/* ------------------------------------------------------------ rules */

struct rule **g_pending_rules = NULL;   /* recipes append here */
size_t g_pending_count = 0;

static void pending_clear(void)
{
    free(g_pending_rules);
    g_pending_rules = NULL;
    g_pending_count = 0;
}

static void pending_add(struct rule *r)
{
    g_pending_rules = xrealloc(g_pending_rules,
                               (g_pending_count + 1) * sizeof *g_pending_rules);
    g_pending_rules[g_pending_count++] = r;
}

static void add_tsvar(const char *target, const char *nm,
                      const char *op, const char *val)
{
    struct tsvar *tv = xmalloc(sizeof *tv);
    tv->name = xstrdup(nm);
    tv->op = xstrdup(op);
    tv->value = xstrdup(val);
    tv->next = hm_get(&g_tsvars, target);
    hm_put(&g_tsvars, target, tv);
}

static void set_default_goal(const char *t)
{
    if (g_default_goal) return;
    if (t[0] == '.' && strchr(t, '/') == NULL) return;   /* .PHONY etc. */
    if (strchr(t, '%')) return;
    g_default_goal = xstrdup(t);
}

static void do_rule(char *line, int was_indented)
{
    int dcolon = 0;
    char *colon = find_rule_colon(line, &dcolon);
    if (!colon) {
        if (was_indented)
            craft_error(g_cur_file, g_cur_line,
                        "missing separator - this line is indented with spaces; "
                        "recipe lines must begin with a TAB");
        else
            craft_error(g_cur_file, g_cur_line, "missing separator");
        return;
    }

    *colon = '\0';
    char *left = colon + (dcolon ? 2 : 1);
    char *targets_raw = trim(line);

    /* inline recipe after ';' at top level */
    char *inline_recipe = NULL;
    {
        int depth = 0;
        for (char *p = left; *p; p++) {
            if (*p == '(' || *p == '{') depth++;
            else if (*p == ')' || *p == '}') { if (depth) depth--; }
            else if (*p == ';' && depth == 0) { *p = '\0'; inline_recipe = p + 1; break; }
        }
    }

    /* static pattern rule?  targets : tpat : prereqs */
    char *second = NULL;
    {
        int d2 = 0;
        second = NULL;
        char *tmp = left;
        second = find_rule_colon(tmp, &d2);
    }

    /* target-specific variable?  target: NAME [op] value  */
    {
        int oplen = 0;
        char *op = find_assign_op(left, &oplen);
        if (op && !second) {
            char opbuf[4] = {0};
            memcpy(opbuf, op, (size_t)oplen);
            *op = '\0';
            char *nm = trim(left);
            char *val = trim(op + oplen);
            struct strvec tv;
            sv_init(&tv);
            char *te = expand(targets_raw);
            sv_push_words(&tv, te);
            free(te);
            for (size_t i = 0; i < tv.len; i++)
                add_tsvar(tv.items[i], nm, opbuf, val);
            sv_free(&tv);
            pending_clear();
            return;
        }
    }

    char *tpat = NULL, *prereq_part = left;
    if (second) {
        *second = '\0';
        tpat = trim(left);
        prereq_part = second + 1;
    }

    /* order-only prerequisites after '|' */
    char *order_part = NULL;
    {
        int depth = 0;
        for (char *p = prereq_part; *p; p++) {
            if (*p == '(' || *p == '{') depth++;
            else if (*p == ')' || *p == '}') { if (depth) depth--; }
            else if (*p == '|' && depth == 0) { *p = '\0'; order_part = p + 1; break; }
        }
    }

    char *te = expand(targets_raw);
    char *pe = expand(prereq_part);
    char *oe = order_part ? expand(order_part) : xstrdup("");

    struct strvec targets, prereqs, order;
    sv_init(&targets); sv_init(&prereqs); sv_init(&order);
    sv_push_words(&targets, te);
    sv_push_words(&prereqs, pe);
    sv_push_words(&order, oe);
    free(te); free(pe); free(oe);

    pending_clear();

    if (targets.len == 0) { sv_free(&targets); sv_free(&prereqs); sv_free(&order); return; }

    /* special targets */
    for (size_t i = 0; i < targets.len; i++)
        if (is_special_target(targets.items[i]))
            note_special(targets.items[i], &prereqs);

    if (tpat) {
        /* synthesize one concrete rule per target */
        for (size_t i = 0; i < targets.len; i++) {
            char *stem = NULL;
            if (!pattern_match(tpat, targets.items[i], &stem)) {
                craft_error(g_cur_file, g_cur_line,
                            "target '%s' doesn't match pattern '%s'",
                            targets.items[i], tpat);
                continue;
            }
            struct rule *r = rule_new();
            sv_push_dup(&r->targets, targets.items[i]);
            for (size_t j = 0; j < prereqs.len; j++) {
                struct sbuf b; sb_init(&b);
                for (const char *q = prereqs.items[j]; *q; q++) {
                    if (*q == '%') sb_add(&b, stem); else sb_addch(&b, *q);
                }
                sv_push(&r->prereqs, sb_detach(&b));
            }
            for (size_t j = 0; j < order.len; j++)
                sv_push_dup(&r->order_prereqs, order.items[j]);
            r->stem = xstrdup(stem);
            r->deffile = g_cur_file; r->defline = g_cur_line;
            rule_register(r);
            pending_add(r);
            if (inline_recipe && !is_blank(inline_recipe)) {
                sv_push_dup(&r->recipe, trim(inline_recipe));
                iv_push(&r->recipe_lno, g_cur_line);
            }
            free(stem);
        }
        set_default_goal(targets.items[0]);
    } else {
        struct rule *r = rule_new();
        for (size_t i = 0; i < targets.len; i++)
            sv_push_dup(&r->targets, targets.items[i]);
        for (size_t i = 0; i < prereqs.len; i++)
            sv_push_dup(&r->prereqs, prereqs.items[i]);
        for (size_t i = 0; i < order.len; i++)
            sv_push_dup(&r->order_prereqs, order.items[i]);
        r->double_colon = dcolon;
        r->is_pattern = 0;
        for (size_t i = 0; i < targets.len; i++)
            if (strchr(targets.items[i], '%')) r->is_pattern = 1;
        r->deffile = g_cur_file; r->defline = g_cur_line;
        rule_register(r);
        pending_add(r);
        if (inline_recipe && !is_blank(inline_recipe)) {
            sv_push_dup(&r->recipe, trim(inline_recipe));
            iv_push(&r->recipe_lno, g_cur_line);
        }
        if (!r->is_pattern)
            set_default_goal(targets.items[0]);
    }

    sv_free(&targets); sv_free(&prereqs); sv_free(&order);
}

/* ------------------------------------------------------------ conditionals */

static char *unquote(char *s)
{
    s = trim(s);
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\''))) {
        s[n - 1] = '\0';
        s++;
    }
    return s;
}

static int eval_ifeq(const char *args, int negate)
{
    char *a = xstrdup(args);
    char *t = trim(a);
    char *s1, *s2;
    if (*t == '(') {
        t++;
        size_t n = strlen(t);
        while (n && t[n - 1] != ')') n--;
        if (n) t[n - 1] = '\0';
        int depth = 0;
        char *comma = NULL;
        for (char *p = t; *p; p++) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (*p == ',' && depth == 0) { comma = p; break; }
        }
        if (!comma) { free(a); return negate; }
        *comma = '\0';
        s1 = t;
        s2 = comma + 1;
    } else {
        /* "a" "b" form */
        s1 = t;
        char *sp = t;
        char q = *sp;
        sp++;
        while (*sp && *sp != q) sp++;
        if (*sp) sp++;
        *sp = '\0';
        s2 = sp + 1;
    }
    char *e1 = expand(unquote(s1));
    char *e2 = expand(unquote(s2));
    int eq = strcmp(e1, e2) == 0;
    free(e1); free(e2); free(a);
    return negate ? !eq : eq;
}

static int eval_ifdef(const char *args, int negate)
{
    char *n = expand(args);
    char *nm = trim(n);
    struct var *v = var_lookup(nm);
    int def = v && v->value[0] != '\0';
    free(n);
    return negate ? !def : def;
}

static void push_cond(int cond)
{
    int parent = cond_active();
    struct cond_frame *f = &condstack[condsp++];
    f->parent_active = parent;
    f->any_taken = cond && parent;
    f->active = cond && parent;
    f->seen_else = 0;
}

static int handle_conditional(char *line)
{
    if (starts_word(line, "ifeq"))  { push_cond(eval_ifeq(lstrip(line + 4), 0)); return 1; }
    if (starts_word(line, "ifneq")) { push_cond(eval_ifeq(lstrip(line + 5), 1)); return 1; }
    if (starts_word(line, "ifdef")) { push_cond(eval_ifdef(lstrip(line + 5), 0)); return 1; }
    if (starts_word(line, "ifndef")){ push_cond(eval_ifdef(lstrip(line + 6), 1)); return 1; }
    if (starts_word(line, "else")) {
        if (condsp == 0) { craft_error(g_cur_file, g_cur_line, "else without if"); return 1; }
        struct cond_frame *f = &condstack[condsp - 1];
        char *rest = lstrip(line + 4);
        int newcond = 1, have_new = 0, neg = 0;
        if (starts_word(rest, "ifeq"))  { newcond = eval_ifeq(lstrip(rest + 4), 0); have_new = 1; }
        else if (starts_word(rest, "ifneq")) { newcond = eval_ifeq(lstrip(rest + 5), 1); have_new = 1; }
        else if (starts_word(rest, "ifdef")) { newcond = eval_ifdef(lstrip(rest + 5), 0); have_new = 1; }
        else if (starts_word(rest, "ifndef")){ newcond = eval_ifdef(lstrip(rest + 6), 1); have_new = 1; }
        (void)neg;
        if (!have_new) {
            if (f->seen_else) craft_error(g_cur_file, g_cur_line, "else after else");
            f->seen_else = 1;
        }
        int take = f->parent_active && !f->any_taken && newcond;
        f->active = take;
        if (take) f->any_taken = 1;
        return 1;
    }
    if (starts_word(line, "endif")) {
        if (condsp == 0) craft_error(g_cur_file, g_cur_line, "endif without if");
        else condsp--;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ define/endef */

static void handle_define(struct lexer *lx, char *line)
{
    char *rest = lstrip(line + 6);
    char *name;
    int oplen = 0;
    char *op = find_assign_op(rest, &oplen);
    if (op) { *op = '\0'; }
    name = expand(trim(rest));

    struct sbuf body;
    sb_init(&body);
    int is_recipe, lno, first = 1;
    char *l;
    while ((l = lex_next(lx, &is_recipe, &lno))) {
        if (!is_recipe && starts_word(l, "endef")) { free(l); break; }
        if (!first) sb_addch(&body, '\n');
        first = 0;
        if (is_recipe) sb_addch(&body, '\t');
        sb_add(&body, l);
        free(l);
    }
    if (cond_active())
        var_set(name, body.data, F_RECUR, OR_FILE);
    sb_free(&body);
    free(name);
}

/* ------------------------------------------------------------ include */

static void do_include(char *line, int optional)
{
    char *rest = line;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    char *e = expand(rest);
    struct strvec files;
    sv_init(&files);
    sv_push_words(&files, e);
    free(e);

    for (size_t i = 0; i < files.len; i++) {
        const char *fn = files.items[i];
        char found[4096];
        FILE *tf = fopen(fn, "rb");
        if (tf) { fclose(tf); parse_makefile(fn, 1); continue; }
        int ok = 0;
        for (size_t d = 0; d < g_include_dirs.len; d++) {
            snprintf(found, sizeof found, "%s/%s", g_include_dirs.items[d], fn);
            tf = fopen(found, "rb");
            if (tf) { fclose(tf); parse_makefile(found, 1); ok = 1; break; }
        }
        if (!ok && !optional)
            craft_error(g_cur_file, g_cur_line,
                        "%s: No such file or directory", fn);
    }
    sv_free(&files);
}

/* ------------------------------------------------------------ top level */

void parse_makefile(const char *path, int required)
{
    struct lexer lx;
    if (!lex_open(&lx, path)) {
        if (required) die("%s: No such file or directory", path);
        return;
    }

    /* keep the file name alive: rules store pointers to it for diagnostics,
       and lex_close() frees the lexer's own copy */
    static struct strvec parsed_names;
    char *fname = xstrdup(path);
    sv_push(&parsed_names, fname);

    const char *save_file = g_cur_file;
    int save_line = g_cur_line;
    g_cur_file = fname;

    int is_recipe, lno;
    char *line;
    while ((line = lex_next(&lx, &is_recipe, &lno))) {
        g_cur_line = lno;

        if (is_recipe) {
            if (cond_active() && g_pending_count) {
                for (size_t i = 0; i < g_pending_count; i++) {
                    sv_push_dup(&g_pending_rules[i]->recipe, line);
                    iv_push(&g_pending_rules[i]->recipe_lno, lno);
                }
            } else if (cond_active()) {
                craft_error(g_cur_file, lno, "recipe commences before first target");
            }
            free(line);
            continue;
        }

        /* directives may be indented with spaces (never a tab - that is a
           recipe).  Work on the left-stripped text from here on. */
        char *t = lstrip(line);

        if (handle_conditional(t)) { free(line); continue; }

        if (!cond_active()) { free(line); continue; }

        if (starts_word(t, "define")) { handle_define(&lx, t); free(line); continue; }

        if (starts_word(t, "include"))  { do_include(t, 0); free(line); continue; }
        if (starts_word(t, "-include")) { do_include(t + 1, 1); free(line); continue; }
        if (starts_word(t, "sinclude")) { do_include(t, 1); free(line); continue; }

        if (starts_word(t, "vpath")) {
            static int warned = 0;
            if (!warned) { warn("vpath is not supported; ignoring"); warned = 1; }
            free(line); continue;
        }
        if (starts_word(t, "undefine")) {
            char *nm = expand(trim(t + 8));
            hm_put(&g_vars, nm, NULL);
            free(nm); free(line); continue;
        }

        int is_override = 0, is_export = 0;
        if (starts_word(t, "override")) { is_override = 1; t = lstrip(t + 8); }
        if (starts_word(t, "export"))   { is_export = 1;   t = lstrip(t + 6); }
        else if (starts_word(t, "unexport")) {
            char *nm = expand(trim(t + 8));
            struct var *v = var_lookup(nm);
            if (v) v->exported = 0;
            free(nm); free(line); continue;
        }

        if (is_export && *t == '\0') { g_export_all = 1; free(line); continue; }

        int oplen = 0;
        if (find_assign_op(t, &oplen)) {
            do_assign(t, is_override, is_export);
        } else if (is_export) {
            struct strvec names;
            sv_init(&names);
            char *ne = expand(t);
            sv_push_words(&names, ne);
            free(ne);
            for (size_t i = 0; i < names.len; i++) {
                struct var *v = var_lookup(names.items[i]);
                if (!v) v = var_set(names.items[i], "", F_RECUR, OR_FILE);
                v->exported = 1;
            }
            sv_free(&names);
        } else {
            do_rule(t, t != line);
        }
        free(line);
    }

    lex_close(&lx);
    g_cur_file = save_file;
    g_cur_line = save_line;
}
