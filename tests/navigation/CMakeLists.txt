cmake_minimum_required(VERSION 3.16)
project(navigation_tests C CXX)
set(CMAKE_CXX_STANDARD 20)
if(MSVC)
    add_compile_options(/utf-8)
endif()
get_filename_component(PROJECT_ROOT "../.." ABSOLUTE)
set(LVGL_ROOT "${PROJECT_ROOT}/managed_components/lvgl__lvgl" CACHE PATH "Existing LVGL 8 source tree")
if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    message(FATAL_ERROR "Set LVGL_ROOT to an existing LVGL 8 source tree")
endif()

# Generate include shims in the test build directory; production headers are untouched.
foreach(header esp_err.h esp_log.h esp_heap_caps.h esp_psram.h esp_lvgl_port.h
               esp_qmc5883_port.h driver/uart.h driver/gpio.h
               freertos/FreeRTOS.h freertos/task.h freertos/semphr.h)
    get_filename_component(dir "${CMAKE_CURRENT_BINARY_DIR}/stubs/${header}" DIRECTORY)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stubs/${header}"
         "#include \"platform_stubs.h\"\n")
endforeach()

if(MSVC)
    # lib.exe reads UTF-8 response-file paths as ANSI. Mirror unchanged dependencies
    # into the ignored build directory so all object paths in response files are ASCII.
    file(COPY "${LVGL_ROOT}/src" "${LVGL_ROOT}/lvgl.h" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
    set(LVGL_ROOT "${CMAKE_CURRENT_BINARY_DIR}/lvgl")
endif()
file(GLOB_RECURSE LVGL_SOURCES "${LVGL_ROOT}/src/*.c")
add_library(lvgl_host STATIC ${LVGL_SOURCES})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_SKIP LV_KCONFIG_IGNORE
    LV_FONT_MONTSERRAT_20=1 LV_MEM_SIZE=262144 LV_COLOR_16_SWAP=1)
target_include_directories(lvgl_host PUBLIC "${LVGL_ROOT}")

set(GPS_SOURCE "${PROJECT_ROOT}/components/neo_m8n/neo_m8n.c")
set(ARROW_SOURCE "${PROJECT_ROOT}/components/lvgl_ui/images/navigation_arrow.cpp")
if(MSVC)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/project_sources")
    configure_file("${GPS_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/project_sources/neo_m8n.c" COPYONLY)
    configure_file("${ARROW_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/project_sources/navigation_arrow.cpp" COPYONLY)
    set(GPS_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/project_sources/neo_m8n.c")
    set(ARROW_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/project_sources/navigation_arrow.cpp")
endif()
add_executable(navigation_tests navigation_tests.cpp "${GPS_SOURCE}" "${ARROW_SOURCE}")
target_include_directories(navigation_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/stubs"
    "${PROJECT_ROOT}/components/neo_m8n" "${PROJECT_ROOT}/components/lvgl_ui"
    "${PROJECT_ROOT}/components/lvgl_ui/images")
target_link_libraries(navigation_tests PRIVATE lvgl_host)
enable_testing()
add_test(NAME navigation COMMAND navigation_tests)

add_executable(map_tile_diagnostics_tests map_tile_diagnostics_test.cpp)
if(MSVC)
    target_compile_definitions(map_tile_diagnostics_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME map_tile_diagnostics COMMAND map_tile_diagnostics_tests)
