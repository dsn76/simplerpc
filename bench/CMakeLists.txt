
# Замеры производительности. Собираются отдельно от example/,
# запуск — через bench/run_bench.sh
#
# --no-as-needed обязателен: процесс, который только экспортирует RPC-функцию,
# может не иметь ни одной ссылки на символы libsrpc.so, и линкер уберёт
# DT_NEEDED, из-за чего конструктор библиотеки никогда не выполнится.

macro(add_bench name source use_srpc)
    add_executable(${name} ${source})
    target_compile_options(${name} PRIVATE -Wall -O2)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${RPCGEN_DIR}
    )
    if(${use_srpc})
        target_link_options(${name} PRIVATE -Wl,--no-as-needed)
        target_link_libraries(${name} srpc)
    endif()
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )
endmacro()

add_bench(bench_srpc_server  bench_srpc_server.c   TRUE)
add_bench(bench_srpc_client  bench_srpc_client.c   TRUE)
add_bench(bench_ipc_baseline bench_ipc_baseline.c  FALSE)

target_link_libraries(bench_srpc_client pthread)
