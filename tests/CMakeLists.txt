find_package(GTest REQUIRED CONFIG)

include(GoogleTest)

add_executable(logreader_tests
    filter_test.cpp
    log_reader_test.cpp
)

target_link_libraries(logreader_tests PRIVATE
    logreader::logreader
    GTest::gtest_main
)

gtest_discover_tests(logreader_tests)
