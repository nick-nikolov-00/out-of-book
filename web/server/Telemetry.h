#pragma once

#include "ClientMetrics.h"

#include <cstdint>
#include <memory>
#include <string>

namespace prometheus {
class Registry;
class Counter;
class Gauge;
class Histogram;
template <typename T> class Family;
} // namespace prometheus

namespace telemetry {

/*
 * Which handler answered, as a closed set.
 *
 * An enum rather than the request path: a label whose values come from the
 * caller is a label whose cardinality the caller chooses. Every value below is
 * decided here.
 */
enum class Endpoint {
  Position,   /* /api/position */
  Health,     /* /api/health */
  UnknownApi, /* /api/... that no handler claims */
  Rejected,   /* refused by the pre-routing handler: not a GET or a HEAD */
};

/* What a /api/position request turned into. Distinct from the HTTP status,
 * because "not in the tree" is a perfectly good 200 and is the interesting
 * number when judging whether the tree covers what people look up. */
enum class PositionResult {
  Found,
  NotFound,
  BadFen, /* rejected by fen::validate before any lookup */
  Error,  /* the handler threw */
};

/*
 * The process' metrics. One instance lives in main(); everything is safe to
 * call from any request thread, since Add() on an existing label set returns
 * the same object and the counters themselves are atomic.
 */
class Telemetry {
public:
  Telemetry();
  ~Telemetry();

  Telemetry(const Telemetry&) = delete;
  Telemetry& operator=(const Telemetry&) = delete;

  void observeRequest(Endpoint endpoint, int status, double seconds);
  void observePosition(PositionResult result);

  /*
   * Connection accounting, separate from request accounting on purpose. A
   * worker holds a connection for its whole life, keep-alive idle time
   * included, so the pool size is a ceiling on concurrent *connections* and is
   * reached long before any per-request number looks unusual.
   *
   * The wait for a worker cannot be seen from inside a handler, since the
   * request timer only starts once a worker has picked the connection up.
   * Without these, running out of workers looks exactly like a healthy server.
   */
  void observeConnectionQueued();
  void observeConnectionStarted(double waitedSeconds);
  void observeConnectionFinished();

  /* Enqueue refused; the pool is built without a bound today, so this exists
   * to keep the queued gauge honest if one is ever set. */
  void observeConnectionDropped();

  /* Constant, so a dashboard can divide active connections by it to get
   * saturation without hard-coding the pool size in the query. */
  void setWorkerThreads(uint64_t threads);

  /* One /api/ request refused by the rate limiter. Kept apart from the 4xx
   * counter, which cannot say whether a refusal was a bad FEN or a client being
   * throttled — and those two want different reactions. */
  void observeRateLimited();

  /* Client buckets currently held, sampled at scrape rather than kept live:
   * it is a table size, and nothing needs it between scrapes. */
  void setRateLimitClients(uint64_t clients);

  /*
   * One accepted flush from a browser, and one refused by the parser.
   *
   * Nothing in the report becomes a label, and every value is a count the
   * parser has already bounded. The numbers say what the pages report, which is
   * not quite the same as what the pages did.
   */
  void observeClientReport(const clientmetrics::Report& report);
  void observeClientReportRejected();

  /* The Prometheus text exposition format, for the /metrics handler. Reads
   * /proc/self/statm on the way through, so the memory gauges are sampled at
   * scrape time rather than kept up to date on a timer nobody needs. */
  std::string scrape();

private:
  std::shared_ptr<prometheus::Registry> registry;

  prometheus::Family<prometheus::Counter>* requests;
  prometheus::Family<prometheus::Histogram>* requestDuration;
  prometheus::Family<prometheus::Counter>* positionResults;
  prometheus::Family<prometheus::Histogram>* connectionQueue;
  prometheus::Family<prometheus::Gauge>* connections;
  prometheus::Family<prometheus::Gauge>* workerThreads;
  prometheus::Family<prometheus::Counter>* rateLimited;
  prometheus::Family<prometheus::Gauge>* rateLimitClients;
  prometheus::Family<prometheus::Histogram>* clientPageLoad;
  prometheus::Family<prometheus::Counter>* clientCache;
  prometheus::Family<prometheus::Counter>* clientErrors;
  prometheus::Family<prometheus::Counter>* clientPositionWaits;
  prometheus::Family<prometheus::Counter>* clientVersionEvents;
  prometheus::Family<prometheus::Counter>* clientReports;

  /* Resolved once in the constructor rather than looked up per connection:
   * these are touched on the accept path, where the family's map lookup and
   * lock would be pure overhead. */
  prometheus::Histogram* connectionQueueWait;

  /* Also resolved up front so the series exists at zero from startup. A counter
   * that only appears on its first increment reads as "no data" on a panel
   * until the thing it counts happens, which is exactly backwards for one that
   * is meant to be flat. */
  prometheus::Counter* rateLimitedTotal;

  /* Resolved up front for the same reason, and because a flush touches most of
   * them at once: a report should not pay a family lookup per field. */
  prometheus::Histogram* clientPageLoadSeconds;
  prometheus::Counter* clientCacheHits;
  prometheus::Counter* clientCacheMisses;
  prometheus::Counter* clientUncaughtErrors;
  prometheus::Counter* clientApiErrors;
  prometheus::Counter* clientFastWaits;
  prometheus::Counter* clientSlowWaits;
  prometheus::Counter* clientVersionReloads;
  prometheus::Counter* clientVersionStuck;
  prometheus::Counter* clientReportsAccepted;
  prometheus::Counter* clientReportsRejected;

  prometheus::Gauge* connectionsQueued;
  prometheus::Gauge* connectionsActive;
  prometheus::Family<prometheus::Gauge>* residentBytes;
  prometheus::Family<prometheus::Gauge>* virtualBytes;
};

} // namespace telemetry
