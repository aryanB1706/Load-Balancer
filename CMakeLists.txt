cmake_minimum_required(VERSION 3.16)
project(tcp_load_balancer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -O2)

# Sanitizer builds (Task 8): cmake -B build-asan -DSANITIZE=address,undefined
# or cmake -B build-tsan -DSANITIZE=thread. Plain builds are unaffected.
if(DEFINED SANITIZE)
  add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${SANITIZE})
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

file(GLOB LB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

add_executable(lb ${LB_SOURCES})

target_include_directories(lb PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Threads (pthread) — needed for Worker / std::thread going forward.
find_package(Threads REQUIRED)
target_link_libraries(lb PRIVATE Threads::Threads)
