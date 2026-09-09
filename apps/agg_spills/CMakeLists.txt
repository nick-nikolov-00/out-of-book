add_executable(agg_spills agg_spills.cpp)
target_link_libraries(agg_spills PRIVATE stockfish cache queue fastmap common)