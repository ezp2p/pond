cmake_minimum_required(VERSION 3.21)
project(pond VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The Puddle's lower layers (request signatures, key chains, JSON, HTTP, TLS) come from the Puddle, pinned as the submodule
# deps/puddle so a Pond commit always builds against the Puddle commit it was made with. -DPUDDLE_DIR=../puddle builds
# against a working checkout instead, for changing both at once (scripts/build.sh --local).
set(PUDDLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/puddle CACHE PATH "The Puddle source to build against")
if(NOT EXISTS ${PUDDLE_DIR}/CMakeLists.txt)
  message(FATAL_ERROR "no Puddle source at ${PUDDLE_DIR} (git submodule update --init, or set -DPUDDLE_DIR=)")
endif()
if(APPLE AND NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()
set(PUDDLE_BUILD_GUI OFF CACHE BOOL "" FORCE)
set(PUDDLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${PUDDLE_DIR} puddle EXCLUDE_FROM_ALL)

# MariaDB's C client (brew install mariadb / apt install libmariadb-dev)
find_path(MARIADB_INCLUDE_DIR mysql.h PATH_SUFFIXES mariadb mysql HINTS /opt/homebrew/opt/mariadb/include)
find_library(MARIADB_LIBRARY NAMES mariadb HINTS /opt/homebrew/opt/mariadb/lib /opt/homebrew/opt/mariadb/lib/mariadb)
if(NOT MARIADB_INCLUDE_DIR OR NOT MARIADB_LIBRARY)
  message(FATAL_ERROR "MariaDB client not found (brew install mariadb / apt install libmariadb-dev)")
endif()

add_compile_options(-Wall -Wextra)

add_library(pond_lib STATIC
  src/config.cpp
  src/db.cpp
  src/key.cpp
  src/listener.cpp
  src/pond.cpp
  src/routes.cpp)
target_include_directories(pond_lib PUBLIC src ${MARIADB_INCLUDE_DIR})
target_link_libraries(pond_lib PUBLIC puddle_http ${MARIADB_LIBRARY})

add_executable(pondd src/pondd.cpp)
target_link_libraries(pondd PRIVATE pond_lib)
add_executable(pondctl src/pondctl.cpp)
target_link_libraries(pondctl PRIVATE pond_lib)
install(TARGETS pondd pondctl RUNTIME DESTINATION bin)

# --- tests: each starts a throwaway MariaDB of its own (tests/testdb.h) ----------
enable_testing()
foreach(t pond e2e)
  add_executable(${t}_test tests/${t}_test.cpp)
  target_link_libraries(${t}_test PRIVATE pond_lib)
  if(t STREQUAL "e2e") # real Puddles join the Pond
    target_link_libraries(${t}_test PRIVATE puddle_service)
  endif()
  add_test(NAME ${t} COMMAND ${t}_test)
  set_tests_properties(${t} PROPERTIES TIMEOUT 120)
endforeach()
