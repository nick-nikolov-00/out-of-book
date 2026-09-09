#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace clientmetrics {

/*
 * One flush from one browser: what that tab counted since it last reported.
 *
 * Deltas rather than running totals, because a tab is not a time series. To
 * accept absolutes this side would have to keep a previous value per client to
 * difference against; adding deltas into one process-wide counter costs nothing
 * and makes a flush that never arrives an undercount rather than a counter that
 * goes backwards.
 */
struct Report {
  /* Milliseconds from navigation to a painted board. Measured once per page,
   * so it is absent from almost every flush. */
  std::optional<uint32_t> pageLoadMs;

  uint32_t cacheHits = 0;
  uint32_t cacheMisses = 0;

  /* Exceptions and rejections nothing caught. */
  uint32_t uncaughtErrors = 0;

  /* Request failures that reached the screen as an error rather than a board. */
  uint32_t apiErrors = 0;

  /* Waits for a position, and how many of those were slow enough to notice.
   * slowPositionWaits is a subset, so it is never the larger of the two. */
  uint32_t positionWaits = 0;
  uint32_t slowPositionWaits = 0;

  /* The contract-version mechanism firing: a page that reloaded itself onto a
   * new bundle, and a page that reloaded and found the same disagreement. */
  uint32_t versionReloads = 0;
  uint32_t versionStuck = 0;
};

/*
 * The most this will read. A flush is a handful of small integers, so anything
 * longer is not a flush and is not worth parsing to find out.
 */
inline constexpr size_t MAX_HEADER_BYTES = 256;

/*
 * The largest plausible value for a count, and for a page load in
 * milliseconds. A tab flushes about once a minute, so a legitimate count is in
 * the hundreds; these leave well over an order of magnitude of room and still
 * keep a single header from moving a counter by an implausible amount.
 */
inline constexpr uint32_t MAX_COUNT = 10000;
inline constexpr uint32_t MAX_PAGE_LOAD_MS = 600000;

/*
 * Read a flush header into a report, or nothing at all.
 *
 * The whole header is refused rather than partly applied: half a report is a
 * number nobody can reason about afterwards. Refusals are counted, so a client
 * that has started sending nonsense is visible rather than silently ignored.
 *
 * Keys this build does not know are skipped instead of refused. Adding a metric
 * is not a change an already-loaded page can misread, so it does not bump the
 * contract version — which means a newer bundle talking to an older server is a
 * situation that is allowed to happen and must not lose the keys they agree on.
 */
std::optional<Report> parse(std::string_view header);

} // namespace clientmetrics
