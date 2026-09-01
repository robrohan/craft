/* craft - a Makefile clone in portable C (C99, builds with tcc and clang) */
#ifndef CRAFT_H
#define CRAFT_H

/* Ask the C library for POSIX.1-2008 declarations (popen/pclose, putenv,
   struct stat's st_mtim) even under a strict -std=c99.  Must precede every
   system header, so craft.h is always included first in each .c file. */
#ifndef _WIN32
#  if defined(__APPLE__)
#    ifndef _DARWIN_C_SOURCE
#      define _DARWIN_C_SOURCE 1
#    endif
#  else
#    ifndef _XOPEN_SOURCE
#      define _XOPEN_SOURCE 700
#    endif
#    ifndef _DEFAULT_SOURCE
#      define _DEFAULT_SOURCE 1
#    endif
#  endif
#endif

#include <stddef.h>
#include <time.h>

#define CRAFT_VERSION "0.1.0"

/* ------------------------------------------------------------------ options */
extern const char *program_name;   /* argv[0] basename, for messages   */
extern const char *make_name;      /* value for the MAKE variable      */
extern int opt_keep_going;         /* -k  */
extern int opt_dry_run;            /* -n  */
extern int opt_silent;             /* -s  */
extern int opt_always_make;        /* -B  */
extern int opt_no_builtin;         /* -r  */
extern int opt_env_override;       /* -e  */
extern int opt_ignore_errors;      /* -i  */
extern int opt_print_db;           /* -p  */
extern int opt_question;           /* -q  */
extern int opt_debug;              /* -d  */
extern int makelevel;

/* ------------------------------------------------------------------- util.c */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void  die(const char *fmt, ...);
void  warn(const char *fmt, ...);   /* "craft: ..." to stderr, bumps errors */
void  craft_error(const char *file, int line, const char *fmt, ...);
extern int error_count;

int   is_blank(const char *s);      /* NULL, "" or all-whitespace */
char *lstrip(char *s);              /* in place: skip leading ws, returns s+ */
void  rstrip(char *s);              /* in place: trim trailing whitespace */
char *trim(char *s);                /* lstrip + rstrip */
const char *base_name(const char *path);

/* dynamic string */
struct sbuf { char *data; size_t len, cap; };
void  sb_init(struct sbuf *b);
void  sb_free(struct sbuf *b);
void  sb_addch(struct sbuf *b, char c);
void  sb_add(struct sbuf *b, const char *s);
void  sb_addn(struct sbuf *b, const char *s, size_t n);
char *sb_detach(struct sbuf *b);    /* hand ownership to caller, resets b */

/* growable array of owned char* */
struct strvec { char **items; size_t len, cap; };
void  sv_init(struct strvec *v);
void  sv_free(struct strvec *v);
void  sv_push(struct strvec *v, char *s);          /* takes ownership */
void  sv_push_dup(struct strvec *v, const char *s);
void  sv_push_words(struct strvec *v, const char *s);
void  sv_push_words_uniq(struct strvec *v, const char *s);
int   sv_contains(const struct strvec *v, const char *s);
char *sv_join(const struct strvec *v, const char *sep);   /* malloc'd, "" if empty */

/* growable int array */
struct ivec { int *items; size_t len, cap; };
void iv_push(struct ivec *v, int n);

/* hashmap: duped char* key -> void* value */
struct hentry { char *key; void *val; struct hentry *next; };
struct hmap   { struct hentry **buckets; size_t nbuckets, count; };
void  hm_init(struct hmap *m);
void *hm_get(const struct hmap *m, const char *key);
void  hm_put(struct hmap *m, const char *key, void *val);
#define HM_FOREACH(m, e) \
    for (size_t _hb = 0; _hb < (m)->nbuckets; _hb++) \
        for (struct hentry *e = (m)->buckets[_hb]; e; e = e->next)

/* --------------------------------------------------------------- variables */
enum var_flavor { F_RECUR, F_SIMPLE };
enum var_origin {                 /* low precedence -> high */
    OR_DEFAULT, OR_FILE, OR_ENV, OR_ENV_OVERRIDE, OR_COMMAND, OR_OVERRIDE, OR_AUTO
};
struct var {
    char *name;
    char *value;                  /* recursive: raw; simple: already expanded */
    enum var_flavor flavor;
    enum var_origin origin;
    int   exported;
};
void        vars_init(void);
struct var *var_lookup(const char *name);
struct var *var_set(const char *name, const char *value,
                    enum var_flavor flavor, enum var_origin origin);
void        var_append(const char *name, const char *value, enum var_origin origin);
const char *var_origin_name(enum var_origin o);
extern struct hmap g_vars;

/* automatic-variable context, set while a recipe/prereq list is expanded */
struct autov {
    const char *target;
    const char *stem;                 /* may be NULL */
    struct strvec *prereqs;           /* de-duplicated normal prerequisites */
    struct strvec *prereqs_all;       /* with duplicates */
    struct strvec *prereqs_newer;     /* newer than target (for $?)   */
};
extern struct autov *g_auto;

/* --------------------------------------------------------------- expand.c  */
char *expand(const char *s);                 /* malloc'd fully expanded copy */
void  expand_into(struct sbuf *out, const char *s);
int   is_function_name(const char *name);
int   pattern_match(const char *pat, const char *str, char **stem);
char *pattern_subst_word(const char *pat, const char *repl, const char *str);
char *glob_expand(const char *pattern);
char *os_abspath(const char *p, int must_exist);

/* ---------------------------------------------------------------- rules    */
struct rule {
    struct strvec targets;        /* literal words; may contain '%' */
    struct strvec prereqs;
    struct strvec order_prereqs;
    struct strvec recipe;         /* raw lines, prefix chars retained */
    struct ivec   recipe_lno;     /* source line of each recipe line */
    int  is_pattern;
    int  double_colon;
    int  builtin;
    char *stem;                   /* set for synthesized static-pattern rules */
    const char *deffile; int defline;
    struct rule *next;            /* master list, definition order */
    struct rule *pat_next;        /* pattern-rule list */
};
extern struct rule *g_rules;          /* in definition order */
extern struct hmap  g_phony;          /* name -> (void*)1 */
extern struct hmap  g_precious;
extern struct hmap  g_secondary;
extern int  g_delete_on_error;
extern int  g_oneshell;
extern int  g_export_all;
extern char *g_default_goal;          /* first real target seen */

/* target-specific variables: target name -> struct tsvar* list */
struct tsvar { char *name, *op, *value; struct tsvar *next; };
extern struct hmap g_tsvars;

struct rule *rule_new(void);
void  rule_register(struct rule *r);  /* appends, indexes pattern rules */
void  seed_builtin_rules(void);
void  explicit_rules_foreach(const char *name,
                             void (*cb)(struct rule *, void *), void *ud);
int   has_explicit_rule_with_recipe(const char *name);
extern struct rule *g_pattern_rules;  /* linked via ->pat_next */
extern struct hmap  g_explicit;

/* ---------------------------------------------------------------- build.c  */
struct file {
    char *name;
    int   phony;
    int   exists;
    long long mtime;              /* nanoseconds, monotone for comparison */
    enum { FS_NEW, FS_CONSIDER, FS_DONE } state;
    int   updated;                /* recipe ran / would run */
    int   failed;
    struct strvec deps;
    struct strvec order_deps;
    struct strvec recipe;         /* chosen (still raw) */
    struct ivec   recipe_lno;
    char *stem;                   /* if built via a pattern rule */
    const char *recipe_file; int recipe_line;
};
struct file *file_intern(const char *name);
int   update_goal(const char *name);   /* 0 ok, non-zero on failure */
void  print_database(void);

/* ---------------------------------------------------------------- shell.c  */
int   run_recipe_line(const char *cmdline, int ignore_err, int silent_echo);
char *run_shell_capture(const char *cmdline);   /* $(shell ...) - malloc'd */
int   file_mtime(const char *path, long long *out_ns); /* 1 if exists */
const char *default_shell(void);
void  touch_file(const char *path);

/* ---------------------------------------------------------------- parse.c  */
void  parse_makefile(const char *path, int required);
extern struct strvec g_include_dirs;

#endif /* CRAFT_H */
