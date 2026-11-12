add_executable(crowdsim_bench bench_sim.cpp)
target_link_libraries(crowdsim_bench PRIVATE CrowdSim::sim benchmark::benchmark)
crowdsim_set_warnings(crowdsim_bench)
