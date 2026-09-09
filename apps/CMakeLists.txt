add_executable(scratch scratch.cpp)
target_link_libraries(scratch PRIVATE stockfish zst_streamer)

add_subdirectory(expectimax)
add_subdirectory(db_to_spills)
add_subdirectory(agg_spills)
add_subdirectory(fen_stats)
add_subdirectory(agg_merger)
add_subdirectory(generate_otb)
add_subdirectory(calibration)

add_subdirectory(verify_ote)

add_subdirectory(export_subtree)

add_subdirectory(line_odds)

add_subdirectory(policy_sim)
