#ifndef FILE_LIBSRPC_RPC_FUNCTIONS_H
#define FILE_LIBSRPC_RPC_FUNCTIONS_H


/* Список объявлений прототипов RPC функций с параметрами, которые будут использоваться для удаленного вызова.
 * XF(флаг отправки, возвращаемый тип, имя функции, параметры)
 * Функции с переменным числом параметров - не допустимы.
 */
#define RPC_LIST    \
    XF(RPC_SEND_ALL, int,testlocal) \
    XF(RPC_SEND_ALL, pid_t,log_write,const char*) \
    XF(RPC_SEND_ALL, int,calc_add,int,int) \
    XF(RPC_SEND_ALL, void,all_exit) \

#endif // FILE_LIBSRPC_RPC_FUNCTIONS_H
