/* craft - the update engine: dependency graph, timestamps, recipe execution */
#include "craft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
  #include <unistd.h>
#endif

static struct hmap g_files;
static int g_files_ready = 0;

extern const char *g_cur_file;
extern int g_cur_line;

static const char *norm(const char *name)
{
    while (name[0] == '.' && (name[1] == '/' || name[1] == '\\'))
        name += 2;
    return name;
}

struct file *file_intern(const char *name)
{
    if (!g_files_ready) { hm_init(&g_files); g_files_ready = 1; }
    name = norm(name);
    struct file *f = hm_get(&g_files, name);
    if (f) return f;
    f = xmalloc(sizeof *f);
    memset(f, 0, sizeof *f);
    f->name = xstrdup(name);
    sv_init(&f->deps);
    sv_init(&f->order_deps);
    sv_init(&f->recipe);
    f->exists = file_mtime(f->name, &f->mtime);
    f->phony = hm_get(&g_phony, f->name) != NULL;
    hm_put(&g_files, f->name, f);
    return f;
}

/* ------------------------------------------------------------ rule binding */

struct collect_ctx { struct file *f; struct rule *recipe_rule; int nrecipe; };

static void collect_cb(struct rule *r, void *ud)
{
    struct collect_ctx *c = ud;
    for (size_t i = 0; i < r->prereqs.len; i++)
        if (!sv_contains(&c->f->deps, r->prereqs.items[i]))
            sv_push_dup(&c->f->deps, r->prereqs.items[i]);
    for (size_t i = 0; i < r->order_prereqs.len; i++)
        if (!sv_contains(&c->f->order_deps, r->order_prereqs.items[i]))
            sv_push_dup(&c->f->order_deps, r->order_prereqs.items[i]);
    if (r->recipe.len) {
        if (!r->double_colon) {
            if (c->nrecipe && !r->builtin)
                warn("%s:%d: warning: overriding recipe for target '%s'",
                     r->deffile ? r->deffile : "?", r->defline, c->f->name);
            c->f->recipe.len = 0;
            c->f->recipe_lno.len = 0;
        }
        for (size_t i = 0; i < r->recipe.len; i++) {
            sv_push_dup(&c->f->recipe, r->recipe.items[i]);
            iv_push(&c->f->recipe_lno,
                    i < r->recipe_lno.len ? r->recipe_lno.items[i] : r->defline);
        }
        c->recipe_rule = r;
        c->nrecipe++;
        c->f->recipe_file = r->deffile;
        c->f->recipe_line = r->defline;
        if (r->stem && !c->f->stem) c->f->stem = xstrdup(r->stem);
    }
}

/* can NAME be produced (exists, or explicit recipe, or a pattern chain)? */
static int can_make(const char *name, int depth)
{
    name = norm(name);
    long long mt;
    if (file_mtime(name, &mt)) return 1;
    if (has_explicit_rule_with_recipe(name)) return 1;
    if (depth > 8) return 0;
    for (struct rule *pr = g_pattern_rules; pr; pr = pr->pat_next) {
        if (!pr->recipe.len) continue;
        for (size_t t = 0; t < pr->targets.len; t++) {
            char *stem = NULL;
            if (!pattern_match(pr->targets.items[t], name, &stem)) continue;
            int ok = 1;
            for (size_t p = 0; p < pr->prereqs.len && ok; p++) {
                struct sbuf b; sb_init(&b);
                for (const char *q = pr->prereqs.items[p]; *q; q++)
                    if (*q == '%') sb_add(&b, stem); else sb_addch(&b, *q);
                char *cand = sb_detach(&b);
                if (!can_make(cand, depth + 1)) ok = 0;
                free(cand);
            }
            free(stem);
            if (ok) return 1;
        }
    }
    return 0;
}

static int implicit_search(struct file *f)
{
    struct rule *best = NULL;
    char *best_stem = NULL;
    struct strvec best_prereqs;
    sv_init(&best_prereqs);

    for (struct rule *pr = g_pattern_rules; pr; pr = pr->pat_next) {
        if (!pr->recipe.len) continue;
        for (size_t t = 0; t < pr->targets.len; t++) {
            char *stem = NULL;
            if (!pattern_match(pr->targets.items[t], f->name, &stem)) continue;

            struct strvec cand;
            sv_init(&cand);
            int ok = 1;
            for (size_t p = 0; p < pr->prereqs.len; p++) {
                struct sbuf b; sb_init(&b);
                for (const char *q = pr->prereqs.items[p]; *q; q++)
                    if (*q == '%') sb_add(&b, stem); else sb_addch(&b, *q);
                char *c = sb_detach(&b);
                if (!can_make(c, 0)) ok = 0;
                sv_push(&cand, c);
            }
            int better;
            if (!best)                          better = 1;
            else if (best->builtin && !pr->builtin) better = 1;
            else if (!best->builtin && pr->builtin) better = 0;
            else better = strlen(stem) < strlen(best_stem);

            if (ok && better) {
                free(best_stem);
                best_stem = xstrdup(stem);
                sv_free(&best_prereqs);
                best_prereqs = cand;
                cand.items = NULL; cand.len = cand.cap = 0;
                best = pr;
            } else {
                sv_free(&cand);
            }
            free(stem);
        }
    }

    if (!best) { sv_free(&best_prereqs); return 0; }

    /* the implicit rule's prerequisites come first, so that $< and $^ match
       GNU make (implicit prereq before any explicit prerequisites) */
    struct strvec merged;
    sv_init(&merged);
    for (size_t i = 0; i < best_prereqs.len; i++)
        if (!sv_contains(&merged, best_prereqs.items[i]))
            sv_push_dup(&merged, best_prereqs.items[i]);
    for (size_t i = 0; i < f->deps.len; i++)
        if (!sv_contains(&merged, f->deps.items[i]))
            sv_push_dup(&merged, f->deps.items[i]);
    sv_free(&f->deps);
    f->deps = merged;

    f->recipe.len = 0;
    f->recipe_lno.len = 0;
    for (size_t i = 0; i < best->recipe.len; i++) {
        sv_push_dup(&f->recipe, best->recipe.items[i]);
        iv_push(&f->recipe_lno,
                i < best->recipe_lno.len ? best->recipe_lno.items[i] : best->defline);
    }
    free(f->stem);
    f->stem = best_stem;
    f->recipe_file = best->deffile;
    f->recipe_line = best->defline;
    sv_free(&best_prereqs);
    return 1;
}

/* ------------------------------------------------------------ recipe run */

struct var *push_temp_var(const char *name, const char *value);
void        pop_temp_var(const char *name, struct var *old);

static int apply_tsvars(struct file *f, struct var **saved, char (*names)[64])
{
    int n = 0;
    /* entries were pushed head-first; reverse to apply in source order */
    struct tsvar *list = hm_get(&g_tsvars, f->name);
    struct tsvar *arr[64];
    int m = 0;
    for (struct tsvar *tv = list; tv && m < 64; tv = tv->next) arr[m++] = tv;
    for (int i = m - 1; i >= 0 && n < 64; i--) {
        struct tsvar *tv = arr[i];
        struct var *cur = var_lookup(tv->name);
        char *val;
        if (strcmp(tv->op, "+=") == 0 && cur && cur->value[0]) {
            char *add = expand(tv->value);
            size_t bl = strlen(cur->value);
            val = xmalloc(bl + strlen(add) + 2);
            memcpy(val, cur->value, bl);
            val[bl] = ' ';
            strcpy(val + bl + 1, add);
            free(add);
        } else {
            val = expand(tv->value);
        }
        snprintf(names[n], 64, "%s", tv->name);
        saved[n] = push_temp_var(tv->name, val);
        free(val);
        n++;
    }
    return n;
}

static int is_silent_line(const char *line, int *ignore, int *force,
                          const char **body)
{
    int silent = 0;
    const char *p = line;
    for (;;) {
        if (*p == '@') { silent = 1; p++; }
        else if (*p == '-') { *ignore = 1; p++; }
        else if (*p == '+') { *force = 1; p++; }
        else break;
    }
    *body = p;
    return silent;
}

static int run_recipe(struct file *f)
{
    struct strvec pre_uniq, pre_all, pre_newer;
    sv_init(&pre_uniq); sv_init(&pre_all); sv_init(&pre_newer);
    for (size_t i = 0; i < f->deps.len; i++) {
        sv_push_dup(&pre_all, f->deps.items[i]);
        if (!sv_contains(&pre_uniq, f->deps.items[i]))
            sv_push_dup(&pre_uniq, f->deps.items[i]);
        struct file *df = file_intern(f->deps.items[i]);
        if (!f->exists || df->updated || (df->exists && df->mtime > f->mtime))
            sv_push_dup(&pre_newer, f->deps.items[i]);
    }

    struct autov av;
    av.target = f->name;
    av.stem = f->stem;
    av.prereqs = &pre_uniq;
    av.prereqs_all = &pre_all;
    av.prereqs_newer = &pre_newer;
    struct autov *save_auto = g_auto;
    g_auto = &av;

    struct var *saved[64];
    char names[64][64];
    int nsaved = apply_tsvars(f, saved, names);

    const char *rf = f->recipe_file ? f->recipe_file : g_cur_file;
    struct var *mkv = var_lookup("MAKE");
    const char *makeval = mkv ? mkv->value : NULL;

    int rc = 0;
    struct sbuf oneshell;
    if (g_oneshell) sb_init(&oneshell);

    for (size_t i = 0; i < f->recipe.len; i++) {
        int ignore = 0, force = 0;
        const char *body;
        int silent = is_silent_line(f->recipe.items[i], &ignore, &force, &body);
        if (opt_ignore_errors) ignore = 1;
        if (opt_silent) silent = 1;

        char *cmd = expand(body);
        if (is_blank(cmd)) { free(cmd); continue; }

        if (makeval && *makeval && strstr(cmd, makeval)) force = 1;

        if (g_oneshell) {
            if (oneshell.len) sb_addch(&oneshell, '\n');
            sb_add(&oneshell, cmd);
            free(cmd);
            continue;
        }

        if (opt_dry_run && !force) {
            printf("%s\n", cmd);
            free(cmd);
            continue;
        }

        int r = run_recipe_line(cmd, ignore, silent);
        free(cmd);
        if (r != 0) {
            int eln = i < f->recipe_lno.len ? f->recipe_lno.items[i]
                                            : f->recipe_line;
            fprintf(stderr, "%s: *** [%s:%d: %s] Error %d\n",
                    program_name, rf ? rf : "?", eln, f->name, r);
            if (g_delete_on_error && !hm_get(&g_precious, f->name)) {
                if (file_mtime(f->name, NULL)) {
                    fprintf(stderr, "%s: *** Deleting file '%s'\n",
                            program_name, f->name);
                    remove(f->name);
                }
            }
            rc = r;
            break;
        }
    }

    if (g_oneshell && rc == 0 && oneshell.len) {
        if (opt_dry_run) printf("%s\n", oneshell.data);
        else rc = run_recipe_line(oneshell.data, opt_ignore_errors, opt_silent);
        sb_free(&oneshell);
    } else if (g_oneshell) {
        sb_free(&oneshell);
    }

    for (int i = nsaved - 1; i >= 0; i--)
        pop_temp_var(names[i], saved[i]);

    g_auto = save_auto;
    sv_free(&pre_uniq); sv_free(&pre_all); sv_free(&pre_newer);
    return rc;
}

/* ------------------------------------------------------------ update */

static int update_file(struct file *f)
{
    if (f->state == FS_DONE) return f->failed ? -1 : 0;
    if (f->state == FS_CONSIDER) {
        warn("Circular %s <- %s dependency dropped.", f->name, f->name);
        return 0;
    }
    f->state = FS_CONSIDER;

    struct collect_ctx c = { f, NULL, 0 };
    explicit_rules_foreach(f->name, collect_cb, &c);

    if (!f->recipe.len)
        implicit_search(f);

    if (hm_get(&g_phony, f->name)) f->phony = 1;

    int fail = 0;

    for (size_t i = 0; i < f->deps.len; i++) {
        struct file *df = file_intern(f->deps.items[i]);
        int r = update_file(df);
        if (r != 0) {
            fail = 1;
            if (!opt_keep_going) goto done;
        }
    }
    for (size_t i = 0; i < f->order_deps.len; i++) {
        struct file *df = file_intern(f->order_deps.items[i]);
        if (update_file(df) != 0 && !opt_keep_going) { fail = 1; goto done; }
    }

    f->exists = file_mtime(f->name, &f->mtime);

    int need = 0;
    if (f->phony) need = 1;
    if (!f->exists) need = 1;
    if (opt_always_make && f->recipe.len) need = 1;

    for (size_t i = 0; i < f->deps.len; i++) {
        struct file *df = file_intern(f->deps.items[i]);
        if (df->updated) need = 1;
        if (df->exists && f->exists && df->mtime > f->mtime) need = 1;
    }

    if (!f->exists && !f->recipe.len && f->deps.len == 0 && !f->phony) {
        craft_error(g_cur_file, g_cur_line,
                    "No rule to make target '%s'", f->name);
        fail = 1;
        goto done;
    }

    if (need && f->recipe.len && !fail) {
        int r = run_recipe(f);
        f->updated = 1;
        if (r != 0) { fail = 1; }
        if (!opt_dry_run) f->exists = file_mtime(f->name, &f->mtime);
    } else if (need && !f->recipe.len) {
        /* an aggregator target (recipe-less): it counts as updated if any of
           its prerequisites were rebuilt */
        for (size_t i = 0; i < f->deps.len; i++) {
            struct file *df = file_intern(f->deps.items[i]);
            if (df->updated) f->updated = 1;
        }
    }

done:
    f->state = FS_DONE;
    f->failed = fail;
    return fail ? -1 : 0;
}

int update_goal(const char *name)
{
    struct file *f = file_intern(name);
    int r = update_file(f);
    if (r != 0)
        return r;

    if (!f->updated) {
        if (f->exists)
            printf("%s: '%s' is up to date.\n", program_name, name);
        else
            printf("%s: Nothing to be done for '%s'.\n", program_name, name);
    }
    return 0;
}

/* ------------------------------------------------------------ -p dump */

void print_database(void)
{
    printf("# craft %s\n# Variables\n\n", CRAFT_VERSION);
    HM_FOREACH(&g_vars, e) {
        struct var *v = e->val;
        if (!v) continue;
        printf("# %s\n%s %s= %s\n", var_origin_name(v->origin), v->name,
               v->flavor == F_SIMPLE ? ":" : "", v->value);
    }
    printf("\n# Rules\n\n");
    for (struct rule *r = g_rules; r; r = r->next) {
        char *t = sv_join(&r->targets, " ");
        char *p = sv_join(&r->prereqs, " ");
        printf("%s:%s %s\n", t, r->double_colon ? ":" : "", p);
        for (size_t i = 0; i < r->recipe.len; i++)
            printf("\t%s\n", r->recipe.items[i]);
        free(t); free(p);
    }
}
