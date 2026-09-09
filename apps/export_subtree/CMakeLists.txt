add_executable(export_subtree export_subtree.cpp)
target_link_libraries(export_subtree PRIVATE stockfish mm common metrics)
