/*
 * srpc_rpcgen — генератор RPC-стабов из libsrpc_rpc_functions.txt
 *
 * Использование: srpc_rpcgen <input.txt> <outdir>
 *
 * Формат входа:
 *   #include <header.h> / #include "header.h"
 *   rettype name(type name, ...) [KEY=VALUE ...];
 *
 * RPC_MODE по умолчанию — RPC_SEND_ALL.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_PARAMS  32
#define MAX_FLAGS   16
#define MAX_FUNCS   256
#define MAX_INCLS   64

static const char *g_infile = "";
static int g_errcnt = 0;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "srpc_rpcgen: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void err_at(int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: ", g_infile, line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_errcnt++;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p)
        die("out of memory");
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p)
        die("out of memory");
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static bool is_type_kw(const char *s)
{
    static const char *kw[] = {
        "void", "char", "short", "int", "long", "signed", "unsigned",
        "float", "double", "const", "volatile", "restrict",
        "struct", "union", "enum", "_Bool", "bool", "_Atomic",
        NULL
    };
    for (int i = 0; kw[i]; i++) {
        if (strcmp(s, kw[i]) == 0)
            return true;
    }
    return false;
}

static bool is_ident_start(int c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* -------------------------------------------------------------------------- */

typedef struct {
    char *type;     /* "const char *" */
    char *name;     /* "msg" или сгенерированное "p0" */
    bool  named;    /* имя было во входе */
} Param;

typedef struct {
    char *key;
    char *val;
} Flag;

typedef struct {
    int   line;
    char *rettype;
    char *name;
    char *proto;        /* "pid_t log_write(const char* msg)" */
    char *rpc_mode;     /* RPC_SEND_* */
    Param params[MAX_PARAMS];
    int   nparams;
    bool  is_void_ret;
    Flag  flags[MAX_FLAGS];
    int   nflags;
} Func;

typedef struct {
    char *line; /* целиком "#include <sys/types.h>" */
} Incl;

static Incl g_incls[MAX_INCLS];
static int  g_nincl;
static Func g_funcs[MAX_FUNCS];
static int  g_nfunc;

static const char *flag_get(const Func *f, const char *key)
{
    for (int i = 0; i < f->nflags; i++) {
        if (strcmp(f->flags[i].key, key) == 0)
            return f->flags[i].val;
    }
    return NULL;
}

static bool valid_rpc_mode(const char *s)
{
    return strcmp(s, "RPC_SEND_ALL") == 0
        || strcmp(s, "RPC_SEND_FIRST") == 0
        || strcmp(s, "RPC_SEND_LAST") == 0
        || strcmp(s, "RPC_SEND_RR") == 0;
}

/* Сжать пробелы, '*' без ведущего пробела оставляем как " *". */
static char *join_tokens(char **tok, int n)
{
    size_t cap = 1;
    for (int i = 0; i < n; i++)
        cap += strlen(tok[i]) + 1;
    char *out = malloc(cap);
    if (!out)
        die("out of memory");
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i > 0 && strcmp(tok[i], "*") != 0)
            strcat(out, " ");
        if (strcmp(tok[i], "*") == 0 && i > 0)
            strcat(out, " *");
        else
            strcat(out, tok[i]);
    }
    return out;
}

/* -------------------------------------------------------------------------- */
/* Удаление комментариев, переводы строк сохраняем для номеров строк.       */
/* -------------------------------------------------------------------------- */

static char *strip_comments(const char *src)
{
    size_t n = strlen(src);
    char *out = malloc(n + 1);
    if (!out)
        die("out of memory");
    size_t j = 0;
    enum { NRM, SL, BL, CHR, STR } st = NRM;
    for (size_t i = 0; src[i]; i++) {
        char c = src[i];
        char nxt = src[i + 1];
        switch (st) {
        case NRM:
            if (c == '/' && nxt == '/') { st = SL; out[j++] = ' '; i++; }
            else if (c == '/' && nxt == '*') { st = BL; out[j++] = ' '; i++; }
            else if (c == '\'') { st = CHR; out[j++] = c; }
            else if (c == '"') { st = STR; out[j++] = c; }
            else out[j++] = c;
            break;
        case SL:
            if (c == '\n') { st = NRM; out[j++] = c; }
            else out[j++] = ' ';
            break;
        case BL:
            if (c == '\n') out[j++] = c;
            else if (c == '*' && nxt == '/') { st = NRM; out[j++] = ' '; i++; }
            else out[j++] = ' ';
            break;
        case CHR:
            out[j++] = c;
            if (c == '\\' && nxt) { out[j++] = nxt; i++; }
            else if (c == '\'') st = NRM;
            break;
        case STR:
            out[j++] = c;
            if (c == '\\' && nxt) { out[j++] = nxt; i++; }
            else if (c == '"') st = NRM;
            break;
        }
    }
    out[j] = '\0';
    return out;
}

/* -------------------------------------------------------------------------- */
/* Лексер                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    T_EOF, T_IDENT, T_STAR, T_LPAREN, T_RPAREN, T_COMMA, T_SEMI, T_EQ,
    T_INCLUDE, T_OTHER
} TokKind;

typedef struct {
    TokKind kind;
    char *text;
    int line;
} Tok;

typedef struct {
    const char *s;
    size_t i;
    int line;
    Tok cur;
} Lex;

static void tok_free(Tok *t)
{
    free(t->text);
    t->text = NULL;
}

static void lex_skip_ws(Lex *lx)
{
    while (lx->s[lx->i]) {
        if (lx->s[lx->i] == '\n') {
            lx->line++;
            lx->i++;
        } else if (isspace((unsigned char)lx->s[lx->i])) {
            lx->i++;
        } else {
            break;
        }
    }
}

static void lex_next(Lex *lx)
{
    tok_free(&lx->cur);
    lex_skip_ws(lx);
    lx->cur.line = lx->line;
    lx->cur.text = NULL;
    if (!lx->s[lx->i]) {
        lx->cur.kind = T_EOF;
        return;
    }
    char c = lx->s[lx->i];
    if (c == '#') {
        size_t start = lx->i;
        int line0 = lx->line;
        lx->i++;
        lex_skip_ws(lx);
        if (strncmp(lx->s + lx->i, "include", 7) != 0 || is_ident(lx->s[lx->i + 7])) {
            err_at(line0, "unsupported preprocessor directive");
            while (lx->s[lx->i] && lx->s[lx->i] != '\n')
                lx->i++;
            lx->cur.kind = T_OTHER;
            lx->cur.text = xstrdup("#");
            return;
        }
        lx->i += 7;
        lex_skip_ws(lx);
        if (lx->s[lx->i] != '"' && lx->s[lx->i] != '<') {
            err_at(lx->line, "expected \"file\" or <file> after #include");
            lx->cur.kind = T_OTHER;
            lx->cur.text = xstrdup("#include");
            return;
        }
        char end = (lx->s[lx->i] == '"') ? '"' : '>';
        lx->i++;
        while (lx->s[lx->i] && lx->s[lx->i] != end && lx->s[lx->i] != '\n')
            lx->i++;
        if (lx->s[lx->i] != end)
            err_at(lx->line, "unterminated #include");
        else
            lx->i++;
        lx->cur.kind = T_INCLUDE;
        lx->cur.text = xstrndup(lx->s + start, lx->i - start);
        /* нормализуем пробелы: "#include <foo.h>" */
        return;
    }
    if (is_ident_start((unsigned char)c)) {
        size_t start = lx->i;
        lx->i++;
        while (is_ident((unsigned char)lx->s[lx->i]))
            lx->i++;
        lx->cur.kind = T_IDENT;
        lx->cur.text = xstrndup(lx->s + start, lx->i - start);
        return;
    }
    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)lx->s[lx->i + 1]))) {
        size_t start = lx->i;
        if (c == '-')
            lx->i++;
        while (isdigit((unsigned char)lx->s[lx->i]))
            lx->i++;
        lx->cur.kind = T_IDENT; /* число как значение флага */
        lx->cur.text = xstrndup(lx->s + start, lx->i - start);
        return;
    }
    lx->i++;
    switch (c) {
    case '*': lx->cur.kind = T_STAR; lx->cur.text = xstrdup("*"); break;
    case '(': lx->cur.kind = T_LPAREN; lx->cur.text = xstrdup("("); break;
    case ')': lx->cur.kind = T_RPAREN; lx->cur.text = xstrdup(")"); break;
    case ',': lx->cur.kind = T_COMMA; lx->cur.text = xstrdup(","); break;
    case ';': lx->cur.kind = T_SEMI; lx->cur.text = xstrdup(";"); break;
    case '=': lx->cur.kind = T_EQ; lx->cur.text = xstrdup("="); break;
    case '.':
        if (lx->s[lx->i] == '.' && lx->s[lx->i + 1] == '.') {
            lx->i += 2;
            err_at(lx->line, "variadic functions are not allowed");
            lx->cur.kind = T_OTHER;
            lx->cur.text = xstrdup("...");
            break;
        }
        /* fall through */
    default:
        lx->cur.kind = T_OTHER;
        char buf[2] = { c, 0 };
        lx->cur.text = xstrdup(buf);
        err_at(lx->line, "unexpected character '%c'", c);
        break;
    }
}

static void lex_init(Lex *lx, const char *s)
{
    memset(lx, 0, sizeof(*lx));
    lx->s = s;
    lx->line = 1;
    lex_next(lx);
}

/* -------------------------------------------------------------------------- */
/* Разбор типа / имени                                                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    char *toks[64];
    int n;
    int line;
} TokList;

static void tl_add(TokList *tl, const char *s)
{
    if (tl->n >= 64)
        die("type too complex");
    tl->toks[tl->n++] = xstrdup(s);
}

static void tl_free(TokList *tl)
{
    for (int i = 0; i < tl->n; i++)
        free(tl->toks[i]);
    tl->n = 0;
}

/* Собираем токены типа/имени до stop-токена. '(' ')' ',' ';' */
static void collect_typeish(Lex *lx, TokList *tl)
{
    tl->n = 0;
    tl->line = lx->cur.line;
    while (lx->cur.kind == T_IDENT || lx->cur.kind == T_STAR) {
        if (lx->cur.kind == T_IDENT &&
            (strcmp(lx->cur.text, "struct") == 0
             || strcmp(lx->cur.text, "union") == 0
             || strcmp(lx->cur.text, "enum") == 0)) {
            tl_add(tl, lx->cur.text);
            lex_next(lx);
            if (lx->cur.kind == T_IDENT) {
                tl_add(tl, lx->cur.text);
                lex_next(lx);
            }
            continue;
        }
        tl_add(tl, lx->cur.text);
        lex_next(lx);
    }
}

static void split_type_name(TokList *tl, char **ptype, char **pname, bool *named)
{
    *named = false;
    *pname = NULL;
    if (tl->n == 0) {
        *ptype = xstrdup("");
        return;
    }
    /* имя — последний IDENT, если перед ним есть ещё спецификаторы
     * и сам IDENT не является type-keyword. */
    int name_i = -1;
    if (strcmp(tl->toks[tl->n - 1], "*") != 0) {
        bool kw = is_type_kw(tl->toks[tl->n - 1]);
        if (!kw) {
            int specs = 0;
            for (int i = 0; i < tl->n - 1; i++) {
                if (strcmp(tl->toks[i], "*") != 0)
                    specs++;
            }
            if (specs > 0)
                name_i = tl->n - 1;
        }
    }
    if (name_i >= 0) {
        *named = true;
        *pname = xstrdup(tl->toks[name_i]);
        *ptype = join_tokens(tl->toks, name_i);
    } else {
        *ptype = join_tokens(tl->toks, tl->n);
    }
}

static bool parse_param_list(Lex *lx, Func *fn)
{
    if (lx->cur.kind != T_LPAREN) {
        err_at(lx->cur.line, "expected '(' after function name");
        return false;
    }
    lex_next(lx);
    if (lx->cur.kind == T_RPAREN) {
        lex_next(lx);
        return true;
    }
    /* void как единственный параметр */
    if (lx->cur.kind == T_IDENT && strcmp(lx->cur.text, "void") == 0) {
        /* void ) → нет параметров; void * / void x — параметр */
        char *voidtxt = xstrdup(lx->cur.text);
        int vline = lx->cur.line;
        lex_next(lx);
        if (lx->cur.kind == T_RPAREN) {
            free(voidtxt);
            lex_next(lx);
            return true;
        }
        /* void * или void что-то ещё — это параметр */
        TokList tl = {0};
        tl.line = vline;
        tl_add(&tl, voidtxt);
        free(voidtxt);
        while (lx->cur.kind == T_IDENT || lx->cur.kind == T_STAR) {
            if (lx->cur.kind == T_IDENT &&
                (strcmp(lx->cur.text, "struct") == 0
                 || strcmp(lx->cur.text, "union") == 0
                 || strcmp(lx->cur.text, "enum") == 0)) {
                tl_add(&tl, lx->cur.text);
                lex_next(lx);
                if (lx->cur.kind == T_IDENT) {
                    tl_add(&tl, lx->cur.text);
                    lex_next(lx);
                }
                continue;
            }
            tl_add(&tl, lx->cur.text);
            lex_next(lx);
        }
        if (fn->nparams >= MAX_PARAMS) {
            err_at(tl.line, "too many parameters");
            tl_free(&tl);
            return false;
        }
        Param *p = &fn->params[fn->nparams++];
        split_type_name(&tl, &p->type, &p->name, &p->named);
        tl_free(&tl);
        if (lx->cur.kind == T_COMMA)
            lex_next(lx);
        else if (lx->cur.kind == T_RPAREN) {
            lex_next(lx);
            return true;
        }
    }

    while (lx->cur.kind != T_RPAREN && lx->cur.kind != T_EOF && lx->cur.kind != T_SEMI) {
        if (fn->nparams >= MAX_PARAMS) {
            err_at(lx->cur.line, "too many parameters");
            return false;
        }
        TokList tl = {0};
        collect_typeish(lx, &tl);
        if (tl.n == 0) {
            err_at(lx->cur.line, "expected parameter type");
            return false;
        }
        Param *p = &fn->params[fn->nparams++];
        split_type_name(&tl, &p->type, &p->name, &p->named);
        tl_free(&tl);
        if (lx->cur.kind == T_COMMA) {
            lex_next(lx);
            continue;
        }
        if (lx->cur.kind == T_RPAREN)
            break;
        err_at(lx->cur.line, "expected ',' or ')' in parameter list");
        return false;
    }
    if (lx->cur.kind != T_RPAREN) {
        err_at(lx->cur.line, "expected ')' ");
        return false;
    }
    lex_next(lx);
    return true;
}

static bool parse_flags(Lex *lx, Func *fn)
{
    while (lx->cur.kind != T_SEMI && lx->cur.kind != T_EOF) {
        if (lx->cur.kind != T_IDENT) {
            err_at(lx->cur.line, "expected flag KEY=VALUE");
            return false;
        }
        if (fn->nflags >= MAX_FLAGS) {
            err_at(lx->cur.line, "too many flags");
            return false;
        }
        Flag *fl = &fn->flags[fn->nflags++];
        fl->key = xstrdup(lx->cur.text);
        lex_next(lx);
        if (lx->cur.kind != T_EQ) {
            err_at(lx->cur.line, "expected '=' after flag name '%s'", fl->key);
            return false;
        }
        lex_next(lx);
        if (lx->cur.kind != T_IDENT) {
            err_at(lx->cur.line, "expected value after '%s='", fl->key);
            return false;
        }
        fl->val = xstrdup(lx->cur.text);
        lex_next(lx);
    }
    if (lx->cur.kind != T_SEMI) {
        err_at(lx->cur.line, "expected ';' after declaration");
        return false;
    }
    lex_next(lx);
    return true;
}

static void apply_defaults(Func *fn)
{
    const char *mode = flag_get(fn, "RPC_MODE");
    if (mode) {
        if (!valid_rpc_mode(mode))
            err_at(fn->line, "invalid RPC_MODE '%s' (expected RPC_SEND_ALL/FIRST/LAST/RR)", mode);
        fn->rpc_mode = xstrdup(mode);
    } else {
        fn->rpc_mode = xstrdup("RPC_SEND_ALL");
    }
    /* Неизвестные ключи допускаются — запас под будущие флаги.
     * RPC_MODE проверяется выше. */
    for (int i = 0; i < fn->nparams; i++) {
        if (!fn->params[i].name) {
            char buf[16];
            snprintf(buf, sizeof(buf), "p%d", i);
            fn->params[i].name = xstrdup(buf);
        }
    }
    fn->is_void_ret = (strcmp(fn->rettype, "void") == 0);
}

static char *make_proto(const Func *fn)
{
    size_t cap = strlen(fn->rettype) + strlen(fn->name) + 8;
    for (int i = 0; i < fn->nparams; i++) {
        cap += strlen(fn->params[i].type) + strlen(fn->params[i].name) + 4;
    }
    char *s = malloc(cap);
    if (!s)
        die("out of memory");
    if (fn->nparams == 0)
        snprintf(s, cap, "%s %s(void)", fn->rettype, fn->name);
    else {
        snprintf(s, cap, "%s %s(", fn->rettype, fn->name);
        for (int i = 0; i < fn->nparams; i++) {
            if (i)
                strcat(s, ", ");
            strcat(s, fn->params[i].type);
            strcat(s, " ");
            strcat(s, fn->params[i].name);
        }
        strcat(s, ")");
    }
    return s;
}

static bool parse_decl(Lex *lx)
{
    if (g_nfunc >= MAX_FUNCS) {
        err_at(lx->cur.line, "too many functions");
        return false;
    }
    Func *fn = &g_funcs[g_nfunc];
    memset(fn, 0, sizeof(*fn));
    fn->line = lx->cur.line;

    TokList tl = {0};
    collect_typeish(lx, &tl);
    if (tl.n == 0) {
        err_at(lx->cur.line, "expected function declaration");
        return false;
    }
    char *ptype = NULL, *pname = NULL;
    bool named = false;
    split_type_name(&tl, &ptype, &pname, &named);
    tl_free(&tl);
    if (!named || !pname) {
        err_at(fn->line, "missing function name");
        free(ptype);
        free(pname);
        return false;
    }
    fn->rettype = ptype;
    fn->name = pname;

    for (int i = 0; i < g_nfunc; i++) {
        if (strcmp(g_funcs[i].name, fn->name) == 0) {
            err_at(fn->line, "duplicate function '%s' (first at line %d)", fn->name, g_funcs[i].line);
            return false;
        }
    }

    if (!parse_param_list(lx, fn))
        return false;
    if (!parse_flags(lx, fn))
        return false;

    apply_defaults(fn);
    fn->proto = make_proto(fn);
    g_nfunc++;
    return true;
}

static void parse_file(const char *text)
{
    Lex lx;
    lex_init(&lx, text);
    while (lx.cur.kind != T_EOF) {
        if (lx.cur.kind == T_INCLUDE) {
            if (g_nincl >= MAX_INCLS)
                die("too many #include lines");
            g_incls[g_nincl++].line = xstrdup(lx.cur.text);
            lex_next(&lx);
            continue;
        }
        if (!parse_decl(&lx)) {
            /* до следующего ';' или EOF, чтобы не зациклиться */
            while (lx.cur.kind != T_EOF && lx.cur.kind != T_SEMI)
                lex_next(&lx);
            if (lx.cur.kind == T_SEMI)
                lex_next(&lx);
        }
    }
    tok_free(&lx.cur);
}

/* -------------------------------------------------------------------------- */
/* Вывод                                                                      */
/* -------------------------------------------------------------------------- */

static FILE *open_out(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "w");
    if (!fp)
        die("cannot write %s: %s", path, strerror(errno));
    fprintf(fp, "/* Generated by srpc_rpcgen from %s — do not edit. */\n", g_infile);
    return fp;
}

static void emit_includes(const char *dir)
{
    FILE *fp = open_out(dir, "libsrpc_rpc_includes.inl");
    for (int i = 0; i < g_nincl; i++)
        fprintf(fp, "%s\n", g_incls[i].line);
    fclose(fp);
}

static void emit_funid(const char *dir)
{
    FILE *fp = open_out(dir, "libsrpc_rpc_funid.inl");
    for (int i = 0; i < g_nfunc; i++)
        fprintf(fp, "    libsrpc_funid_%s,\n", g_funcs[i].name);
    fclose(fp);
}

static void emit_decls(const char *dir)
{
    FILE *fp = open_out(dir, "libsrpc_rpc_decls.inl");
    for (int i = 0; i < g_nfunc; i++)
        fprintf(fp, "%s;\n", g_funcs[i].proto);
    fclose(fp);
}

static void emit_ids(const char *dir)
{
    FILE *fp = open_out(dir, "libsrpc_rpc_ids.inl");
    for (int i = 0; i < g_nfunc; i++)
        fprintf(fp, "    sRPCFNID_%s,\n", g_funcs[i].name);
    fclose(fp);
}

static void emit_param_list(FILE *fp, const Func *fn)
{
    if (fn->nparams == 0) {
        fputs("void", fp);
        return;
    }
    for (int i = 0; i < fn->nparams; i++) {
        if (i)
            fputs(", ", fp);
        fprintf(fp, "%s %s", fn->params[i].type, fn->params[i].name);
    }
}

static void emit_arg_names(FILE *fp, const Func *fn)
{
    for (int i = 0; i < fn->nparams; i++) {
        if (i)
            fputs(", ", fp);
        fputs(fn->params[i].name, fp);
    }
}

static void emit_wrappers(const char *dir)
{
    FILE *fp = open_out(dir, "libsrpc_rpc_wrappers.inl");
    for (int i = 0; i < g_nfunc; i++) {
        const Func *fn = &g_funcs[i];
        fprintf(fp, "static %s librpcimp_%s(", fn->rettype, fn->name);
        emit_param_list(fp, fn);
        fputs(")\n{\n", fp);
        fputs("    int len = 0; int rlen = 0; int pos = 0;\n", fp);
        fputs("    libsrpc_request_t *req = NULL;\n", fp);
        fputs("    __libsrpc_errno_clear();\n", fp);
        if (fn->is_void_ret)
            fputs("    rlen = 0;\n", fp);
        else
            fprintf(fp, "    rlen = (int)sizeof(%s);\n", fn->rettype);
        fputs("    char rbuf[rlen];\n", fp);
        fputs("    memset(rbuf, 0, (size_t)rlen);\n", fp);
        if (fn->nparams == 0)
            fputs("    len = 0;\n", fp);
        else {
            fputs("    len = ", fp);
            for (int p = 0; p < fn->nparams; p++)
                fprintf(fp, "(int)sizeof(%s) + ", fn->params[p].name);
            fputs("0;\n", fp);
        }
        fprintf(fp, "    req = libsrpc_req_alloc(sRPCFNID_%s, len, rlen);\n", fn->name);
        fputs("    if (req == NULL) { __libsrpc_errno_set(ENOMEM); } else {\n", fp);
        for (int p = 0; p < fn->nparams; p++) {
            fprintf(fp,
                    "        memcpy(&req->buf[pos], &%s, sizeof(%s)); pos += (int)sizeof(%s);\n",
                    fn->params[p].name, fn->params[p].name, fn->params[p].name);
        }
        fputs("        if (!!req && pos != len) { __libsrpc_errno_set(EBADMSG); } else {\n", fp);
        fputs("            libsrpc_response_t *resp = libsrpc_req_get_response(req, 0);\n", fp);
        fprintf(fp, "            libsrpc_send_request(req, %s);\n", fn->rpc_mode);
        fputs("            libsrpc_req_response_get(resp, rbuf, (size_t)rlen);\n", fp);
        fputs("        }\n", fp);
        fputs("    }\n", fp);
        fprintf(fp, "    DBG_PRINT(\"RPC call: %s %s( fnid=%%d) len=%%d retsz=%%d\\n\", sRPCFNID_%s, len, rlen);\n",
                fn->rettype, fn->name, fn->name);
        if (fn->is_void_ret)
            fputs("    return;\n", fp);
        else
            fprintf(fp, "    return (*(%s *)(rbuf));\n", fn->rettype);
        fputs("}\n\n", fp);
    }
    fclose(fp);
}

static void emit_weak(const char *dir)
{
    FILE *fp = open_out(dir, "libsrpc_rpc_weak.inl");
    for (int i = 0; i < g_nfunc; i++) {
        fprintf(fp, "__attribute__((weak, alias(\"librpcimp_%s\")))\n", g_funcs[i].name);
        fprintf(fp, "%s;\n\n", g_funcs[i].proto);
    }
    fclose(fp);
}

static void emit_table(const char *dir)
{
    FILE *fp = open_out(dir, "libsrpc_rpc_table.inl");
    for (int i = 0; i < g_nfunc; i++) {
        fprintf(fp, "    { .rpc = librpcimp_%s, .loc = %s, .name = \"%s\" },\n",
                g_funcs[i].name, g_funcs[i].name, g_funcs[i].name);
    }
    fclose(fp);
}

static void emit_callbacks(const char *dir)
{
    FILE *fp = open_out(dir, "libsrpc_rpc_callbacks.inl");
    for (int i = 0; i < g_nfunc; i++) {
        const Func *fn = &g_funcs[i];
        fprintf(fp, "        case sRPCFNID_%s:\n", fn->name);
        fprintf(fp, "            if ((void *)&(%s) == (void *)&(librpcimp_%s)) { rc = -ERECURSIVE; break; }\n",
                fn->name, fn->name);
        fputs("            {\n", fp);
        fputs("                int pos = 0;\n", fp);
        for (int p = 0; p < fn->nparams; p++)
            fprintf(fp, "                %s %s;\n", fn->params[p].type, fn->params[p].name);
        for (int p = 0; p < fn->nparams; p++) {
            fprintf(fp,
                    "                memcpy(&%s, &req->buf[pos], sizeof(%s)); pos += (int)sizeof(%s);\n",
                    fn->params[p].name, fn->params[p].name, fn->params[p].name);
        }
        if (fn->is_void_ret) {
            fputs("                ", fp);
            fputs(fn->name, fp);
            fputs("(", fp);
            emit_arg_names(fp, fn);
            fputs(");\n", fp);
        } else {
            fprintf(fp, "                %s retval = %s(", fn->rettype, fn->name);
            emit_arg_names(fp, fn);
            fputs(");\n", fp);
            fputs("                rc = libsrpc_req_response_set(req, resp, ctrl, &retval, sizeof(retval));\n", fp);
        }
        fputs("                (void)pos;\n", fp);
        fputs("            }\n", fp);
        fputs("            break;\n", fp);
    }
    fclose(fp);
}

static void emit_all(const char *dir)
{
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        die("cannot create %s: %s", dir, strerror(errno));
    emit_includes(dir);
    emit_funid(dir);
    emit_decls(dir);
    emit_ids(dir);
    emit_wrappers(dir);
    emit_weak(dir);
    emit_table(dir);
    emit_callbacks(dir);
}

/* -------------------------------------------------------------------------- */

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        die("cannot open %s: %s", path, strerror(errno));
    if (fseek(fp, 0, SEEK_END) != 0)
        die("cannot seek %s", path);
    long n = ftell(fp);
    if (n < 0)
        die("cannot stat %s", path);
    rewind(fp);
    char *buf = malloc((size_t)n + 1);
    if (!buf)
        die("out of memory");
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <libsrpc_rpc_functions.txt> <outdir>\n", argv[0]);
        return 2;
    }
    g_infile = argv[1];
    const char *outdir = argv[2];

    char *raw = read_file(g_infile);
    char *text = strip_comments(raw);
    free(raw);
    parse_file(text);
    free(text);

    if (g_errcnt)
        die("%d error(s)", g_errcnt);
    if (g_nfunc == 0)
        die("%s: no RPC functions declared", g_infile);

    emit_all(outdir);
    return 0;
}
