add_library(common INTERFACE)

target_link_libraries(common INTERFACE stockfish)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
