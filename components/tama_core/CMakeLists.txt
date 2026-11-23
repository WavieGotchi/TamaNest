idf_component_register(
    SRCS
        "tc_all.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_system
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC TC_HOST)
target_compile_definitions(${COMPONENT_LIB} PRIVATE RTC_MS)   # guest clock seconds from real milliseconds (periph_rtc_ms), not CPU cycles
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fjump-tables -ftree-switch-conversion -fno-crossjumping -fno-gcse -ffast-math -Wno-attributes -Wno-unused-function)
