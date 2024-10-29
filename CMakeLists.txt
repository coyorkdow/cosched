cmake_minimum_required(VERSION 3.13)

project(my_project)

message(
  STATUS
    "CMake C++ Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)
message(
  STATUS "CMake C Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
)

if(NOT DEFINED CMAKE_CXX_STANDARD)
  message(STATUS "The CPP standard is not set. Default: C++20")
  set(CMAKE_CXX_STANDARD 20)
else()
  message(STATUS "Checking the CPP standard. C++${CMAKE_CXX_STANDARD}")
  if(CMAKE_CXX_STANDARD LESS 20)
    message(FATAL_ERROR "C++20 or higher is required.")
  endif()
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS
    ON
    CACHE INTERNAL "")

find_package(GTest REQUIRED)
if(NOT GTest_FOUND)
    message(FATAL_ERROR "GTest not found")
endif()

find_package(spdlog REQUIRED)
if(NOT spdlog_FOUND)
    message(FATAL_ERROR "spdlog not found")
endif()

# list(APPEND PROJECT_SRCS
#   cosched.hpp
# )

# add_library(projectcore STATIC ${PROJECT_SRCS})
# target_include_directories(projectcore PUBLIC
#     ${CMAKE_CURRENT_SOURCE_DIR}/include
# )
# target_link_libraries(projectcore PUBLIC
#     spdlog::spdlog_header_only
# )

# add_executable(my_project ${CMAKE_CURRENT_SOURCE_DIR}/test.cc)
# target_link_libraries(my_project PRIVATE
#     projectcore
# )

enable_testing()
add_executable(my_project_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test.cc
)

target_compile_options(my_project_test PUBLIC -g -Wall)
target_compile_options(my_project_test PUBLIC -fsanitize=address)
target_link_options(my_project_test PUBLIC -fsanitize=address)

target_link_libraries(my_project_test PRIVATE
    # projectcore
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME my_project_test COMMAND my_project_test)