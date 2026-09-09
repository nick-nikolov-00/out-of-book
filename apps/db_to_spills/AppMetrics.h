#pragma once
#include <cstdint>

#include "metrics.h"
#include "ZstStreamer.h"

struct BYTES_WRITTEN : metrics::Counter<uint64_t> {};

struct GAMES_SEEN : metrics::Counter<uint64_t> {};
struct GAMES_DISCARDED : metrics::Counter<uint64_t> {};

using AppMetrics = metrics::Metrics<BYTES_READ, BYTES_DECOMPRESSED, BYTES_WRITTEN, GAMES_SEEN, GAMES_DISCARDED>;