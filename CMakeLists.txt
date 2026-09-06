cmake_minimum_required(VERSION 3.10)

project(srpc
    VERSION 0.2.1
    LANGUAGES C
)
message(STATUS "================================")
message(STATUS "Configuring ${PROJECT_NAME} version ${PROJECT_VERSION}")
message(STATUS "================================")

# ============================================================
# Глобальные опции проекта
# ============================================================
set(BUILD_TYPE "release" CACHE STRING "Build type: debug / release")
#set(BUILD_TYPE "debug" CACHE STRING "Build type: debug / release")
#set(ENABLE_SANITIZER  ON CACHE BOOL   "Enable ASan/UBSan")
set(ENABLE_SANITIZER  OFF CACHE BOOL   "Enable ASan/UBSan")

# Отключение подмены стандартного аллокатора на shared memory аллокатор.
# Если включено, библиотека не перехватывает malloc/calloc/realloc/free.
option(DISABLE_ALLOC "Disable substitution of standard allocator with shared memory allocator" ON)

# Уровень детализации отладочных сообщений: 0 — отключены, 1 — ERR, 2 — ERR+WRN, 3 — ERR+WRN+INF.
set(DBG_LVL 2 CACHE STRING "Debug print level (0=off, 1=err, 2=+warn, 3=+info)")

# Степень двойки для размера MPMC очереди: 2^10 = 1024.
set(MPMCQ_DEGREE 10 CACHE STRING "MPMC queue capacity as power of two (10 => 2^10 = 1024)")

# Параметры shared memory
set(SHMEM_SIZE_KB   4096             CACHE STRING "Shared memory size in KB")
set(SHMEM_BASE_VADR "0x200000000000" CACHE STRING "Base virtual address for shared memory")

# ============================================================
# Настройки компилятора
# ============================================================
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Timestamp для BUILD_TS
string(TIMESTAMP BUILD_TS "%Y%m%d%H%M%S")

# === Пути ===
set(TLSF_DIR ${CMAKE_SOURCE_DIR}/libs/tlsf_txn)
set(LIBS_DIR ${CMAKE_SOURCE_DIR}/libs)

# === Режими сборки ===
string(TOLOWER "${BUILD_TYPE}" _bt)
if(_bt STREQUAL "debug")
    message(STATUS "Build type: DEBUG")
    set(PROJECT_C_FLAGS   -O0 -ggdb3 -DDEBUG)
    if(ENABLE_SANITIZER)
        list(APPEND PROJECT_C_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
elseif(_bt STREQUAL "release")
    message(STATUS "Build type: RELEASE")
    set(PROJECT_C_FLAGS   -O3 )
else()
    message(FATAL_ERROR "Unknown BUILD_TYPE '${BUILD_TYPE}'. Use: debug / release")
endif()

# === Параметры памяти → PROJECT_C_FLAGS ===
math(EXPR SHMEM_SIZE_BYTES "${SHMEM_SIZE_KB} * 1024")
list(APPEND PROJECT_C_FLAGS
    -DSHMEM_BASE_VADR=${SHMEM_BASE_VADR}ULL
    -DSHMEM_SIZE=${SHMEM_SIZE_BYTES}ULL
)

# === Опции проекта → PROJECT_C_FLAGS ===
if(DISABLE_ALLOC)
    list(APPEND PROJECT_C_FLAGS -DLIBSRPC_DISABLE_SUBSTITUTION_ALLOCATOR)
endif()
list(APPEND PROJECT_C_FLAGS -DDBG_LVL=${DBG_LVL})
math(EXPR MPMCQ_CAPACITY "1 << ${MPMCQ_DEGREE}")
list(APPEND PROJECT_C_FLAGS -DMPMC_QUEUE_CAPACITY=${MPMCQ_CAPACITY})

# ============================================================
# libtlsf.a — статическая библиотека из внешних исходников
# ============================================================
add_library(tlsf STATIC
    ${TLSF_DIR}/tlsf_txn.c
)
target_compile_options(tlsf PRIVATE -fPIC -Wall -Wextra -Werror -Wformat ${PROJECT_C_FLAGS})
target_include_directories(tlsf PUBLIC ${TLSF_DIR})

# ============================================================
# embedded_loader.c — генерация через xxd
# ============================================================
set(LOADER_BIN ${CMAKE_CURRENT_BINARY_DIR}/loader)
set(EMBEDDED_LOADER ${CMAKE_CURRENT_BINARY_DIR}/embedded_loader.c)

add_custom_command(
    OUTPUT ${EMBEDDED_LOADER}
    COMMAND gcc -Wall -Wextra -Werror -O3 ${CMAKE_CURRENT_SOURCE_DIR}/src/libsrpc_loader.c -s -o ${LOADER_BIN}
    COMMAND sh -c "xxd -i -n loader ${LOADER_BIN} > ${EMBEDDED_LOADER}"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/libsrpc_loader.c
    COMMENT "Generating embedded_loader.c"
    VERBATIM
)

# ============================================================
# libsrpc.so — динамическая библиотека
# ============================================================
set(SRPC_SOURCES
    src/libsrpc.c
    src/libsrpc_daemon.c
    src/libsrpc_unix_socket.c
    src/libsrpc_fixblockalloc.c
    src/libsrpc_shmem.c
    src/libsrpc_shm_gc.c
    src/libsrpc_errno.c
    src/lf_mpmc_queue.c
    src/libsrpc_pthread.c
    src/libsrpc_list_spin.c
    src/libsrpc_proc.c
    ${EMBEDDED_LOADER}
)

add_library(srpc SHARED ${SRPC_SOURCES})
target_compile_definitions(srpc PRIVATE BUILD_TS="${BUILD_TS}")
target_compile_options(srpc PRIVATE -Wall -Wextra -Werror -fPIC ${PROJECT_C_FLAGS})
target_include_directories(srpc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBS_DIR}
    ${TLSF_DIR}
)
target_link_libraries(srpc tlsf dl)

# ============================================================
# Подключаем подкаталог с примерами
# ============================================================
add_subdirectory(example)

# ============================================================
# Замеры производительности (bench/run_bench.sh)
# ============================================================
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt)
    add_subdirectory(bench)
endif()

