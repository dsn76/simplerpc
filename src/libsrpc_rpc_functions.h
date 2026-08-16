#ifndef FILE_LIBSRPC_RPC_FUNCTIONS_H
#define FILE_LIBSRPC_RPC_FUNCTIONS_H


/* Список RPC функций с параметрами
 * XF(возвращаемый тип, имя функции, параметры)
 * Функции с переменным числом параметров - не допустимы.
 */
#define RPC_LIST    \
    XF(int,testrpc0)  \
    XF(void,testrpc1,int,char*,long) \
    XF(int,testlocal) \
    XF(void,log_write,const char*) \


#endif // FILE_LIBSRPC_RPC_FUNCTIONS_H