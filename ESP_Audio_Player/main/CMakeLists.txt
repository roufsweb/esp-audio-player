idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES console nvs_flash bt storage audio_player network web_server)

