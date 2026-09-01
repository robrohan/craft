/* craft - variable table, flavors, origins, precedence */
#include "craft.h"

#include <stdlib.h>
#include <string.h>

struct hmap g_vars;
struct autov *g_auto = NULL;

void vars_init(void)
{
    hm_init(&g_vars);
}

const char *var_origin_name(enum var_origin o)
{
    switch (o) {
    case OR_DEFAULT:      return "default";
    case OR_FILE:         return "makefile";
    case OR_ENV:          return "environment";
    case OR_ENV_OVERRIDE: return "environment override";
    case OR_COMMAND:      return "command line";
    case OR_OVERRIDE:     return "override";
    case OR_AUTO:         return "automatic";
    }
    return "?";
}

struct var *var_lookup(const char *name)
{
    return hm_get(&g_vars, name);
}

/* A new definition wins only if its origin is at least as strong as the
   existing one.  This mirrors GNU make's precedence rules. */
static int origin_wins(enum var_origin incoming, enum var_origin existing)
{
    return (int)incoming >= (int)existing;
}

struct var *var_set(const char *name, const char *value,
                    enum var_flavor flavor, enum var_origin origin)
{
    struct var *v = var_lookup(name);
    if (v) {
        if (!origin_wins(origin, v->origin))
            return v;
        free(v->value);
        v->value = xstrdup(value);
        v->flavor = flavor;
        v->origin = origin;
        return v;
    }
    v = xmalloc(sizeof *v);
    v->name = xstrdup(name);
    v->value = xstrdup(value);
    v->flavor = flavor;
    v->origin = origin;
    v->exported = 0;
    hm_put(&g_vars, name, v);
    return v;
}

/* `+=` : append, keeping the existing flavor.  For a simple variable the
   appended text is expanded now; for a recursive one it is stored raw. */
void var_append(const char *name, const char *value, enum var_origin origin)
{
    struct var *v = var_lookup(name);
    if (!v) {
        var_set(name, value, F_RECUR, origin);
        return;
    }
    if (!origin_wins(origin, v->origin))
        return;

    char *add;
    if (v->flavor == F_SIMPLE)
        add = expand(value);
    else
        add = xstrdup(value);

    size_t n = strlen(v->value);
    char *joined = xmalloc(n + 1 + strlen(add) + 1);
    memcpy(joined, v->value, n);
    joined[n] = n ? ' ' : '\0';
    strcpy(joined + n + (n ? 1 : 0), add);
    free(add);
    free(v->value);
    v->value = joined;
    if (origin > v->origin) v->origin = origin;
}
