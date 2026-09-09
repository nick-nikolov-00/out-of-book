add_executable(db_to_spills db_to_spills.cpp extractor.cpp RecordCache.cpp RecordCache.h)
target_link_libraries(db_to_spills PRIVATE metrics stockfish cache queue zst_streamer common)

