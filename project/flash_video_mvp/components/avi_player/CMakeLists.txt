idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           AVI_PLAYER_VER_MAJOR=2 AVI_PLAYER_VER_MINOR=0 AVI_PLAYER_VER_PATCH=0)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-implicit-fallthrough")
