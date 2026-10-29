cmake_minimum_required(VERSION 3.21)
project(pond VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew: the system OpenSSL is LibreSSL, and MariaDB's client lives under its own prefix. Use Homebrew's default OpenSSL
# (3 or later): its headers are the ones in /opt/homebrew/include, which other libraries' include paths also bring in, so any
# other version would be compiled against one OpenSSL's headers and linked with another's library.
if(APPLE AND EXISTS /opt/homebrew)
  if(NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl)
    set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl)
  endif()
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /opt/homebrew/opt/mariadb)
  set(ENV{MYSQL_DIR} /opt/homebrew/opt/mariadb)
endif()

# Drogon (HTTP, TLS, the MariaDB and Redis clients), built from source at a fixed release: distribution packages often leave
# out MariaDB or Redis. It needs MariaDB's C client, hiredis, jsoncpp and OpenSSL 3 (scripts/build.sh says how to get them).
include(FetchContent)
set(BUILD_CTL OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_BROTLI OFF CACHE BOOL "" FORCE)
set(BUILD_YAML_CONFIG OFF CACHE BOOL "" FORCE)
set(BUILD_ORM ON CACHE BOOL "" FORCE)
set(BUILD_REDIS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_pg TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_SQLite3 TRUE)
# patches/: Drogon leaves a connection open after a 204 even when the request asked for it to close, so a client that reads
# to the end waits for the idle timeout.
set(DROGON_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/patches/drogon-close-after-no-content.patch)
FetchContent_Declare(drogon
  GIT_REPOSITORY https://github.com/drogonframework/drogon.git
  GIT_TAG v1.9.14
  GIT_SHALLOW TRUE
  GIT_SUBMODULES trantor
  PATCH_COMMAND sh -c "git apply --reverse --check ${DROGON_PATCH} 2>/dev/null || git apply ${DROGON_PATCH}")
FetchContent_MakeAvailable(drogon)
# Trantor builds with -Werror, and OpenSSL 4 deprecates calls it still makes
target_compile_options(trantor PRIVATE -Wno-error=deprecated-declarations)
if(NOT USE_MYSQL)
  message(FATAL_ERROR "Drogon was built without MariaDB: install MariaDB's C client (brew install mariadb / apt install libmariadb-dev)")
endif()

find_package(yaml-cpp REQUIRED)
find_package(OpenSSL 3 REQUIRED)

add_library(pond_lib STATIC
  src/config.cpp
  src/db.cpp
  src/encoding.cpp
  src/files.cpp
  src/identity.cpp
  src/json.cpp
  src/key.cpp
  src/ratelimit.cpp
  src/roles.cpp
  src/pond.cpp
  src/reqsig.cpp
  src/routes.cpp
  src/text.cpp
  src/tls.cpp)
target_include_directories(pond_lib PUBLIC src)
target_link_libraries(pond_lib PUBLIC drogon yaml-cpp::yaml-cpp OpenSSL::Crypto)
target_compile_options(pond_lib PRIVATE -Wall -Wextra)

add_executable(pondd src/pondd.cpp)
target_link_libraries(pondd PRIVATE pond_lib)
add_executable(pondctl src/pondctl.cpp)
target_link_libraries(pondctl PRIVATE pond_lib)
install(TARGETS pondd pondctl RUNTIME DESTINATION bin)

# --- tests: each starts a throwaway MariaDB and a real pondd (tests/harness.h) ----------
# e2e also needs a built Puddle (puddled) to join the Pond with. It is a separate program, found at PUDDLED, and the test is
# skipped without one.
set(PUDDLED ${CMAKE_CURRENT_SOURCE_DIR}/../puddle/build/puddled CACHE FILEPATH "A built puddled, for the e2e test")
enable_testing()
foreach(t identity pond e2e)
  add_executable(${t}_test tests/${t}_test.cpp)
  target_link_libraries(${t}_test PRIVATE pond_lib OpenSSL::SSL)
  target_compile_options(${t}_test PRIVATE -Wall -Wextra)
  target_compile_definitions(${t}_test PRIVATE POND_BIN_DIR="${CMAKE_CURRENT_BINARY_DIR}" PUDDLED="${PUDDLED}")
  add_test(NAME ${t} COMMAND ${t}_test)
  set_tests_properties(${t} PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
endforeach()
add_dependencies(pond_test pondd pondctl)
add_dependencies(e2e_test pondd pondctl)
