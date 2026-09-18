cmake_minimum_required(VERSION 3.16)
project(couart VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_definitions(_GNU_SOURCE _DEFAULT_SOURCE _XOPEN_SOURCE=600)
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(cjson STATIC deps/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC deps/cJSON)

set(COUART_LIB_SRCS
    src/log.c
    src/path.c
    src/uart.c
    src/seat.c
    src/park.c
    src/hub.c
    src/ctl.c
    src/hex.c
)

add_library(couart_lib STATIC ${COUART_LIB_SRCS})
target_include_directories(couart_lib PUBLIC src)
target_link_libraries(couart_lib PUBLIC util cjson)

add_executable(couart src/main.c src/cli.c src/serve.c src/mcp.c)
target_link_libraries(couart PRIVATE couart_lib)
target_compile_definitions(couart PRIVATE
    COUART_VERSION="${PROJECT_VERSION}"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_path tests/test_path.c)
    target_include_directories(test_path PRIVATE tests)
    target_link_libraries(test_path PRIVATE couart_lib)
    add_test(NAME test_path COMMAND test_path)

    add_executable(test_hub tests/test_hub.c)
    target_include_directories(test_hub PRIVATE tests)
    target_link_libraries(test_hub PRIVATE couart_lib pthread)
    add_test(NAME test_hub COMMAND test_hub)
endif()

install(TARGETS couart RUNTIME DESTINATION bin)
install(FILES contrib/couart@.service DESTINATION lib/systemd/user)
