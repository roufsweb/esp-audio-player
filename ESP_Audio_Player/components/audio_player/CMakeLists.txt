idf_component_register(SRCS "audio_player.c"
                       INCLUDE_DIRS "include"
                       REQUIRES console storage esp_common heap freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-unused-variable)
