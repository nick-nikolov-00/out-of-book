add_executable(generate_otb generate_otb.cpp)
target_link_libraries(generate_otb PRIVATE stockfish mm common metrics)
