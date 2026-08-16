cmake_minimum_required(VERSION 3.10)
project(srpc C)

# Глобальные опции проекта
set(BUILD_TYPE "release" CACHE STRING "Build type: debug / release")
#set(BUILD_TYPE "debug" CACHE STRING "Build type: debug / release")
#set(ENABLE_SANITIZER  ON CACHE BOOL   "Enable ASan/UBSan")
set(ENABLE_SANITIZER  OFF CACHE BOOL   "Enable ASan/UBSan")

# === Настройки компилятора ===
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Timestamp для BUILD_TS
string(TIMESTAMP BUILD_TS "%Y%m%d%H%M%S")

# === Пути ===
set(TLSF_DIR ${CMAKE_SOURCE_DIR}/libs/tlsf/src)

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

# ============================================================
# libtlsf.a — статическая библиотека из внешних исходников
# ============================================================
add_library(tlsf STATIC
    ${TLSF_DIR}/tlsf.c
    ${TLSF_DIR}/bul.c
)
target_compile_definitions(tlsf PRIVATE TLSF_BUL=1)
target_compile_options(tlsf PRIVATE -fPIC -Wall ${PROJECT_C_FLAGS})
target_include_directories(tlsf PUBLIC ${TLSF_DIR})

# ============================================================
# embedded_loader.c — генерация через xxd
# ============================================================
set(LOADER_BIN ${CMAKE_CURRENT_BINARY_DIR}/loader)
set(EMBEDDED_LOADER ${CMAKE_CURRENT_BINARY_DIR}/embedded_loader.c)

add_custom_command(
    OUTPUT ${EMBEDDED_LOADER}
    COMMAND gcc -Wall -O3 ${CMAKE_CURRENT_SOURCE_DIR}/src/loader.c -static -s -o ${LOADER_BIN}
    COMMAND sh -c "xxd -i -n loader ${LOADER_BIN} > ${EMBEDDED_LOADER}"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/loader.c
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
    src/libsrpc_errno.c
    src/libsrpc_mpmcq.c
    src/libsrpc_sync.c
    src/libsrpc_pthread.c
    src/libsrpc_dlist.c
    src/libsrpc_list.c
    src/libsrpc_futex.c
    ${EMBEDDED_LOADER}
)

add_library(srpc SHARED ${SRPC_SOURCES})
target_compile_definitions(srpc PRIVATE BUILD_TS="${BUILD_TS}")
target_compile_options(srpc PRIVATE -Wall -fPIC ${PROJECT_C_FLAGS})
target_include_directories(srpc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TLSF_DIR}
)
target_link_libraries(srpc tlsf dl)

# ============================================================
# Подключаем подкаталог с примерами
# ============================================================
add_subdirectory(example)

