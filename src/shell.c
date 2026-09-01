/* craft - OS abstraction: timestamps, globbing, running recipe lines */
#include "craft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #define getcwd _getcwd
  #ifndef popen
    #define popen _popen
    #define pclose _pclose
  #endif
#else
  #include <unistd.h>
  #include <dirent.h>
  #include <sys/wait.h>
#endif

/* ------------------------------------------------------------ timestamps */
int file_mtime(const char *path, long long *out)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!out) return 1;
#if defined(__APPLE__)
    *out = (long long)st.st_mtimespec.tv_sec * 1000000000LL
         + st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    *out = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
    /* Windows / other: 1-second resolution (see README, "Deviations") */
    *out = (long long)st.st_mtime * 1000000000LL;
#endif
    return 1;
}

void touch_file(const char *path)
{
    FILE *f = fopen(path, "ab");
    if (f) fclose(f);
}

/* ------------------------------------------------------------ shell choice */
const char *default_shell(void)
{
#ifdef _WIN32
    return "cmd.exe";
#else
    return "/bin/sh";
#endif
}

static int shell_is_default(const char *sh)
{
    const char *b = base_name(sh);
#ifdef _WIN32
    return _stricmp(b, "cmd.exe") == 0 || _stricmp(b, "cmd") == 0;
#else
    return strcmp(sh, "/bin/sh") == 0 || strcmp(b, "sh") == 0;
#endif
}

static const char *shell_kind(const char *sh)
{
    const char *b = base_name(sh);
    size_t n = strlen(b);
    if (n >= 2 && strcmp(b + n - 2, "sh") == 0) return "sh";
#ifdef _WIN32
    if (_stricmp(b, "cmd.exe") == 0 || _stricmp(b, "cmd") == 0) return "cmd";
#endif
    if (strncmp(b, "powershell", 10) == 0 || strncmp(b, "pwsh", 4) == 0)
        return "pwsh";
    if (n >= 3 && strcmp(b + n - 3, "cmd") == 0) return "cmd";
    return "sh";
}

static const char *current_shell(void)
{
    struct var *v = var_lookup("SHELL");
    if (v && v->value[0]) return v->value;
    return default_shell();
}

/* Build "<shell> <flag> <quoted cmd>" for a non-default shell. */
static char *wrap_command(const char *shell, const char *cmd)
{
    const char *kind = shell_kind(shell);
    struct sbuf b;
    sb_init(&b);
    sb_add(&b, shell);
    if (strcmp(kind, "cmd") == 0) {
        sb_add(&b, " /c \"");
        sb_add(&b, cmd);
        sb_addch(&b, '"');
    } else if (strcmp(kind, "pwsh") == 0) {
        sb_add(&b, " -NoProfile -Command \"");
        for (const char *p = cmd; *p; p++) {
            if (*p == '"') sb_add(&b, "\\\"");
            else sb_addch(&b, *p);
        }
        sb_addch(&b, '"');
    } else {                       /* sh-like: single-quote */
        sb_add(&b, " -c '");
        for (const char *p = cmd; *p; p++) {
            if (*p == '\'') sb_add(&b, "'\\''");
            else sb_addch(&b, *p);
        }
        sb_addch(&b, '\'');
    }
    return sb_detach(&b);
}

static int status_to_code(int rc)
{
#ifdef _WIN32
    return rc;
#else
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
    return 1;
#endif
}

int run_recipe_line(const char *cmdline, int ignore_err, int silent_echo)
{
    if (!silent_echo)
        printf("%s\n", cmdline);
    fflush(stdout);

    if (opt_dry_run)
        return 0;

    const char *shell = current_shell();
    int rc;
    if (shell_is_default(shell)) {
        rc = system(cmdline);
    } else {
        char *wrapped = wrap_command(shell, cmdline);
        rc = system(wrapped);
        free(wrapped);
    }
    int code = status_to_code(rc);
    if (code != 0 && !ignore_err)
        return code;
    if (code != 0 && ignore_err)
        fprintf(stderr, "%s: [%s] Error %d (ignored)\n", program_name, cmdline, code);
    return 0;
}

char *run_shell_capture(const char *cmdline)
{
    FILE *p = popen(cmdline, "r");
    if (!p) return xstrdup("");
    struct sbuf b;
    sb_init(&b);
    int c;
    while ((c = fgetc(p)) != EOF)
        sb_addch(&b, (char)c);
    pclose(p);
    char *s = sb_detach(&b);
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n' || s[i] == '\r') s[i] = ' ';
    return s;
}

/* ------------------------------------------------------------ abspath */
char *os_abspath(const char *p, int must_exist)
{
    if (must_exist) {
        struct stat st;
        if (stat(p, &st) != 0) return xstrdup("");
    }
    char cwd[4096];
    int absolute = (p[0] == '/' || p[0] == '\\' ||
                    (isalpha((unsigned char)p[0]) && p[1] == ':'));
    struct sbuf b;
    sb_init(&b);
    if (!absolute) {
        if (getcwd(cwd, sizeof cwd)) {
            sb_add(&b, cwd);
            sb_addch(&b, '/');
        }
    }
    sb_add(&b, p);

    /* lexically collapse "." and ".." and duplicate slashes */
    char *raw = sb_detach(&b);
    for (char *q = raw; *q; q++) if (*q == '\\') *q = '/';
    struct strvec parts;
    sv_init(&parts);
    char *save = raw;
    int lead_slash = (raw[0] == '/');
    for (char *tok = strtok(save, "/"); tok; tok = strtok(NULL, "/")) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) {
            if (parts.len) { free(parts.items[--parts.len]); }
            continue;
        }
        sv_push_dup(&parts, tok);
    }
    struct sbuf o;
    sb_init(&o);
    if (lead_slash) sb_addch(&o, '/');
    for (size_t i = 0; i < parts.len; i++) {
        if (i) sb_addch(&o, '/');
        sb_add(&o, parts.items[i]);
    }
    sv_free(&parts);
    free(raw);
    if (o.len == 0) sb_addch(&o, '/');
    return sb_detach(&o);
}

/* ------------------------------------------------------------ globbing */

/* glob-style match of a single path component (supports * ? [set]) */
static int wildmatch(const char *pat, const char *str)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (; *str; str++)
                if (wildmatch(pat, str)) return 1;
            return wildmatch(pat, str);
        } else if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
        } else if (*pat == '[') {
            const char *p = pat + 1;
            int neg = (*p == '!' || *p == '^');
            if (neg) p++;
            int matched = 0;
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if ((unsigned char)*str >= (unsigned char)p[0] &&
                        (unsigned char)*str <= (unsigned char)p[2]) matched = 1;
                    p += 3;
                } else {
                    if (*str == *p) matched = 1;
                    p++;
                }
            }
            if (*p == ']') p++;
            if (matched == neg || !*str) return 0;
            pat = p; str++;
        } else {
            if (*pat != *str) return 0;
            pat++; str++;
        }
    }
    return *str == '\0';
}

static int has_wild(const char *s)
{
    return strpbrk(s, "*?[") != NULL;
}

/* list directory DIR entries whose name matches PAT; append "DIR/name" (or
   just "name" when dirprefix is empty) to OUT. */
static void glob_dir(const char *dirprefix, const char *dir, const char *pat,
                     struct strvec *out)
{
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    struct sbuf search;
    sb_init(&search);
    sb_add(&search, dir && *dir ? dir : ".");
    sb_add(&search, "\\*");
    HANDLE h = FindFirstFileA(search.data, &fd);
    sb_free(&search);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char *nm = fd.cFileName;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (!wildmatch(pat, nm)) continue;
        struct sbuf b;
        sb_init(&b);
        if (dirprefix && *dirprefix) sb_add(&b, dirprefix);
        sb_add(&b, nm);
        sv_push(out, sb_detach(&b));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir && *dir ? dir : ".");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (pat[0] != '.' && e->d_name[0] == '.') continue;
        if (!wildmatch(pat, e->d_name)) continue;
        struct sbuf b;
        sb_init(&b);
        if (dirprefix && *dirprefix) sb_add(&b, dirprefix);
        sb_add(&b, e->d_name);
        sv_push(out, sb_detach(&b));
    }
    closedir(d);
#endif
}

static int wordcmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

char *glob_expand(const char *pattern)
{
    if (!has_wild(pattern)) {
        struct stat st;
        return xstrdup(stat(pattern, &st) == 0 ? pattern : "");
    }

    /* split into leading directory (assumed literal) + final component */
    const char *slash = NULL;
    for (const char *p = pattern; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;

    struct strvec out;
    sv_init(&out);
    if (slash) {
        char *dir = xstrndup(pattern, (size_t)(slash - pattern + 1));
        char *dbare = xstrndup(pattern, (size_t)(slash - pattern));
        glob_dir(dir, dbare, slash + 1, &out);
        free(dir);
        free(dbare);
    } else {
        glob_dir("", ".", pattern, &out);
    }

    if (out.len > 1)
        qsort(out.items, out.len, sizeof out.items[0], wordcmp);
    char *r = sv_join(&out, " ");
    sv_free(&out);
    return r;
}
