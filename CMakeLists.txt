cmake_minimum_required(VERSION 3.24)
project(ghostctl VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v7.0.3
  GIT_SHALLOW TRUE)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ftxui stb)

find_package(PkgConfig REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp libwebpdemux)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

# Embed the page scripts (page reader, selector helpers, inspect collector).
foreach(js page helpers collect)
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/${js}.js JS_CONTENT)
  set(JS_NAME ${js})
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/embed_js.hpp.in
                 ${CMAKE_CURRENT_BINARY_DIR}/gen/${js}_js.hpp @ONLY)
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/page.js ${CMAKE_CURRENT_SOURCE_DIR}/src/helpers.js ${CMAKE_CURRENT_SOURCE_DIR}/src/collect.js)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(ghostctl ${SOURCES})
target_include_directories(ghostctl PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/gen ${stb_SOURCE_DIR})
target_compile_options(ghostctl PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(ghostctl PRIVATE
  ftxui::screen ftxui::dom ftxui::component
  nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus
  PkgConfig::SECRET PkgConfig::WEBP PkgConfig::ZLIB Threads::Threads)

install(TARGETS ghostctl RUNTIME DESTINATION bin)
