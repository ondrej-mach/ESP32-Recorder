idf_component_register(SRCS "main.c" "recplaymgr.c" "display.c"
                    INCLUDE_DIRS "")


target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
