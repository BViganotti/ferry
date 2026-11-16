cmake_minimum_required(VERSION 3.20)
project(ferry VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FERRY_BUILD_TESTS "Build ferry tests" ON)
option(FERRY_BUILD_EXAMPLES "Build ferry examples" ON)
option(FERRY_BUILD_BENCH "Build the throughput benchmark" ON)
option(FERRY_WITH_ZLIB "Enable gzip request/response compression" ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(FERRY_SOURCES
  src/event_loop.cpp
  src/net.cpp
  src/tls.cpp
  src/url.cpp
  src/http.cpp
  src/codec.cpp
  src/router.cpp
  src/compression.cpp
  src/cookie_jar.cpp
  src/multipart.cpp
  src/server.cpp
  src/client.cpp
)

add_library(ferry ${FERRY_SOURCES})
add_library(ferry::ferry ALIAS ferry)

target_include_directories(ferry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(ferry PUBLIC cxx_std_20)
target_compile_options(ferry PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ferry
  PUBLIC Threads::Threads
  PRIVATE OpenSSL::SSL OpenSSL::Crypto
)

set(FERRY_ZLIB_ENABLED OFF)
if(FERRY_WITH_ZLIB)
  find_package(ZLIB)
  if(ZLIB_FOUND)
    set(FERRY_ZLIB_ENABLED ON)
    target_compile_definitions(ferry PUBLIC FERRY_HAS_ZLIB=1)
    target_link_libraries(ferry PRIVATE ZLIB::ZLIB)
  else()
    message(WARNING "zlib not found; gzip support disabled")
  endif()
endif()

if(FERRY_BUILD_TESTS)
  enable_testing()
  add_executable(ferry_tests tests/tests.cpp)
  target_link_libraries(ferry_tests PRIVATE ferry)
  target_include_directories(ferry_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(ferry_tests PRIVATE
    FERRY_TEST_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/certs"
    FERRY_TEST_HTTP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
  )
  add_test(NAME ferry_tests COMMAND ferry_tests)
endif()

if(FERRY_BUILD_EXAMPLES)
  add_executable(ferry_hello examples/hello.cpp)
  target_link_libraries(ferry_hello PRIVATE ferry)

  add_executable(ferry_https examples/https.cpp)
  target_link_libraries(ferry_https PRIVATE ferry)

  add_executable(ferry_client examples/client.cpp)
  target_link_libraries(ferry_client PRIVATE ferry)
endif()

if(FERRY_BUILD_BENCH)
  add_executable(ferry_bench bench/bench.cpp)
  target_link_libraries(ferry_bench PRIVATE ferry)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ferry EXPORT ferryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ferryTargets NAMESPACE ferry:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ferry)

configure_package_config_file(
  cmake/ferryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ferryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ferry
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ferryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ferryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ferryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ferry
)
