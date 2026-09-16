cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP_Audio_Player)

# --- OVERRIDE BLUEDROID SBC CODEC ---
# Remove the original bta_av_co.c from the 'bt' component and inject our SBC XQ patched version
get_target_property(bt_sources __idf_bt SOURCES)
if(bt_sources)
    list(FILTER bt_sources EXCLUDE REGEX "bta_av_co\\.c$")
    set_target_properties(__idf_bt PROPERTIES SOURCES "${bt_sources}")
    target_sources(__idf_bt PRIVATE "${CMAKE_CURRENT_LIST_DIR}/components/bt_override/bta_av_co.c")
endif()


