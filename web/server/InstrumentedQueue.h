#pragma once

#include "Telemetry.h"

#include <httplib.h>

#include <chrono>
#include <functional>
#include <utility>

/*
 * httplib hands each accepted connection to one worker thread and that worker
 * holds it for the connection's whole life — keep-alive idle time included,
 * not merely the request. So the pool size is the number of connections being
 * served at once rather than the number of requests.
 *
 * A connection's wait for a worker is invisible to every other metric here: the
 * pre-routing hook that starts the request timer only runs once a worker has
 * taken the connection, so a connection that waited still records a
 * sub-millisecond request and the latency histogram stays flat.
 *
 * This wrapper is where that wait becomes a number. It stamps the time at
 * enqueue and measures it again when a worker actually starts the connection.
 * httplib::ThreadPool is final, so this composes one instead of deriving.
 */
class InstrumentedQueue final : public httplib::TaskQueue {
public:
  InstrumentedQueue(size_t threads, telemetry::Telemetry& metrics)
      : pool_(threads), metrics_(metrics) {}

  bool enqueue(std::function<void()> fn) override {
    const auto queued = std::chrono::steady_clock::now();
    metrics_.observeConnectionQueued();

    const bool accepted = pool_.enqueue([this, queued, fn = std::move(fn)] {
      const double waited =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - queued).count();

      metrics_.observeConnectionStarted(waited);

      /* The decrement has to survive an exception escaping the connection, or
       * the active gauge climbs for the life of the process and reads as
       * saturation that is not there. */
      struct Finish {
        telemetry::Telemetry& metrics;

        ~Finish() {
          metrics.observeConnectionFinished();
        }
      } finish{metrics_};

      fn();
    });

    if (!accepted)
      metrics_.observeConnectionDropped();

    return accepted;
  }

  void shutdown() override {
    pool_.shutdown();
  }

private:
  httplib::ThreadPool pool_;
  telemetry::Telemetry& metrics_;
};
