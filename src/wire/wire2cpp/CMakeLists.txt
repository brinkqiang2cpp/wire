# CMakeLists.txt
# Created on: 25 февр. 2016 г.
#     Author: sergey.fedorov

cmake_minimum_required(VERSION 2.6)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

set(
    wire2cpp_SRCS
    plugin_manager.cpp
    cpp_generator.cpp
    fwd_generator.cpp
    cpp_source_stream.cpp
    keywords.cpp
)

add_library(wire-to-cpp SHARED ${wire2cpp_SRCS})
target_link_libraries(wire-to-cpp
    ${Boost_FILESYSTEM_LIBRARIES}
    ${WIRE_IDL_LIB}
)

add_executable(wire2cpp wire2cpp.cpp)
target_link_libraries(wire2cpp
    ${Boost_PROGRAM_OPTIONS_LIBRARIES}
    wire-to-cpp
)

install(
    TARGETS wire-to-cpp wire2cpp
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
