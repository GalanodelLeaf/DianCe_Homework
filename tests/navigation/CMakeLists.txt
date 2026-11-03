cmake_minimum_required(VERSION 3.16)
project(navigation_tests C CXX)
set(CMAKE_CXX_STANDARD 20)
get_filename_component(PROJECT_ROOT "../.." ABSOLUTE)
set(LVGL_ROOT "${PROJECT_ROOT}/managed_components/lvgl__lvgl")

# Generate include shims in the test build directory; production headers are untouched.
foreach(header esp_err.h esp_log.h esp_heap_caps.h esp_psram.h esp_lvgl_port.h
               esp_qmc5883_port.h driver/uart.h driver/gpio.h
               freertos/FreeRTOS.h freertos/task.h freertos/semphr.h)
    get_filename_component(dir "${CMAKE_CURRENT_BINARY_DIR}/stubs/${header}" DIRECTORY)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stubs/${header}"
         "#include \"${CMAKE_CURRENT_SOURCE_DIR}/platform_stubs.h\"\n")
endforeach()

file(GLOB_RECURSE LVGL_SOURCES "${LVGL_ROOT}/src/*.c")
add_library(lvgl_host STATIC ${LVGL_SOURCES})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_SKIP LV_KCONFIG_IGNORE
    LV_FONT_MONTSERRAT_20=1 LV_MEM_SIZE=262144 LV_COLOR_16_SWAP=1)
target_include_directories(lvgl_host PUBLIC "${LVGL_ROOT}")

add_executable(navigation_tests navigation_tests.cpp
    "${PROJECT_ROOT}/components/neo_m8n/neo_m8n.c"
    "${PROJECT_ROOT}/components/lvgl_ui/images/navigation_arrow.cpp")
target_include_directories(navigation_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/stubs"
    "${PROJECT_ROOT}/components/neo_m8n" "${PROJECT_ROOT}/components/lvgl_ui")
target_link_libraries(navigation_tests PRIVATE lvgl_host)
enable_testing()
add_test(NAME navigation COMMAND navigation_tests)
