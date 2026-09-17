# Nokia_C1_01 Display Component
idf_component_register(
    SRCS "Nokia_C1_01_HardwareSPI.cpp" "display_manager.cpp"
    INCLUDE_DIRS "." "include"
    REQUIRES driver esp_hw_support
)
