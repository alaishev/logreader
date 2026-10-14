find_package(benchmark REQUIRED CONFIG)

# Performance benchmarks of the LogReader class, run via run-bench.sh
add_executable(logreader_bench
    log_reader_bench.cpp
)

target_link_libraries(logreader_bench PRIVATE
    logreader::logreader
    benchmark::benchmark_main
)
