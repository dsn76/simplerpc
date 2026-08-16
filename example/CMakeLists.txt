
macro(add_example name sources_str flags libs)
    string(REPLACE "|" ";" sources ${sources_str})

    set(full_sources "")
    foreach(src ${sources})
        list(APPEND full_sources ${src})
    endforeach()

    add_executable(${name} ${full_sources})
    target_compile_options(${name} PRIVATE ${flags})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(${name} srpc ${libs})
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endmacro()

# Приложения
add_example(app    "app.c"         "-Wall"       "srpc")
add_example(log    "log.c"         "-Wall"       "srpc")

