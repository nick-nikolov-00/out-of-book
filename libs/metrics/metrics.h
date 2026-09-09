#pragma once

#include <atomic>
#include <functional>
#include <mutex>

namespace metrics {

namespace detail {

template <typename T, typename... Ts> constexpr bool contains_v = (std::is_same_v<T, Ts> || ...);

template <typename... Ts> struct all_unique : std::true_type {};

template <typename T, typename... Ts>
struct all_unique<T, Ts...>
    : std::bool_constant<!contains_v<T, Ts...> && all_unique<Ts...>::value> {};

template <typename... Ts> inline constexpr bool all_unique_v = all_unique<Ts...>::value;

template <typename T> struct alignas(std::hardware_destructive_interference_size) Cell {
  std::atomic<T> val;
};

} // namespace detail

template <typename T> struct Counter {
  static_assert(std::is_integral<T>::value, "T must be an integral type");
  static_assert(std::atomic<T>::is_always_lock_free, "atomic<T> must be lock-free");

  using value_type = T;
};

template <typename... Counters> class Metrics;

template <typename... Counters> class Snapshot {
  using Values = std::tuple<typename Counters::value_type...>;
  using Metrics = Metrics<Counters...>;

public:
  friend Metrics;

  template <typename T> auto get() {
    return std::get<Metrics::template indexOf<T>()>(currentValues);
  }

  template <typename T> double ratePerSecond() {
    auto diff = std::get<Metrics::template indexOf<T>()>(currentValues) -
                std::get<Metrics::template indexOf<T>()>(prevValues);
    return diff / secondsBetweenUpdates;
  }

  auto getRuntime() {
    return timeSinceStart;
  }

private:
  Snapshot(Values prevValues, Values currentValues, double secondsBetweenUpdates,
           Metrics::Clock::duration timeSinceStart)
      : prevValues(prevValues), currentValues(currentValues),
        secondsBetweenUpdates(secondsBetweenUpdates), timeSinceStart(timeSinceStart) {}

  Values prevValues;
  Values currentValues;
  double secondsBetweenUpdates;
  Metrics::Clock::duration timeSinceStart;
};

template <typename... Counters> class Metrics {
  static_assert((std::is_base_of_v<Counter<typename Counters::value_type>, Counters> && ...),
                "Counters must be derived from metrics::Counter");

  static_assert(detail::all_unique_v<Counters...>, "Counters must not contain duplicates");

  static constexpr std::size_t N = sizeof...(Counters);

public:
  using Clock = std::chrono::steady_clock;
  using Snapshot = Snapshot<Counters...>;

  friend Snapshot;

  Metrics(Clock::duration printDuration, std::function<void(Snapshot)> printCallback,
          std::function<void(Snapshot)> endCallback)
      : printDuration(printDuration), printCallback(printCallback), endCallback(endCallback),
        startTime(Clock::now()) {}

  ~Metrics() {
    auto now = Clock::now();
    Values currentValues = getCurrentValues(std::make_index_sequence<N>());

    Snapshot snapshot{prevValues, currentValues,
                      std::chrono::duration<double>(now - prevPrintTimepoint.load(std::memory_order::relaxed)).count(),
                      now - startTime};

    endCallback(snapshot);
  }

  template <typename T> [[nodiscard]] std::atomic<typename T::value_type>& get() {
    return std::get<indexOf<T>()>(counters).val;
  }

  void maybePrint() {
    auto now = Clock::now();

    if (now - prevPrintTimepoint.load(std::memory_order::relaxed) < printDuration)
      return;

    std::scoped_lock lock(printMutex);

    auto prevPrintTimepointNonAtomic = prevPrintTimepoint.load(std::memory_order::relaxed);
    if (now - prevPrintTimepointNonAtomic  < printDuration)
      return;

    now = Clock::now();
    Values currentValues = getCurrentValues(std::make_index_sequence<N>());

    Snapshot snapshot{prevValues, currentValues,
                      std::chrono::duration<double>(now - prevPrintTimepointNonAtomic).count(),
                      now - startTime};

    prevPrintTimepoint.store(now, std::memory_order::relaxed);

    printCallback(snapshot);

    prevValues = currentValues;
  }

  template <typename T> auto& getCell() {
    return std::get<indexOf<T>()>(counters);
  }

private:
  using Values = std::tuple<typename Counters::value_type...>;

  template <typename T> static constexpr std::size_t indexOf() {
    static_assert((std::size_t(std::is_same_v<T, Counters>) + ... + 0) == 1);
    constexpr bool matches[] = {std::is_same_v<T, Counters>...};

    for (std::size_t i = 0; i < N; ++i)
      if (matches[i])
        return i;

    return N;
  }

  template <size_t... Idx> Values getCurrentValues(std::index_sequence<Idx...>) {
    return {std::get<Idx>(counters).val.load(std::memory_order::relaxed)...};
  }

  std::tuple<detail::Cell<typename Counters::value_type>...> counters;
  Values prevValues{};

  Clock::duration printDuration;
  std::mutex printMutex;
  std::function<void(const Snapshot&)> printCallback;
  std::function<void(const Snapshot&)> endCallback;

  Clock::time_point startTime{};
  std::atomic<Clock::time_point> prevPrintTimepoint{};

  static_assert(decltype(prevPrintTimepoint)::is_always_lock_free);
};

template <typename... Counters> class DerivedMetrics {
public:
  static constexpr std::size_t N = sizeof...(Counters);

  template <typename... SupersetCounters>
  DerivedMetrics(Metrics<SupersetCounters...>& metrics)
      : counters({metrics.template getCell<Counters>()...}) {}

  template <typename T> std::atomic<typename T::value_type>& get() {
    return std::get<indexOf<T>()>(counters).val;
  }

private:
  template <typename T> static constexpr std::size_t indexOf() {
    static_assert((std::size_t(std::is_same_v<T, Counters>) + ... + 0) == 1);
    constexpr bool matches[] = {std::is_same_v<T, Counters>...};

    for (std::size_t i = 0; i < N; ++i)
      if (matches[i])
        return i;

    return N;
  }

  std::tuple<detail::Cell<typename Counters::value_type>&...> counters;
};

} // namespace metrics