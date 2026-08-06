cmake_minimum_required(VERSION 3.16)
project(epistemic_planner CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(nlohmann_json 3.10 REQUIRED)

set(SOURCES
    src/main.cpp
    src/formula.cpp
    src/state.cpp
    src/action.cpp
    src/parser.cpp
    src/product_update.cpp
    src/bisimulation.cpp
    src/heuristic.cpp
    src/selection_policy.cpp
    src/search.cpp
    src/validator.cpp
)

add_executable(epistemic_planner ${SOURCES})

target_include_directories(epistemic_planner PRIVATE include)
target_link_libraries(epistemic_planner PRIVATE nlohmann_json::nlohmann_json)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(ipo_ok AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set_property(TARGET epistemic_planner PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
  message(STATUS "IPO/LTO not enabled: ${ipo_msg}")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_custom_command(TARGET epistemic_planner POST_BUILD
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:epistemic_planner>
    COMMENT "Stripping binary"
  )
endif()
