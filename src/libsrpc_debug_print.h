#ifndef FILE_LIBSRPC_DEBUG_PRINT_H
#define FILE_LIBSRPC_DEBUG_PRINT_H

#include <stdio.h>
#include <unistd.h>

int sched_getcpu(void);

#define DBG_PRN(fmt, ...)   do{ int sz=256; char buf[sz]; int len = snprintf(buf, sz, fmt, ##__VA_ARGS__); write(STDERR_FILENO, buf, len); }while(0)

#ifdef DEBUG
#define DBG_PRINT(fmt, ...) fprintf(stderr, "DBG[%d][%d]%s(): " fmt, getpid(), sched_getcpu(), __func__, ##__VA_ARGS__)
#else
#define DBG_PRINT(fmt, ...) {}
#endif

#ifndef DBG_LVL
#define DBG_LVL 2
#endif // DBG_LVL

#ifdef DBG_LVL
#if DBG_LVL >= 1
#define ERR_PRINT(fmt, ...) fprintf(stderr, "ERR[%d][%d]%s(): " fmt, getpid(), sched_getcpu(), __func__, ##__VA_ARGS__)
#else
#define ERR_PRINT(fmt, ...) {}
#endif // DBG_LVL >= 1
#if DBG_LVL >= 2
#define WRN_PRINT(fmt, ...) fprintf(stderr, "WRN[%d][%d]%s(): " fmt, getpid(), sched_getcpu(), __func__, ##__VA_ARGS__)
#else
#define WRN_PRINT(fmt, ...) {}
#endif // DBG_LVL >= 2
#if DBG_LVL >= 3
#define INF_PRINT(fmt, ...) fprintf(stderr, "INF[%d][%d]%s(): " fmt, getpid(), sched_getcpu(), __func__, ##__VA_ARGS__)
#else
#define INF_PRINT(fmt, ...) {}
#endif // DBG_LVL >= 3
#else
#define ERR_PRINT(fmt, ...) {}
#define WRN_PRINT(fmt, ...) {}
#define INF_PRINT(fmt, ...) {}
#endif // DBG_LVL


#endif // FILE_LIBSRPC_DEBUG_PRINT_H
