add_library(zst_streamer)

target_include_directories(zst_streamer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_sources(zst_streamer PRIVATE ZstStreamer.cpp ZstStreamer.h)

target_link_libraries(zst_streamer PUBLIC zstd metrics)