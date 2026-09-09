add_executable(fen_stats fen_stats.cpp BoardDB.cpp)
target_link_libraries(fen_stats PRIVATE stockfish mm common)

