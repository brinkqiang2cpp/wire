#	/tip-proto/test/CMakeLists.txt
#	
#	@author zmij
#	@date Dec 10, 2015

cmake_minimum_required(VERSION 2.6)

find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR})

set(test_SRCS
    static_tests.cpp
    bits_tests.cpp
    concatenate_test.cpp
    encoding_test.cpp
    variant_io_test.cpp
    message_io_test.cpp
    outgoing_buffer_test.cpp
    endpoints_io_test.cpp
    endpoints_grammar_test.cpp
    uuid_io_test.cpp
    identity_io_test.cpp
)

add_executable(test-wire ${test_SRCS})
target_link_libraries(test-wire
    ${GTEST_BOTH_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${WIRE_LIB}
)

if (GTEST_XML_OUTPUT)
    set (
        BASE_TEST_ARGS --gtest_output=xml:test-wire.xml
    )
endif()

add_test(
    NAME test-wire
    COMMAND test-wire ${BASE_TEST_ARGS}
)