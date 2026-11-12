add_executable(crowdsim_bench bench_sim.cpp bench_animation.cpp)
target_link_libraries(crowdsim_bench PRIVATE CrowdSim::sim CrowdSim::animation benchmark::benchmark)
crowdsim_set_warnings(crowdsim_bench)
