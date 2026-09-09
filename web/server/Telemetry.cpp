#include "Telemetry.h"

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>

#include <fstream>
#include <unistd.h>

namespace telemetry {

namespace {

const char* nameOf(Endpoint endpoint) {
  switch (endpoint) {
  case Endpoint::Position:
    return "position";
  case Endpoint::Health:
    return "health";
  case Endpoint::UnknownApi:
    return "unknown_api";
  case Endpoint::Rejected:
    return "rejected";
  }

  return "other";
}

const char* nameOf(PositionResult result) {
  switch (result) {
  case PositionResult::Found:
    return "found";
  case PositionResult::NotFound:
    return "not_found";
  case PositionResult::BadFen:
    return "bad_fen";
  case PositionResult::Error:
    return "error";
  }

  return "other";
}

/*
 * Bucket boundaries in seconds.
 *
 * The interesting split is page cache versus disk: a position already resident
 * answers in single-digit milliseconds, while one that has to pread cold blocks
 * off disk is an order of magnitude slower. Buckets are dense through
 * that range and thin out afterwards, because past a second the only question
 * is "how bad", not "how much worse than 1.1 s".
 */
const prometheus::Histogram::BucketBoundaries kLatencyBuckets = {
    0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0};

/*
 * Bucket boundaries for how long an accepted connection waited for a worker.
 *
 * Healthy is indistinguishable from zero, so the low buckets exist only to hold
 * "no contention" apart from "a little". The number that matters is how much
 * sits above a few milliseconds, because anything there means every worker was
 * busy. The tail runs past the keep-alive idle timeout, since a queue behind
 * fully occupied workers drains at that rhythm.
 */
const prometheus::Histogram::BucketBoundaries kConnectionQueueBuckets = {
    0.0001, 0.001, 0.01, 0.05, 0.1, 0.5, 1.0, 2.5, 5.0, 10.0};

/*
 * Bucket boundaries for how long a page took to become a board, in seconds.
 *
 * This is the whole arrival: connection, a 100 KB compressed bundle over
 * whatever uplink the visitor has, parse, and the first paint. Sub-second is
 * the good case and only worth splitting coarsely; the buckets are dense from
 * there to about five seconds, which is the range in which somebody decides
 * whether the site is worth waiting for.
 */
const prometheus::Histogram::BucketBoundaries kPageLoadBuckets = {
    0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 5.0, 8.0, 15.0};

/* Resident and virtual size from /proc/self/statm, which reports pages.
 * Returns false rather than guessing if the file is unreadable, so the gauge is
 * left absent instead of reading zero: on a memory panel those mean very
 * different things. */
bool readMemory(double& resident, double& virt) {
  std::ifstream statm("/proc/self/statm");

  if (!statm)
    return false;

  unsigned long long vmPages = 0;
  unsigned long long rssPages = 0;

  if (!(statm >> vmPages >> rssPages))
    return false;

  const double pageSize = static_cast<double>(::sysconf(_SC_PAGESIZE));

  virt = static_cast<double>(vmPages) * pageSize;
  resident = static_cast<double>(rssPages) * pageSize;
  return true;
}

} // namespace

Telemetry::Telemetry() : registry(std::make_shared<prometheus::Registry>()) {
  requests = &prometheus::BuildCounter()
                  .Name("initium_http_requests_total")
                  .Help("HTTP requests answered, by handler and status class")
                  .Register(*registry);

  requestDuration = &prometheus::BuildHistogram()
                         .Name("initium_http_request_duration_seconds")
                         .Help("Wall time from pre-routing to response, by handler")
                         .Register(*registry);

  positionResults = &prometheus::BuildCounter()
                         .Name("initium_position_results_total")
                         .Help("What /api/position lookups resolved to")
                         .Register(*registry);

  connectionQueue = &prometheus::BuildHistogram()
                         .Name("initium_connection_queue_seconds")
                         .Help("Wait between accepting a connection and a worker starting it")
                         .Register(*registry);

  connections = &prometheus::BuildGauge()
                     .Name("initium_connections")
                     .Help("Accepted connections, by whether a worker is serving them yet")
                     .Register(*registry);

  workerThreads = &prometheus::BuildGauge()
                       .Name("initium_worker_threads")
                       .Help("Size of the connection thread pool")
                       .Register(*registry);

  rateLimited = &prometheus::BuildCounter()
                     .Name("initium_rate_limited_total")
                     .Help("Requests refused by the per-client rate limiter")
                     .Register(*registry);

  rateLimitClients = &prometheus::BuildGauge()
                          .Name("initium_rate_limit_clients")
                          .Help("Client buckets the rate limiter is tracking")
                          .Register(*registry);

  clientPageLoad = &prometheus::BuildHistogram()
                        .Name("initium_client_page_load_seconds")
                        .Help("Browser time from navigation to a painted board")
                        .Register(*registry);

  clientCache = &prometheus::BuildCounter()
                     .Name("initium_client_cache_total")
                     .Help("Position lookups the browser answered from its own cache, or did not")
                     .Register(*registry);

  clientErrors = &prometheus::BuildCounter()
                      .Name("initium_client_errors_total")
                      .Help("Browser-side failures, by whether anything caught them")
                      .Register(*registry);

  clientPositionWaits = &prometheus::BuildCounter()
                             .Name("initium_client_position_waits_total")
                             .Help("Waits for a position as the browser measured them, by speed")
                             .Register(*registry);

  clientVersionEvents = &prometheus::BuildCounter()
                             .Name("initium_client_version_events_total")
                             .Help("Pages reloaded by the contract-version check, and pages it could not fix")
                             .Register(*registry);

  clientReports = &prometheus::BuildCounter()
                       .Name("initium_client_reports_total")
                       .Help("Flush headers offered by browsers, by whether the parser took them")
                       .Register(*registry);

  rateLimitedTotal = &rateLimited->Add({});
  connectionQueueWait = &connectionQueue->Add({}, kConnectionQueueBuckets);
  connectionsQueued = &connections->Add({{"state", "queued"}});
  connectionsActive = &connections->Add({{"state", "active"}});

  clientPageLoadSeconds = &clientPageLoad->Add({}, kPageLoadBuckets);
  clientCacheHits = &clientCache->Add({{"result", "hit"}});
  clientCacheMisses = &clientCache->Add({{"result", "miss"}});
  clientUncaughtErrors = &clientErrors->Add({{"kind", "uncaught"}});
  clientApiErrors = &clientErrors->Add({{"kind", "api"}});
  clientFastWaits = &clientPositionWaits->Add({{"speed", "fast"}});
  clientSlowWaits = &clientPositionWaits->Add({{"speed", "slow"}});
  clientVersionReloads = &clientVersionEvents->Add({{"event", "reload"}});
  clientVersionStuck = &clientVersionEvents->Add({{"event", "stuck"}});
  clientReportsAccepted = &clientReports->Add({{"result", "accepted"}});
  clientReportsRejected = &clientReports->Add({{"result", "rejected"}});

  residentBytes = &prometheus::BuildGauge()
                       .Name("initium_process_resident_bytes")
                       .Help("Resident set size of the server process")
                       .Register(*registry);

  virtualBytes = &prometheus::BuildGauge()
                      .Name("initium_process_virtual_bytes")
                      .Help("Virtual size of the server process")
                      .Register(*registry);
}

Telemetry::~Telemetry() = default;

void Telemetry::observeRequest(Endpoint endpoint, int status, double seconds) {
  const char* handler = nameOf(endpoint);

  /* The status is bucketed to its class rather than kept exact: exact codes
   * trade a series per code for detail already in the logs, and the question a
   * dashboard asks here is "is anything failing", which the class answers. */
  char statusClass[4] = {'2', 'x', 'x', '\0'};

  if (status >= 100 && status < 600)
    statusClass[0] = static_cast<char>('0' + status / 100);

  requests->Add({{"endpoint", handler}, {"status", statusClass}}).Increment();
  requestDuration->Add({{"endpoint", handler}}, kLatencyBuckets).Observe(seconds);
}

void Telemetry::observePosition(PositionResult result) {
  positionResults->Add({{"result", nameOf(result)}}).Increment();
}

void Telemetry::observeConnectionQueued() { connectionsQueued->Increment(); }

void Telemetry::observeConnectionStarted(double waitedSeconds) {
  connectionsQueued->Decrement();
  connectionsActive->Increment();
  connectionQueueWait->Observe(waitedSeconds);
}

void Telemetry::observeConnectionFinished() { connectionsActive->Decrement(); }

void Telemetry::observeConnectionDropped() { connectionsQueued->Decrement(); }

void Telemetry::setWorkerThreads(uint64_t threads) {
  workerThreads->Add({}).Set(static_cast<double>(threads));
}

void Telemetry::observeRateLimited() { rateLimitedTotal->Increment(); }

void Telemetry::setRateLimitClients(uint64_t clients) {
  rateLimitClients->Add({}).Set(static_cast<double>(clients));
}

void Telemetry::observeClientReport(const clientmetrics::Report& report) {
  clientReportsAccepted->Increment();

  if (report.pageLoadMs)
    clientPageLoadSeconds->Observe(static_cast<double>(*report.pageLoadMs) / 1000.0);

  /* Increment() with an argument rather than a loop: a flush carries a count,
   * and the counter only ever needs the sum. */
  clientCacheHits->Increment(report.cacheHits);
  clientCacheMisses->Increment(report.cacheMisses);
  clientUncaughtErrors->Increment(report.uncaughtErrors);
  clientApiErrors->Increment(report.apiErrors);
  clientVersionReloads->Increment(report.versionReloads);
  clientVersionStuck->Increment(report.versionStuck);

  /* The client counts its waits and how many were slow; fast is the remainder,
   * so the two series partition the total the way a label ought to. The parser
   * has already refused a report where the subset was the larger number. */
  clientSlowWaits->Increment(report.slowPositionWaits);
  clientFastWaits->Increment(report.positionWaits - report.slowPositionWaits);
}

void Telemetry::observeClientReportRejected() { clientReportsRejected->Increment(); }

std::string Telemetry::scrape() {
  double resident = 0;
  double virt = 0;

  if (readMemory(resident, virt)) {
    residentBytes->Add({}).Set(resident);
    virtualBytes->Add({}).Set(virt);
  }

  return prometheus::TextSerializer().Serialize(registry->Collect());
}

} // namespace telemetry
