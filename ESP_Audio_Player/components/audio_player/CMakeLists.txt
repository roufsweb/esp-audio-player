idf_component_register(SRCS "audio_player.c"
                       INCLUDE_DIRS "include"
                       REQUIRES console storage esp_common heap freertos esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fomit-frame-pointer -Wno-unused-function -Wno-unused-variable)
