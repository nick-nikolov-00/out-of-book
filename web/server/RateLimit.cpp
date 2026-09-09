#include "RateLimit.h"

#include <algorithm>
#include <vector>

namespace ratelimit {

RateLimiter::RateLimiter(Config config)
    : config_(config), lastSweep_(std::chrono::steady_clock::now()) {}

RateLimiter::Decision RateLimiter::check(const std::string& client) {
  const auto now = std::chrono::steady_clock::now();

  const std::lock_guard<std::mutex> lock(mutex_);

  if (now - lastSweep_ >= config_.sweepInterval)
    sweep(now);

  const auto it = buckets_.find(client);

  if (it == buckets_.end()) {
    /*
     * A client seen for the first time starts full, minus the request that
     * introduced it: starting empty would make the first request of every visit
     * a rejection.
     */
    if (buckets_.size() >= config_.maxClients) {
      sweep(now);

      if (buckets_.size() >= config_.maxClients)
        return Decision{true, 0.0};
    }

    buckets_.emplace(client, Bucket{config_.burst - 1.0, now});
    return Decision{true, 0.0};
  }

  Bucket& bucket = it->second;

  /* Refill for the time that passed, capped at the burst. */
  const double elapsed = std::chrono::duration<double>(now - bucket.seen).count();

  bucket.tokens = std::min(config_.burst, bucket.tokens + elapsed * config_.ratePerSecond);
  bucket.seen = now;

  if (bucket.tokens >= 1.0) {
    bucket.tokens -= 1.0;
    return Decision{true, 0.0};
  }

  /*
   * Out of tokens. The wait is until the bucket reaches one token, not until it
   * is full: the client only needs to afford its next request.
   *
   * The refusal costs no token of its own, so retrying does not push the wait
   * further out.
   */
  ++refused_;

  return Decision{false, (1.0 - bucket.tokens) / config_.ratePerSecond};
}

void RateLimiter::sweep(std::chrono::steady_clock::time_point now) {
  lastSweep_ = now;

  for (auto it = buckets_.begin(); it != buckets_.end();) {
    if (now - it->second.seen > config_.idleTimeout)
      it = buckets_.erase(it);
    else
      ++it;
  }

  if (buckets_.size() > config_.maxClients)
    evictOldest(config_.maxClients);
}

void RateLimiter::evictOldest(size_t target) {
  /*
   * Evicting the least recently seen is the least destructive choice available:
   * the entries being dropped are the ones closest to expiring anyway.
   */
  std::vector<std::chrono::steady_clock::time_point> seen;
  seen.reserve(buckets_.size());

  for (const auto& [address, bucket] : buckets_)
    seen.push_back(bucket.seen);

  auto cut = seen.begin() + static_cast<std::ptrdiff_t>(seen.size() - target);
  std::nth_element(seen.begin(), cut, seen.end());

  const auto threshold = *cut;

  for (auto it = buckets_.begin(); it != buckets_.end();) {
    if (it->second.seen < threshold)
      it = buckets_.erase(it);
    else
      ++it;
  }
}

size_t RateLimiter::trackedClients() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return buckets_.size();
}

uint64_t RateLimiter::refusedTotal() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return refused_;
}

} // namespace ratelimit
