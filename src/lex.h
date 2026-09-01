/* craft - lexer interface (see lex.c) */
#ifndef CRAFT_LEX_H
#define CRAFT_LEX_H

#include <stddef.h>

struct lexer {
    char  *buf;
    size_t len, pos;
    char  *fname;
    int    lineno;
};

int   lex_open(struct lexer *lx, const char *path);
void  lex_close(struct lexer *lx);
char *lex_next(struct lexer *lx, int *is_recipe, int *lineno_out);

#endif
