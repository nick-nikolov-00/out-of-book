add_executable(line_odds line_odds.cpp Book.cpp Reachability.cpp TargetGraph.cpp)
target_link_libraries(line_odds PRIVATE stockfish mm common)
