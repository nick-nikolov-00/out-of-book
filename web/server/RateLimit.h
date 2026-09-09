#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

/*
 * Per-client request throttling.
 *
 * A token bucket, because the traffic it is shaped for is bursty: someone
 * stepping through a game fires a short run of requests and then reads for a
 * while. A generous burst over a modest sustained rate lets that through
 * untouched.
 */
namespace ratelimit {

class RateLimiter {
public:
  struct Config {
    /*
     * Sustained requests per second per client, and the burst that can be spent
     * before that rate binds.
     *
     * Eight per second is far above anything the UI produces: each position is
     * one request and the client caches what it has already seen. The burst
     * matters more than the rate in practice, being what absorbs someone
     * holding an arrow key down to scrub through a line.
     */
    double ratePerSecond = 8.0;
    double burst = 40.0;

    /* How long a silent client is remembered. A bucket idle this long has
     * refilled completely. */
    std::chrono::seconds idleTimeout{300};

    /* Sweeps are periodic rather than per-request; this is how often. */
    std::chrono::seconds sweepInterval{60};

    /* Hard ceiling on remembered clients, so the table cannot grow without
     * bound between sweeps. */
    size_t maxClients = 100000;
  };

  struct Decision {
    bool allowed = true;
    /* Seconds until the next token, when and only when `allowed` is false. */
    double retryAfterSeconds = 0.0;
  };

  /* No default argument: a nested class's member initializers are not usable
   * inside the enclosing class definition, and the policy is worth stating at
   * the call site anyway. */
  explicit RateLimiter(Config config);

  /*
   * Spend one token for `client`, and say whether there was one to spend.
   * Safe to call from any request thread.
   */
  Decision check(const std::string& client);

  /* Clients currently held in the table, for the gauge. */
  size_t trackedClients() const;

  /* Requests refused since construction. */
  uint64_t refusedTotal() const;

private:
  struct Bucket {
    double tokens = 0.0;
    std::chrono::steady_clock::time_point seen{};
  };

  /* Both assume the caller holds the mutex. */
  void sweep(std::chrono::steady_clock::time_point now);
  void evictOldest(size_t target);

  const Config config_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Bucket> buckets_;
  std::chrono::steady_clock::time_point lastSweep_{};
  uint64_t refused_ = 0;
};

} // namespace ratelimit
