/* craft - a Makefile clone in portable C.  Entry point / CLI. */
#include "craft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
  #include <direct.h>
  #define chdir _chdir
  #define getcwd _getcwd
  extern char **_environ;
  #define environ _environ
#else
  #include <unistd.h>
  extern char **environ;
#endif

/* portable environ set: setenv() is POSIX-only, _putenv_s() is tcc-version
   dependent; putenv() with a persistent string works everywhere. */
static void env_put(const char *name, const char *val)
{
    size_t n = strlen(name) + strlen(val) + 2;
    char *s = xmalloc(n);              /* deliberately leaked: putenv keeps it */
    snprintf(s, n, "%s=%s", name, val);
    putenv(s);
}

const char *program_name = "craft";
const char *make_name = "craft";
int opt_keep_going = 0, opt_dry_run = 0, opt_silent = 0, opt_always_make = 0;
int opt_no_builtin = 0, opt_env_override = 0, opt_ignore_errors = 0;
int opt_print_db = 0, opt_question = 0, opt_debug = 0;
int makelevel = 0;

static struct strvec makefiles;      /* -f */
static struct strvec goals;
static struct strvec cmdline_vars;   /* "NAME=VALUE" */
static struct strvec chdirs;         /* -C */

static void usage(FILE *o)
{
    fprintf(o,
"Usage: %s [options] [target] ... [VAR=VALUE] ...\n"
"\n"
"  -f FILE      read FILE as the makefile\n"
"  -C DIR       change to DIR before doing anything\n"
"  -I DIR       search DIR for included makefiles\n"
"  -n           print recipes without executing them\n"
"  -k           keep going when a target fails\n"
"  -s           do not echo recipes\n"
"  -B           unconditionally rebuild all targets\n"
"  -e           environment variables override the makefile\n"
"  -i           ignore all recipe errors\n"
"  -r           disable built-in rules and variables\n"
"  -p           print the data base (variables and rules)\n"
"  -q           question mode: exit status only, no recipes\n"
"  -j [N]       accepted for compatibility; builds are always serial\n"
"  -h           this help\n"
"  -v           version\n",
        program_name);
}

static int is_assignment_arg(const char *s)
{
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    for (const char *p = s; *p; p++) {
        if (*p == '=') return p != s;
        if (*p == ':' && p[1] == '=') return 1;
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.') return 0;
    }
    return 0;
}

static void apply_cmdline_var(const char *s)
{
    const char *eq = strstr(s, ":=");
    int simple = eq != NULL;
    if (!eq) eq = strchr(s, '=');
    if (!eq) return;
    char *name = xstrndup(s, (size_t)(eq - s));
    const char *val = eq + (simple ? 2 : 1);
    if (simple) {
        char *e = expand(val);
        var_set(name, e, F_SIMPLE, OR_COMMAND);
        free(e);
    } else {
        var_set(name, val, F_RECUR, OR_COMMAND);
    }
    struct var *v = var_lookup(name);
    if (v) v->exported = 1;
    free(name);
}

static void import_environment(void)
{
    enum var_origin o = opt_env_override ? OR_ENV_OVERRIDE : OR_ENV;
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *name = xstrndup(*e, (size_t)(eq - *e));
        if (strcmp(name, "SHELL") != 0 && strcmp(name, "MAKELEVEL") != 0) {
            struct var *v = var_set(name, eq + 1, F_RECUR, o);
            if (v) v->exported = 1;
        }
        free(name);
    }
    const char *ml = getenv("MAKELEVEL");
    makelevel = ml ? atoi(ml) : 0;
}

/* returns 1 if it consumed a following argument */
static int handle_long(const char *a, const char *next, int *consumed)
{
    *consumed = 0;
    if (!strcmp(a, "--dry-run") || !strcmp(a, "--just-print") ||
        !strcmp(a, "--recon")) opt_dry_run = 1;
    else if (!strcmp(a, "--keep-going")) opt_keep_going = 1;
    else if (!strcmp(a, "--silent") || !strcmp(a, "--quiet")) opt_silent = 1;
    else if (!strcmp(a, "--always-make")) opt_always_make = 1;
    else if (!strcmp(a, "--environment-overrides")) opt_env_override = 1;
    else if (!strcmp(a, "--ignore-errors")) opt_ignore_errors = 1;
    else if (!strcmp(a, "--no-builtin-rules") ||
             !strcmp(a, "--no-builtin-variables")) opt_no_builtin = 1;
    else if (!strcmp(a, "--print-data-base")) opt_print_db = 1;
    else if (!strcmp(a, "--question")) opt_question = 1;
    else if (!strcmp(a, "--print-directory") ||
             !strcmp(a, "--no-print-directory")) { /* accepted, ignored */ }
    else if (!strcmp(a, "--version")) { printf("craft %s\n", CRAFT_VERSION); exit(0); }
    else if (!strcmp(a, "--help")) { usage(stdout); exit(0); }
    else if (!strcmp(a, "--file") || !strcmp(a, "--makefile")) {
        if (next) { sv_push_dup(&makefiles, next); *consumed = 1; }
    }
    else if (!strncmp(a, "--file=", 7)) sv_push_dup(&makefiles, a + 7);
    else if (!strncmp(a, "--makefile=", 11)) sv_push_dup(&makefiles, a + 11);
    else if (!strcmp(a, "--directory")) {
        if (next) { sv_push_dup(&chdirs, next); *consumed = 1; }
    }
    else if (!strncmp(a, "--directory=", 12)) sv_push_dup(&chdirs, a + 12);
    else if (!strncmp(a, "--include-dir=", 14)) sv_push_dup(&g_include_dirs, a + 14);
    else warn("unknown option '%s' (ignored)", a);
    return *consumed;
}

static void parse_flag_cluster(const char *arg, char **argv, int argc, int *ip)
{
    for (const char *p = arg + 1; *p; p++) {
        switch (*p) {
        case 'n': opt_dry_run = 1; break;
        case 'k': opt_keep_going = 1; break;
        case 's': opt_silent = 1; break;
        case 'B': opt_always_make = 1; break;
        case 'e': opt_env_override = 1; break;
        case 'i': opt_ignore_errors = 1; break;
        case 'r': case 'R': opt_no_builtin = 1; break;
        case 'p': opt_print_db = 1; break;
        case 'q': opt_question = 1; break;
        case 'd': opt_debug = 1; break;
        case 'w': break;
        case 'h': usage(stdout); exit(0);
        case 'v': case 'V': printf("craft %s\n", CRAFT_VERSION); exit(0);
        case 'f': case 'C': case 'I': case 'o': case 'W': case 'j': {
            char opt = *p;
            const char *val = p[1] ? p + 1 : NULL;
            if (!val && *ip + 1 < argc && opt != 'j') val = argv[++(*ip)];
            else if (!val && opt == 'j') { val = NULL; }
            if (opt == 'f' && val) sv_push_dup(&makefiles, val);
            else if (opt == 'C' && val) sv_push_dup(&chdirs, val);
            else if (opt == 'I' && val) sv_push_dup(&g_include_dirs, val);
            else if (opt == 'j') { /* serial only */ }
            /* -o / -W: accepted, ignored in v1 */
            return;   /* rest of cluster consumed as the value */
        }
        default:
            warn("unknown option -%c (ignored)", *p);
        }
    }
}

static void seed_make_vars(int argc, char **argv)
{
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) var_set("CURDIR", cwd, F_SIMPLE, OR_FILE);

    var_set("MAKE", argv[0], F_RECUR, OR_DEFAULT);
    var_set(".RECIPEPREFIX", "", F_SIMPLE, OR_DEFAULT);

    char lvl[16];
    snprintf(lvl, sizeof lvl, "%d", makelevel);
    var_set("MAKELEVEL", lvl, F_SIMPLE, OR_FILE);

    struct sbuf mf;
    sb_init(&mf);
    if (opt_keep_going) sb_addch(&mf, 'k');
    if (opt_dry_run) sb_addch(&mf, 'n');
    if (opt_silent) sb_addch(&mf, 's');
    if (opt_ignore_errors) sb_addch(&mf, 'i');
    if (opt_no_builtin) sb_addch(&mf, 'r');
    var_set("MAKEFLAGS", mf.data, F_SIMPLE, OR_FILE);
    sb_free(&mf);

    /* export MAKE / MAKELEVEL / MAKEFLAGS for sub-makes */
    struct var *v;
    if ((v = var_lookup("MAKE"))) v->exported = 1;
    if ((v = var_lookup("MAKEFLAGS"))) v->exported = 1;
    char sub[16];
    snprintf(sub, sizeof sub, "%d", makelevel + 1);
    env_put("MAKELEVEL", sub);
    (void)argc;
}

static void export_all_marked(void)
{
    HM_FOREACH(&g_vars, e) {
        struct var *vv = e->val;
        if (!vv) continue;
        if (!vv->exported && !g_export_all) continue;
        if (vv->name[0] == '.') continue;
        char *val = vv->flavor == F_RECUR ? expand(vv->value)
                                          : xstrdup(vv->value);
        env_put(vv->name, val);
        free(val);
    }
}

int main(int argc, char **argv)
{
    program_name = base_name(argv[0]);
    vars_init();
    sv_init(&makefiles);
    sv_init(&goals);
    sv_init(&cmdline_vars);
    sv_init(&chdirs);
    sv_init(&g_include_dirs);

    int only_files = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!only_files && !strcmp(a, "--")) { only_files = 1; continue; }
        if (!only_files && a[0] == '-' && a[1] == '-') {
            int consumed = 0;
            handle_long(a, (i + 1 < argc) ? argv[i + 1] : NULL, &consumed);
            if (consumed) i++;
            continue;
        }
        if (!only_files && a[0] == '-' && a[1]) {
            parse_flag_cluster(a, argv, argc, &i);
            continue;
        }
        if (is_assignment_arg(a)) sv_push_dup(&cmdline_vars, a);
        else sv_push_dup(&goals, a);
    }

    for (size_t i = 0; i < chdirs.len; i++)
        if (chdir(chdirs.items[i]) != 0)
            die("%s: cannot change directory", chdirs.items[i]);

    import_environment();
    seed_make_vars(argc, argv);
    seed_builtin_rules();

    for (size_t i = 0; i < cmdline_vars.len; i++)
        apply_cmdline_var(cmdline_vars.items[i]);

    if (opt_question) { opt_dry_run = 1; opt_silent = 1; }

    /* discover the makefile */
    if (makefiles.len == 0) {
        static const char *names[] = {
            "Craftfile", "craftfile", "GNUmakefile", "makefile", "Makefile", NULL
        };
        for (int i = 0; names[i]; i++) {
            FILE *f = fopen(names[i], "rb");
            if (f) { fclose(f); sv_push_dup(&makefiles, names[i]); break; }
        }
        if (makefiles.len == 0)
            die("No makefile found (looked for Craftfile, GNUmakefile, "
                "makefile, Makefile)");
    }

    for (size_t i = 0; i < makefiles.len; i++)
        parse_makefile(makefiles.items[i], 1);

    if (error_count)
        die("%d error%s while reading makefiles", error_count,
            error_count == 1 ? "" : "s");

    if (opt_print_db)
        print_database();

    export_all_marked();

    if (goals.len == 0) {
        if (g_default_goal) sv_push_dup(&goals, g_default_goal);
        else die("No targets.  Stop.");
    }

    int any_updated = 0, failed = 0;
    for (size_t i = 0; i < goals.len; i++) {
        int r = update_goal(goals.items[i]);
        struct file *gf = file_intern(goals.items[i]);
        if (gf->updated) any_updated = 1;
        if (r != 0) {
            failed = 1;
            if (!opt_keep_going) break;
        }
    }

    if (failed || error_count)
        return 2;
    if (opt_question)
        return any_updated ? 1 : 0;
    return 0;
}
