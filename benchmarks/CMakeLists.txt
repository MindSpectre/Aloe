set(BENCHMARKS ${PROJECT_NAME}.Benchmarks)

# Google Benchmark is resolved by the root CMakeLists.txt, as benchmark::benchmark, for microbenchmarks that may
# come later. The tick-to-trade benchmark records its own distributions and does not link it.
add_subdirectory(tick_to_trade)
