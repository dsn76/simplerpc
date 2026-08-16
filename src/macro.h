#ifndef FILE_MACRO_H
#define FILE_MACRO_H

#define CAT(a, ...) PRIMITIVE_CAT(a, __VA_ARGS__)
#define PRIMITIVE_CAT(a, ...) a ## __VA_ARGS__

#define COMPL(b) PRIMITIVE_CAT(COMPL_, b)
#define COMPL_0 1
#define COMPL_1 0

#define BITAND(x) PRIMITIVE_CAT(BITAND_, x)
#define BITAND_0(y) 0
#define BITAND_1(y) y



#define CHECK_N(x, n, ...) n
#define CHECK(...) CHECK_N(__VA_ARGS__, 0,)
#define PROBE(x) x, 1,

#define IS_PAREN(x) CHECK(IS_PAREN_PROBE x)
#define IS_PAREN_PROBE(...) PROBE(~)

#define NOT(x) CHECK(PRIMITIVE_CAT(NOT_, x))
#define NOT_0 PROBE(~)

#define COMPL(b) PRIMITIVE_CAT(COMPL_, b)
#define COMPL_0 1
#define COMPL_1 0

#define BOOL(x) COMPL(NOT(x))

#define IIF(c) PRIMITIVE_CAT(IIF_, c)
#define IIF_0(t, ...) __VA_ARGS__
#define IIF_1(t, ...) t
#define IF(c) IIF(BOOL(c))

#define EAT(...)
#define EXPAND(...) __VA_ARGS__
#define WHEN(c) IF(c)(EXPAND, EAT)

#define EMPTY()
#define DEFER(id) id EMPTY()
#define OBSTRUCT(id) id DEFER(EMPTY)()

#define COMMA() ,
#define COMMA_IF(n) IF(n)(COMMA, EAT)()

#define PLUS() +
#define PLUS_IF(n) IF(n)(PLUS, EAT)()

#define EVAL(...)  EVAL1(EVAL1(EVAL1(__VA_ARGS__)))
#define EVAL1(...) EVAL2(EVAL2(EVAL2(__VA_ARGS__)))
#define EVAL2(...) EVAL3(EVAL3(EVAL3(__VA_ARGS__)))
#define EVAL3(...) EVAL4(EVAL4(EVAL4(__VA_ARGS__)))
#define EVAL4(...) EVAL5(EVAL5(EVAL5(__VA_ARGS__)))
#define EVAL5(...) __VA_ARGS__

// -----------------
//#define CHECKX_N(x, n, ...) x##n
//#define CHECKX(...) CHECKX_N(__VA_ARGS__, 0,)
// -----------------
//#define CHECKX_IMPL(dummy, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
//#define CHECKX_CALL_IMPL(...) CHECKX_IMPL(dummy, ##__VA_ARGS__, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0)
//#define CHECKX(...) CHECKX_CALL_IMPL(__VA_ARGS__)
// std C11 way
#define CHECKX(...) CHECKX_IMPL(dummy __VA_OPT__(,) 1, 0)
#define CHECKX_IMPL(dummy, _1, ...) _1


#define FOREACH(macro, arg, ...) \
    WHEN(CHECKX(arg##__VA_ARGS__)) \
    ( \
        OBSTRUCT(macro)(arg) \
        OBSTRUCT(FOREACH_INDIRECT)() (macro, __VA_ARGS__) \
    )
#define FOREACH_INDIRECT() FOREACH


#define GETARG1(n, ...) n
#define GETARGV(n, ...) __VA_ARGS__

#define FOREACH2(macro,symb,symbs, ...) \
    WHEN(CHECKX(__VA_ARGS__)) \
    ( \
        OBSTRUCT(macro)(GETARG1(__VA_ARGS__), symb##symbs, CHECKX(GETARGV(__VA_ARGS__)) ) \
        OBSTRUCT(FOREACH2_INDIRECT)() (macro,symb,symb##symbs, GETARGV(__VA_ARGS__)) \
    )
#define FOREACH2_INDIRECT() FOREACH2


#define FOREACH1(macro, ...) \
    WHEN(CHECKX(__VA_ARGS__)) \
    ( \
        OBSTRUCT(macro)(GETARG1(__VA_ARGS__)) \
        OBSTRUCT(FOREACH1_INDIRECT)() (macro, GETARGV(__VA_ARGS__)) \
    )
#define FOREACH1_INDIRECT() FOREACH1


#endif // FILE_MACRO_H