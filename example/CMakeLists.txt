
macro(add_example name sources_str flags libs)
    string(REPLACE "|" ";" sources ${sources_str})

    set(full_sources "")
    foreach(src ${sources})
        list(APPEND full_sources ${src})
    endforeach()

    add_executable(${name} ${full_sources})
    target_compile_options(${name} PRIVATE ${flags})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${RPCGEN_DIR}
    )
    target_link_options(${name} PRIVATE -Wl,--no-as-needed)
    target_link_libraries(${name} srpc ${libs})
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endmacro()

# Приложения
add_example(app    "app.c"         "-Wall"       "srpc")
add_example(log    "log.c"         "-Wall"       "srpc")
add_example(clc    "clc.c"         "-Wall"       "srpc")
add_example(all_exit    "all_exit.c"    "-Wall"       "srpc")
