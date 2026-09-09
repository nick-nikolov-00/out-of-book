include(FetchContent)

FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.49.0
)
FetchContent_MakeAvailable(httplib)

# Core only. prometheus-cpp ships its own civetweb server for /metrics, but
# httplib is already here and serves that page instead, so ENABLE_PULL=OFF
# keeps a second HTTP library out of the build. ENABLE_PUSH would pull in
# libcurl for a Pushgateway nothing here uses.
set(ENABLE_PUSH OFF CACHE BOOL "" FORCE)
set(ENABLE_PULL OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        prometheus_cpp
        GIT_REPOSITORY https://github.com/jupp0r/prometheus-cpp.git
        GIT_TAG v1.3.0
        # The repository carries civetweb and googletest as submodules, for
        # the pull exposer and the tests, both switched off above. Skipping
        # them is not just faster: cloning them is a second network dependency
        # that can fail the configure step of a build with no use for either.
        GIT_SUBMODULES ""
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(prometheus_cpp)

# The top-level build turns on -ffast-math, and prometheus-cpp cannot survive
# it. Its text serializer decides how to print a bucket boundary with
# std::isinf(), and closes the histogram with `last != infinity()`; under
# -ffinite-math-only the compiler may assume neither can be true, so both fold
# away. The exposition then carries a junk `le="inf"` bucket and a duplicate
# `+Inf`, and Prometheus sees the same series twice for every histogram.
#
# The library wants IEEE semantics, so it gets them; target options are
# appended after directory options, so this wins. The target is named `core`
# inside prometheus-cpp, and prometheus-cpp::core is an ALIAS, which
# target_compile_options will not take. Guarded so a rename upstream leaves a
# build that is merely un-patched rather than one that refuses to configure.
if (TARGET core)
    target_compile_options(core PRIVATE -fno-fast-math)
else ()
    message(WARNING "prometheus-cpp target `core` not found: -ffast-math will "
            "corrupt histogram +Inf buckets in the /metrics output")
endif ()

add_executable(initium_server main.cpp Analysis.cpp PositionJson.cpp SelfCheck.cpp
        Options.cpp OtbStore.cpp Telemetry.cpp RateLimit.cpp RateLimitKey.cpp
        ClientMetrics.cpp
        ClientAddress.cpp DubiousMoves.cpp San.cpp)

target_link_libraries(initium_server PRIVATE stockfish common httplib::httplib
        prometheus-cpp::core)

# How many connections the kernel will hold for us between accept() calls.
# httplib passes a literal 5 to listen(), which is small enough that a handful
# of browsers arriving together can have their SYNs dropped and wait a second
# for the retransmit. There is no runtime setter, so it is a compile-time
# definition -- and only reachable at all because HTTPLIB_COMPILE is off and the
# library is header-only, so this translation unit's definition is the one that
# builds listen_internal().
target_compile_definitions(initium_server PRIVATE CPPHTTPLIB_LISTEN_BACKLOG=1024)

find_package(Threads REQUIRED)
target_link_libraries(initium_server PRIVATE Threads::Threads)
