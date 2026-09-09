#include "Analysis.h"
#include "ApiVersion.h"
#include "ClientAddress.h"
#include "ClientMetrics.h"
#include "DubiousMoves.h"
#include "Fen.h"
#include "InstrumentedQueue.h"
#include "Json.h"
#include "Options.h"
#include "OtbStore.h"
#include "PositionJson.h"
#include "RateLimit.h"
#include "RateLimitKey.h"
#include "SelfCheck.h"
#include "Telemetry.h"

#include "bitboard.h"
#include "position.h"

#include <httplib.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

using namespace Stockfish;

namespace {

/* The 429 body. `retryAfter` is the real figure in seconds; the Retry-After
 * header beside it is whole seconds, which is all that header may carry. */
std::string rateLimitJson(double retryAfter) {
  Json json;
  json.beginObject();
  json.field("error", std::string_view("too many requests"));
  json.field("retryAfter", retryAfter);
  json.endObject();
  return json.take();
}

std::string errorJson(std::string_view message) {
  Json json;
  json.beginObject();
  json.field("error", message);
  json.endObject();
  return json.take();
}

bool declaresBody(const httplib::Request& req) {
  return req.has_header("Transfer-Encoding") || req.get_header_value_u64("Content-Length") > 0;
}

/*
 * Map a request onto the closed set of endpoint labels. Every branch returns a
 * constant, so the series count is fixed no matter what URLs arrive.
 */
telemetry::Endpoint classify(const httplib::Request& req) {
  /* Only a bodyless GET or HEAD gets past the pre-routing handler. */
  if (req.method != "GET" && req.method != "HEAD")
    return telemetry::Endpoint::Rejected;

  if (declaresBody(req))
    return telemetry::Endpoint::Rejected;

  if (req.path == "/api/position")
    return telemetry::Endpoint::Position;

  if (req.path == "/api/health")
    return telemetry::Endpoint::Health;

  /* The proxy in front forwards /api/ and serves everything else itself, so a
   * path that reaches here and matches neither handler is a caller asking for
   * something that does not exist — the same answer whether or not it happens
   * to begin with /api/. */
  return telemetry::Endpoint::UnknownApi;
}

} // namespace

int main(int argc, char** argv) {
  std::optional<Options> options;

  try {
    options = parseArgs(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  if (!options) {
    std::cerr << usage() << "\n";
    return 1;
  }

  Bitboards::init();
  Position::init();

  std::optional<OtbStore> store;

  try {
    const auto started = std::chrono::steady_clock::now();

    std::cout << "loading index from " << options->otb << std::endl;
    store.emplace(options->otb, options->ote);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    std::cout << "loaded " << store->nodeCount() << " nodes (" << store->indexBytes() / (1u << 20)
              << " MiB of index) in " << elapsed.count() << " ms\n";
    std::cout << "evaluations: " << (store->hasEvals() ? "yes" : "no") << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  DubiousMoves dubious;

  if (!options->dubious.empty()) {
    try {
      dubious = DubiousMoves::load(options->dubious);
    } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 1;
    }
  }

  /* Said out loud because a deploy that lost the file would otherwise be
   * invisible: the server would come up perfectly well and quietly go back to
   * recommending the moves the list exists to rule out. */
  std::cout << "moves the book will not recommend: " << dubious.size();

  if (options->dubious.empty())
    std::cout << " (no --dubious given)";
  else
    std::cout << " (from " << options->dubious << ")";

  std::cout << std::endl;

  if (store->hasEvals()) {
    const CheckResult check = selfCheck(*store, dubious);

    std::cout << "move ranking: reproduces " << check.agreed << " of " << check.checked
              << " stored picks" << std::endl;

    if (check.checked > 0 && check.agreed != check.checked) {
      std::cerr << "\nError: this .ote was not built with the ranking rule in\n"
                   "       common/MoveRanking.h. Serving it anyway would explain every move\n"
                   "       list with a rule the file never used. Rebuild the server against\n"
                   "       the header that built the file, or regenerate the .ote with this\n"
                   "       one.\n";
      return 1;
    }
  }

  /* The numbers are the policy, so they are stated at the call site: eight
   * requests a second sustained, forty in hand to spend at once. What each one
   * buys is argued where the config is declared. */
  ratelimit::RateLimiter limiter({.ratePerSecond = 8.0, .burst = 40.0});

  /*
   * Who those eight requests a second are counted against. Behind a reverse
   * proxy every connection arrives from the same place, so without this every
   * visitor would share one bucket.
   */
  const clientaddr::Resolver clients(options->trustForwardedFrom);

  /* Everything a serving process would have verified has now been verified,
   * so there is nothing left to do but say so. */
  if (options->checkOnly) {
    std::cout << "check passed: " << options->otb
              << (options->ote.empty() ? " (no .ote)" : " + " + options->ote) << std::endl;
    return 0;
  }

  telemetry::Telemetry metrics;

  httplib::Server server;

  /*
   * The work a connection does is pread-bound rather than CPU-bound, so threads
   * are the right resource to spend: a worker blocked on a disk read is not
   * competing for a core. What each one costs is a few pages of touched stack,
   * the rest of its reservation being address space that never becomes a page.
   *
   * The queue is instrumented rather than merely resized, because a pool that
   * is too small is not visible in any of the other metrics on this page.
   */
  constexpr size_t WORKER_THREADS = 128;

  metrics.setWorkerThreads(WORKER_THREADS);

  server.new_task_queue = [&metrics] {
    return new InstrumentedQueue(WORKER_THREADS, metrics);
  };

  /*
   * Timing and counting happen here rather than inside each handler, so a
   * handler added later is instrumented by existing, and the static mount —
   * which has no handler of ours at all — is covered too.
   *
   * The start time is thread_local because httplib runs a request start to
   * finish on one thread, so the pre-routing hook and the logger that follows
   * it are guaranteed to be the same thread for the same request.
   */
  static thread_local std::chrono::steady_clock::time_point requestStarted;

  server.set_pre_routing_handler([&limiter, &clients, &metrics](const httplib::Request& req,
                                                               httplib::Response& res) {
    requestStarted = std::chrono::steady_clock::now();

    /*
     * Every /api/ response says which contract it speaks, so a page whose
     * bundle was built against an older one notices on its first request after
     * a deploy and reloads, instead of reading the answer with the wrong rules.
     *
     * Set here rather than in the handlers so it also reaches the refusals and
     * the unknown-endpoint 404 — a stale page is at least as likely to be
     * asking for something that has since moved as it is to be asking
     * correctly.
     */
    if (req.path.starts_with("/api/"))
      res.set_header("X-Api-Version", std::to_string(API_VERSION));

    if (req.method != "GET" && req.method != "HEAD") {
      res.status = 405;
      res.set_header("Allow", "GET, HEAD");
      res.set_header("Connection", "close");
      res.set_content(errorJson("method not allowed"), "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }

    if (declaresBody(req)) {
      res.status = 400;
      res.set_header("Connection", "close");
      res.set_content(errorJson("request body not allowed"), "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }

    /*
     * Only /api/ is throttled, which is every request that gets this far: the
     * proxy serves the bundle itself and forwards nothing else. The prefix
     * check stays because it is the throttle's own statement of what it covers.
     *
     * The address is resolved rather than read straight off the connection,
     * because the connection comes from the proxy. See ClientAddress.h.
     */
    if (req.path.starts_with("/api/")) {
      /*
       * Grouped before it is counted, because an address and a client are not
       * the same thing on IPv6. What each family narrows to is described where
       * the grouping is defined.
       */
      const auto decision = limiter.check(ratelimitkey::group(clients.of(req)));

      if (!decision.allowed) {
        metrics.observeRateLimited();

        res.status = 429;
        res.set_header("Retry-After",
                       std::to_string(static_cast<int>(std::ceil(decision.retryAfterSeconds))));
        res.set_content(rateLimitJson(decision.retryAfterSeconds), "application/json");
        return httplib::Server::HandlerResponse::Handled;
      }

      /*
       * What the browser counted since it last said anything, riding on a
       * request it was making regardless. Read after the limiter rather than
       * before, so a client being refused cannot go on moving counters, and
       * before routing so it does not matter which endpoint carried it.
       *
       * The parser decides what is acceptable: a length cap, a closed set of
       * keys, decimal integers only, and a ceiling on each. Nothing from here
       * reaches a label.
       */
      if (const std::string flush = req.get_header_value("X-Initium-Client"); !flush.empty()) {
        if (const auto report = clientmetrics::parse(flush))
          metrics.observeClientReport(*report);
        else
          metrics.observeClientReportRejected();
      }
    }

    return httplib::Server::HandlerResponse::Unhandled;
  });

  server.set_logger([&metrics](const httplib::Request& req, const httplib::Response& res) {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - requestStarted).count();

    metrics.observeRequest(classify(req), res.status, seconds);
  });

  /*
   * No CORS headers: the proxy publishes the UI and this API under one origin,
   * and in development Vite proxies /api so the browser never leaves its own.
   */

  server.set_default_headers({{"X-Content-Type-Options", "nosniff"}});

  server.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
        res.status = 500;
        res.set_content(errorJson("internal error"), "application/json");
      });
  server.set_payload_max_length(0);

  server.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
    Json json;
    json.beginObject();
    json.field("status", std::string_view("ok"));
    json.field("apiVersion", API_VERSION);
    json.field("nodes", store->nodeCount());
    json.field("hasEvals", store->hasEvals());
    json.field("buckets", static_cast<uint64_t>(NUM_BUCKETS));
    json.endObject();

    res.set_content(json.take(), "application/json");
  });

  server.Get("/api/position", [&](const httplib::Request& req, httplib::Response& res) {
    res.set_header("Cache-Control", "no-store");

    if (!req.has_param("fen")) {
      res.status = 400;
      res.set_content(errorJson("missing 'fen' query parameter"), "application/json");
      return;
    }

    const std::string fenStr = req.get_param_value("fen");

    if (const auto problem = fen::validate(fenStr)) {
      metrics.observePosition(telemetry::PositionResult::BadFen);
      res.status = 400;
      res.set_content(errorJson("invalid FEN: " + *problem), "application/json");
      return;
    }

    try {
      /* Whether the position was in the tree is a 200 either way, so it is
       * counted here rather than inferred from the status. It is the number
       * that says whether the tree covers what people actually look up. */
      bool found = false;
      std::string body = describePosition(*store, dubious, fenStr, &found);

      metrics.observePosition(found ? telemetry::PositionResult::Found
                                    : telemetry::PositionResult::NotFound);

      res.set_content(std::move(body), "application/json");
    } catch (const std::exception& e) {
      metrics.observePosition(telemetry::PositionResult::Error);
      std::cerr << "request failed for FEN '" << fenStr << "': " << e.what() << "\n";
      res.status = 500;
      res.set_content(errorJson(e.what()), "application/json");
    }
  });

  /*
   * Anything that is neither of the two endpoints above.
   *
   * The proxy in front serves the UI and forwards only /api/, so a request that
   * reaches this far is asking for something that does not exist. Answering in
   * JSON is what makes that legible to the caller: a page handed back instead
   * would surface as a parse error somewhere a long way from the bad URL.
   *
   * Registered last, so it cannot shadow the handlers above.
   */
  server.Get(R"(/.*)", [](const httplib::Request& req, httplib::Response& res) {
    res.status = 404;
    res.set_content(errorJson("no such endpoint: " + req.path), "application/json");
  });

  /*
   * The metrics listener. Separate from the server above so the two can be
   * bound to different addresses.
   *
   * It runs on its own thread because httplib::listen blocks, and it is stopped
   * before the thread is joined so a shutdown of the main server takes this one
   * down with it rather than hanging on exit.
   */
  httplib::Server metricsServer;

  metricsServer.Get("/metrics", [&metrics, &limiter](const httplib::Request&,
                                                     httplib::Response& res) {
    metrics.setRateLimitClients(limiter.trackedClients());

    /* The exposition format is text/plain with an explicit version parameter;
     * Prometheus accepts a bare text/plain but says so in a warning. */
    res.set_content(metrics.scrape(), "text/plain; version=0.0.4; charset=utf-8");
  });

  /* Cheap liveness for anything that wants it without paying for a scrape. */
  metricsServer.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("ok\n", "text/plain");
  });

  std::thread metricsThread([&] {
    if (!metricsServer.listen(options->metricsHost, options->metricsPort)) {
      /* Not fatal. Losing metrics is worth a loud complaint, but it is not a
       * reason to refuse to serve the application. */
      std::cerr << "Warning: metrics listener failed on " << options->metricsHost << ":"
                << options->metricsPort << " -- continuing without it\n";
    }
  });

  metricsServer.wait_until_ready();

  std::cout << "metrics on http://" << options->metricsHost << ":" << options->metricsPort
            << "/metrics" << std::endl;

  /*
   * Said out loud because it decides what the rate limiter counts: with no peer
   * configured and a proxy in front, every visitor lands in one bucket, and the
   * symptom is a long way from the cause.
   */
  if (clients.trustsAnyone()) {
    std::cout << "trusting X-Forwarded-For from:";

    for (const std::string& peer : options->trustForwardedFrom)
      std::cout << ' ' << peer;

    std::cout << std::endl;
  } else {
    std::cout << "not trusting X-Forwarded-For: clients are identified by the connection"
              << std::endl;
  }

  std::cout << "listening on http://" << options->host << ":" << options->port << std::endl;

  const bool ok = server.listen(options->host, options->port);

  metricsServer.stop();
  metricsThread.join();

  if (!ok) {
    std::cerr << "Error: failed to listen on " << options->host << ":" << options->port << "\n";
    return 1;
  }

  return 0;
}
