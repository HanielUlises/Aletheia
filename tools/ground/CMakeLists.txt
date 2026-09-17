cmake_minimum_required(VERSION 3.16)
project(aletheia_ground CXX)

# Links against a built plank checkout:
#   cmake -S tools/ground -B build-ground -DPLANK_DIR=/path/to/plank
set(PLANK_DIR "" CACHE PATH "plank source directory")
set(PLANK_BUILD_DIR "${PLANK_DIR}/build" CACHE PATH "plank build directory")

if (NOT EXISTS "${PLANK_DIR}/include/lib/epddl/grounder/grounder_helper.h")
    message(FATAL_ERROR "PLANK_DIR must point to a plank source tree")
endif ()

find_library(PLANK_EPDDL_LIB epddl_lib PATHS "${PLANK_BUILD_DIR}/src/lib/epddl" NO_DEFAULT_PATH REQUIRED)
find_library(PLANK_DEL_LIB   del_lib   PATHS "${PLANK_BUILD_DIR}/src/lib/del"   NO_DEFAULT_PATH REQUIRED)
find_package(Boost REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

add_executable(ground ground.cpp)
target_include_directories(ground PRIVATE "${PLANK_DIR}/include/lib" "${PLANK_DIR}/include" "${PLANK_BUILD_DIR}/include")
target_link_libraries(ground PRIVATE ${PLANK_EPDDL_LIB} ${PLANK_DEL_LIB} Boost::boost)
