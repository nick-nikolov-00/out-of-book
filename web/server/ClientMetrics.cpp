#include "ClientMetrics.h"

#include <array>

namespace clientmetrics {

namespace {

/*
 * The wire names, kept to two characters because they ride on a header attached
 * to requests that are already carrying a FEN.
 */
enum class Field {
  PageLoadMs,
  CacheHits,
  CacheMisses,
  UncaughtErrors,
  ApiErrors,
  PositionWaits,
  SlowPositionWaits,
  VersionReloads,
  VersionStuck,
  Unknown,
};

constexpr size_t FIELD_COUNT = static_cast<size_t>(Field::Unknown);

Field fieldOf(std::string_view key) {
  if (key == "pl")
    return Field::PageLoadMs;
  if (key == "ch")
    return Field::CacheHits;
  if (key == "cm")
    return Field::CacheMisses;
  if (key == "ee")
    return Field::UncaughtErrors;
  if (key == "ea")
    return Field::ApiErrors;
  if (key == "wn")
    return Field::PositionWaits;
  if (key == "ws")
    return Field::SlowPositionWaits;
  if (key == "vr")
    return Field::VersionReloads;
  if (key == "vs")
    return Field::VersionStuck;

  return Field::Unknown;
}

void store(Report& report, Field field, uint32_t value) {
  switch (field) {
  case Field::PageLoadMs:
    report.pageLoadMs = value;
    return;
  case Field::CacheHits:
    report.cacheHits = value;
    return;
  case Field::CacheMisses:
    report.cacheMisses = value;
    return;
  case Field::UncaughtErrors:
    report.uncaughtErrors = value;
    return;
  case Field::ApiErrors:
    report.apiErrors = value;
    return;
  case Field::PositionWaits:
    report.positionWaits = value;
    return;
  case Field::SlowPositionWaits:
    report.slowPositionWaits = value;
    return;
  case Field::VersionReloads:
    report.versionReloads = value;
    return;
  case Field::VersionStuck:
    report.versionStuck = value;
    return;
  case Field::Unknown:
    return;
  }
}

/*
 * A decimal integer, or nothing.
 *
 * Ten digits is already past what any ceiling below allows, so the length cap
 * is what keeps the accumulator inside uint32_t rather than a check after the
 * fact. Nothing is trimmed first: a space or a sign means the header was not
 * written by the client this expects.
 */
std::optional<uint32_t> parseValue(std::string_view text) {
  if (text.empty() || text.size() > 9)
    return std::nullopt;

  uint32_t value = 0;

  for (const char c : text) {
    if (c < '0' || c > '9')
      return std::nullopt;

    value = value * 10 + static_cast<uint32_t>(c - '0');
  }

  return value;
}

} // namespace

std::optional<Report> parse(std::string_view header) {
  if (header.empty() || header.size() > MAX_HEADER_BYTES)
    return std::nullopt;

  Report report;
  std::array<bool, FIELD_COUNT> seen{};

  size_t at = 0;

  while (at <= header.size()) {
    const size_t comma = header.find(',', at);
    const std::string_view pair =
        header.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);

    const size_t equals = pair.find('=');

    if (equals == std::string_view::npos)
      return std::nullopt;

    const std::string_view key = pair.substr(0, equals);

    /* An empty key is not an unknown key. Skipping it would let a malformed
     * header be accepted as an empty report. */
    if (key.empty())
      return std::nullopt;
    const std::optional<uint32_t> value = parseValue(pair.substr(equals + 1));

    if (!value)
      return std::nullopt;

    const Field field = fieldOf(key);

    if (field != Field::Unknown) {
      const size_t index = static_cast<size_t>(field);

      /* A repeated key is not a client this understands, and picking either
       * occurrence would be a guess. */
      if (seen[index])
        return std::nullopt;

      seen[index] = true;

      if (*value > (field == Field::PageLoadMs ? MAX_PAGE_LOAD_MS : MAX_COUNT))
        return std::nullopt;

      store(report, field, *value);
    }

    if (comma == std::string_view::npos)
      break;

    at = comma + 1;
  }

  /* The slow waits are drawn from the total, so a report claiming more of them
   * than there were waits describes nothing that can have happened. */
  if (report.slowPositionWaits > report.positionWaits)
    return std::nullopt;

  return report;
}

} // namespace clientmetrics
