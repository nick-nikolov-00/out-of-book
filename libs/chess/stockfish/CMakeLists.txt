add_library(stockfish)

target_include_directories(stockfish PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(stockfish PRIVATE
        misc.cpp
        bitboard.cpp
        position.cpp
        movegen.cpp
        uci.cpp
)

target_link_libraries(stockfish PRIVATE ctre)