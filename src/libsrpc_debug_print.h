#ifndef FILE_LIBSRPC_DEBUG_PRINT_H
#define FILE_LIBSRPC_DEBUG_PRINT_H

#include <stdio.h>
#include <unistd.h>

//#define DBG_PRN(fmt, ...)   do{ int sz=4096; char buf[sz]; int len = snprintf(buf, sz, fmt, ##__VA_ARGS__); write(STDERR_FILENO, buf, len); }while(0)
#define DBG_PRN(fmt, ...)   do{ int sz=256; char buf[sz]; int len = snprintf(buf, sz, fmt, ##__VA_ARGS__); write(STDERR_FILENO, buf, len); }while(0)

#ifdef DEBUG
#define DBG_PRINT(fmt, ...) fprintf(stderr, "DBG[%d]%s(): " fmt, getpid(), __func__, ##__VA_ARGS__)
#else
#define DBG_PRINT(fmt, ...)
#endif
#define ERR_PRINT(fmt, ...) fprintf(stderr, "ERR[%d]%s(): " fmt, getpid(), __func__, ##__VA_ARGS__)
#define WRN_PRINT(fmt, ...) fprintf(stderr, "WRN[%d]%s(): " fmt, getpid(), __func__, ##__VA_ARGS__)

#endif // FILE_LIBSRPC_DEBUG_PRINT_H
