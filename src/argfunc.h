#ifndef ARGFUNC_H
#define ARGFUNC_H

/* -------------------------------------------------------------------------
 * Макросы для каждого конкретного числа аргументов
 * ------------------------------------------------------------------------- */
#define ARGFUNC_0() void
//#define ARGFUNC_0() 
#define ARGFUNC_1(t0) t0 p0
#define ARGFUNC_2(t0, t1) t0 p0, t1 p1
#define ARGFUNC_3(t0, t1, t2) t0 p0, t1 p1, t2 p2
#define ARGFUNC_4(t0, t1, t2, t3) t0 p0, t1 p1, t2 p2, t3 p3
#define ARGFUNC_5(t0, t1, t2, t3, t4) t0 p0, t1 p1, t2 p2, t3 p3, t4 p4
#define ARGFUNC_6(t0, t1, t2, t3, t4, t5) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5
#define ARGFUNC_7(t0, t1, t2, t3, t4, t5, t6) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6
#define ARGFUNC_8(t0, t1, t2, t3, t4, t5, t6, t7) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7
#define ARGFUNC_9(t0, t1, t2, t3, t4, t5, t6, t7, t8) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8
#define ARGFUNC_10(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9
#define ARGFUNC_11(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10
#define ARGFUNC_12(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11
#define ARGFUNC_13(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12
#define ARGFUNC_14(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13
#define ARGFUNC_15(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14
#define ARGFUNC_16(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14, t15 p15
#define ARGFUNC_17(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14, t15 p15, t16 p16
#define ARGFUNC_18(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14, t15 p15, t16 p16, t17 p17
#define ARGFUNC_19(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14, t15 p15, t16 p16, t17 p17, t18 p18
#define ARGFUNC_20(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19) \
    t0 p0, t1 p1, t2 p2, t3 p3, t4 p4, t5 p5, t6 p6, t7 p7, t8 p8, t9 p9, t10 p10, t11 p11, t12 p12, t13 p13, t14 p14, t15 p15, t16 p16, t17 p17, t18 p18, t19 p19

/* -------------------------------------------------------------------------
 * Выбор нужного макроса на основе количества аргументов
 * Метод: GET_MACRO возвращает N-й макрос из списка, где N = число аргументов.
 * Первый аргумент _0 игнорируется (нужен для сдвига).
 * ------------------------------------------------------------------------- */
#define GET_MACRO(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, \
                  _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, NAME, ...) NAME

#define ARGFUNC(...) \
    GET_MACRO(_0, ##__VA_ARGS__, \
        ARGFUNC_20, ARGFUNC_19, ARGFUNC_18, ARGFUNC_17, ARGFUNC_16, \
        ARGFUNC_15, ARGFUNC_14, ARGFUNC_13, ARGFUNC_12, ARGFUNC_11, \
        ARGFUNC_10, ARGFUNC_9,  ARGFUNC_8,  ARGFUNC_7,  ARGFUNC_6,  \
        ARGFUNC_5,  ARGFUNC_4,  ARGFUNC_3,  ARGFUNC_2,  ARGFUNC_1,  \
        ARGFUNC_0 \
    )(__VA_ARGS__)



#include <string.h>

/* ==================== 1. ИНФРАСТРУКТУРА ПРЕПРОЦЕССОРА ==================== */
/* Двойная косвенность: принудительно раскрывает аргументы ДО склейки ## */
#define PP_CAT(a, b)           PP_CAT_IND(a, b)
#define PP_CAT_IND(a, b)       a ## b

#define PP_NARG_(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, NAME, ...) NAME
#define PP_NARG(...) PP_NARG_(_0, ##__VA_ARGS__, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 )

/* Диспетчер с принудительным раскрытием счётчика перед ## */
#define _SERIALIZE_DISPATCH(n) _SERIALIZE_DISPATCH_IMPL(n)
#define _SERIALIZE_DISPATCH_IMPL(n) SERIALIZE_##n
#define SERIALIZE(...)         _SERIALIZE_DISPATCH(PP_NARG(__VA_ARGS__))(__VA_ARGS__)

/* ==================== 3. SERIALIZE (тело функции) ==================== */
/* 🟢 ЕДИНАЯ ТОЧКА ИЗМЕНЕНИЯ ЛОГИКИ СЕРИАЛИЗАЦИИ */
#define SERIALIZE_OP(idx, type) \
    buf[len++] = idx; \
    memcpy(&buf[len], &p##idx, sizeof(p##idx)); \
    len += sizeof(p##idx);

#define SERIALIZE_0() /* Пустое раскрытие для 0 аргументов */
#define SERIALIZE_1(t1) SERIALIZE_OP(0, t1)
#define SERIALIZE_2(t1,t2) SERIALIZE_OP(0, t1) SERIALIZE_OP(1, t2)
#define SERIALIZE_3(t1,t2,t3) SERIALIZE_OP(0, t1) SERIALIZE_OP(1, t2) SERIALIZE_OP(2, t3)
#define SERIALIZE_4(t1,t2,t3,t4) SERIALIZE_OP(0, t1) SERIALIZE_OP(1, t2) SERIALIZE_OP(2, t3) SERIALIZE_OP(3, t4)
/* Добавьте SERIALIZE_5..SERIALIZE_16 по аналогии */


#endif /* ARGFUNC_H */
