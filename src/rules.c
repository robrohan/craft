/* craft - rule registry, indexing, built-in rules */
#include "craft.h"

#include <stdlib.h>
#include <string.h>

struct rule *g_rules = NULL;
static struct rule *g_rules_tail = NULL;

struct rule *g_pattern_rules = NULL;
static struct rule *g_pattern_tail = NULL;

struct hmap g_phony, g_precious, g_secondary, g_tsvars;
struct hmap g_explicit;                 /* target -> struct rrec* */

int   g_delete_on_error = 0;
int   g_oneshell = 0;
int   g_export_all = 0;
char *g_default_goal = NULL;

struct rrec { struct rule *r; struct rrec *next; };

struct rule *rule_new(void)
{
    struct rule *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    sv_init(&r->targets);
    sv_init(&r->prereqs);
    sv_init(&r->order_prereqs);
    sv_init(&r->recipe);
    return r;
}

void rule_register(struct rule *r)
{
    if (g_rules_tail) g_rules_tail->next = r;
    else g_rules = r;
    g_rules_tail = r;

    for (size_t i = 0; i < r->targets.len; i++) {
        if (strchr(r->targets.items[i], '%')) {
            r->is_pattern = 1;
        }
    }
    if (r->is_pattern) {
        if (g_pattern_tail) g_pattern_tail->pat_next = r;
        else g_pattern_rules = r;
        g_pattern_tail = r;
    } else {
        for (size_t i = 0; i < r->targets.len; i++) {
            struct rrec *rc = xmalloc(sizeof *rc);
            rc->r = r;
            rc->next = NULL;
            struct rrec *head = hm_get(&g_explicit, r->targets.items[i]);
            if (!head) {
                hm_put(&g_explicit, r->targets.items[i], rc);
            } else {
                while (head->next) head = head->next;
                head->next = rc;
            }
        }
    }
}

/* iterate explicit rules for NAME via callback */
void explicit_rules_foreach(const char *name,
                            void (*cb)(struct rule *, void *), void *ud)
{
    for (struct rrec *rc = hm_get(&g_explicit, name); rc; rc = rc->next)
        cb(rc->r, ud);
}

int has_explicit_rule_with_recipe(const char *name)
{
    for (struct rrec *rc = hm_get(&g_explicit, name); rc; rc = rc->next)
        if (rc->r->recipe.len) return 1;
    return 0;
}

/* ------------------------------------------------------------ built-ins */

static struct rule *mkpat(const char *target, const char *prereq,
                          const char *recipe)
{
    struct rule *r = rule_new();
    sv_push_dup(&r->targets, target);
    if (prereq && *prereq) sv_push_dup(&r->prereqs, prereq);
    sv_push_dup(&r->recipe, recipe);
    iv_push(&r->recipe_lno, 0);
    r->builtin = 1;
    rule_register(r);
    return r;
}

void seed_builtin_rules(void)
{
    /* Real defaults only; flag variables (CFLAGS, LDFLAGS, ...) stay undefined
       so that `?=' in a makefile can still set them. */
    var_set("CC",      "cc",  F_RECUR, OR_DEFAULT);
    var_set("CXX",     "g++", F_RECUR, OR_DEFAULT);
    var_set("AR",      "ar",  F_RECUR, OR_DEFAULT);
    var_set("ARFLAGS", "rv",  F_RECUR, OR_DEFAULT);
#ifdef _WIN32
    var_set("RM", "del /f /q", F_RECUR, OR_DEFAULT);
#else
    var_set("RM", "rm -f",     F_RECUR, OR_DEFAULT);
#endif

    if (opt_no_builtin) return;

    mkpat("%.o", "%.c",
          "$(CC) $(CPPFLAGS) $(CFLAGS) $(TARGET_ARCH) -c -o $@ $<");
    mkpat("%.o", "%.cc",
          "$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TARGET_ARCH) -c -o $@ $<");
    mkpat("%.o", "%.cpp",
          "$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TARGET_ARCH) -c -o $@ $<");
    mkpat("%", "%.c",
          "$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(TARGET_ARCH) $< $(LOADLIBES) $(LDLIBS) -o $@");
    mkpat("%", "%.o",
          "$(CC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LOADLIBES) $(LDLIBS) -o $@");
}
