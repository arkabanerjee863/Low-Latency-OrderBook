// Reproducible micro-benchmarks for MatchingEngine / AsyncMatchingEngine.
// Usage: orderbook_bench [--quick]

// Results depend on CPU, OS, compiler and load. Compare runs on the same machine only.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "orderbook/AsyncMatchingEngine.hpp"
#include "orderbook/MatchingEngine.hpp"

using namespace orderbook;
using Clock = std::chrono::steady_clock;

namespace {

struct Config {
    int warmup = 1;
    int throughputRuns = 3;
    int latencyRuns = 3;
    std::size_t scale = 200'000;  
};

struct Workload {
    std::string name;
    std::string description;
    std::vector<Command> setup;
    std::vector<Command> timed;
    std::vector<Command> afterEach;
};

volatile std::int64_t gSink = 0;

std::int64_t nsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    return sorted[std::min(sorted.size() - 1, rank == 0 ? 0 : rank - 1)];
}

constexpr Price kMid = 100'000;

// Workload generators

Command passive(std::mt19937_64& rng, OrderId id, int maxOffset = 50) {
    std::uniform_int_distribution<int> offset(1, maxOffset), qty(1, 100), side(0, 1);
    const bool buy = side(rng) == 0;
    const Price price = buy ? kMid - offset(rng) : kMid + offset(rng);
    return OrderRequest::limit(id, buy ? Side::Buy : Side::Sell, price, qty(rng));
}

Workload insertPassive(std::size_t n) {
    Workload w{"insert_passive", "add non-crossing limit orders over 100 price levels", {}, {}, {}};
    std::mt19937_64 rng(1);
    for (std::size_t i = 0; i < n; ++i) w.timed.push_back(passive(rng, i + 1));
    return w;
}

Workload cancelRandom(std::size_t n) {
    Workload w{"cancel_random", "cancel resting orders in random order", {}, {}, {}};
    std::mt19937_64 rng(2);
    std::vector<OrderId> ids;
    for (std::size_t i = 0; i < n; ++i) {
        w.setup.push_back(passive(rng, i + 1));
        ids.push_back(i + 1);
    }
    std::shuffle(ids.begin(), ids.end(), rng);
    for (OrderId id : ids) w.timed.push_back(CancelRequest{id});
    return w;
}

Workload modifyMixed(std::size_t n) {
    Workload w{"modify", "50% size-down in place, 50% reprice on own side (no crossing)", {}, {}, {}};
    std::mt19937_64 rng(3);
    std::vector<OrderRequest> live;
    for (std::size_t i = 0; i < n; ++i) {
        const Command c = passive(rng, i + 1);
        w.setup.push_back(c);
        live.push_back(std::get<OrderRequest>(c));
    }
    std::uniform_int_distribution<std::size_t> pick(0, n - 1);
    std::uniform_int_distribution<int> coin(0, 1), offset(1, 50);
    for (std::size_t i = 0; i < n; ++i) {
        OrderRequest& o = live[pick(rng)];
        if (coin(rng) == 0 && o.quantity > 1) {
            o.quantity -= 1;
        } else {
            o.price = o.side == Side::Buy ? kMid - offset(rng) : kMid + offset(rng);
            o.quantity += 1;
        }
        w.timed.push_back(ModifyRequest{o.id, o.price, o.quantity});
    }
    return w;
}

Workload aggressiveMatch(std::size_t n) {
    Workload w{"aggressive_match", "IOC/market orders each filling 1-3 resting orders; book replenished untimed", {}, {}, {}};
    std::mt19937_64 rng(4);
    OrderId id = 1;
    // Resting orders of quantity 10 on 20 levels per side.
    std::uniform_int_distribution<int> offset(1, 20), side(0, 1), fills(1, 3), kind(0, 1);
    for (std::size_t i = 0; i < n / 2; ++i) {
        const bool buy = side(rng) == 0;
        w.setup.push_back(OrderRequest::limit(id++, buy ? Side::Buy : Side::Sell, buy ? kMid - offset(rng) : kMid + offset(rng), 10));
    }
    for (std::size_t i = 0; i < n; ++i) {
        const bool buy = side(rng) == 0;
        const Quantity qty = 10 * fills(rng);
        if (kind(rng) == 0) w.timed.push_back(OrderRequest::market(id++, buy ? Side::Buy : Side::Sell, qty));
        else w.timed.push_back(OrderRequest::ioc(id++, buy ? Side::Buy : Side::Sell, buy ? kMid + 20 : kMid - 20, qty));
        // Replenish the liquidity that was just taken, on the side that was hit.
        const bool restBuy = !buy;
        const Price p = restBuy ? kMid - offset(rng) : kMid + offset(rng);
        w.afterEach.push_back(OrderRequest::limit(id++, restBuy ? Side::Buy : Side::Sell, p, qty));
    }
    return w;
}

Workload mixedFlow(std::size_t n) {
    Workload w{"mixed", "45% add, 30% cancel, 10% modify, 8% IOC, 5% market, 2% stop", {}, {}, {}};
    std::mt19937_64 rng(5);
    OrderId id = 1;
    std::vector<OrderId> passiveIds;
    for (std::size_t i = 0; i < 20'000; ++i) {
        w.setup.push_back(passive(rng, id));
        passiveIds.push_back(id++);
    }
    std::uniform_int_distribution<int> roll(0, 99), offset(1, 10), side(0, 1), qty(1, 100);
    auto recent = [&]() {
        // Cancels and modifies target recently added orders, like real flow does.
        const std::size_t window = std::min<std::size_t>(passiveIds.size(), 5'000);
        std::uniform_int_distribution<std::size_t> pick(passiveIds.size() - window, passiveIds.size() - 1);
        return passiveIds[pick(rng)];
    };
    for (std::size_t i = 0; i < n; ++i) {
        const int r = roll(rng);
        const bool buy = side(rng) == 0;
        const Side s = buy ? Side::Buy : Side::Sell;
        if (r < 45) {
            w.timed.push_back(passive(rng, id, 10));
            passiveIds.push_back(id++);
        } else if (r < 75) {
            w.timed.push_back(CancelRequest{recent()});
        } else if (r < 85) {
            const OrderId target = recent();
            const Price price = buy ? kMid - offset(rng) : kMid + offset(rng);
            w.timed.push_back(ModifyRequest{target, price, qty(rng)});
        } else if (r < 93) {
            w.timed.push_back(OrderRequest::ioc(id++, s, buy ? kMid + 2 : kMid - 2, qty(rng)));
        } else if (r < 98) {
            w.timed.push_back(OrderRequest::market(id++, s, qty(rng)));
        } else {
            const Price stopPrice = buy ? kMid + offset(rng) : kMid - offset(rng);
            w.timed.push_back(OrderRequest::stop(id++, s, stopPrice, qty(rng)));
        }
    }
    return w;
}

Workload burst(std::size_t n) {
    Workload w{"burst", "deep book, then back-to-back marketable orders sweeping it", {}, {}, {}};
    std::mt19937_64 rng(6);
    OrderId id = 1;
    for (std::size_t i = 0; i < n; ++i) w.setup.push_back(passive(rng, id++, 100));
    std::uniform_int_distribution<int> side(0, 1), qty(20, 200);
    for (std::size_t i = 0; i < n / 2; ++i) {
        const bool buy = side(rng) == 0;
        w.timed.push_back(OrderRequest::ioc(id++, buy ? Side::Buy : Side::Sell, buy ? kMid + 100 : kMid - 100, qty(rng)));
    }
    return w;
}

// Runner

struct Result {
    std::string name;
    std::size_t ops = 0;
    double throughputMops = 0;
    std::int64_t p50 = 0, p95 = 0, p99 = 0, p999 = 0, max = 0;
    double rejectedPct = 0;
};

void prepare(MatchingEngine& engine, const Workload& w) {
    for (const auto& c : w.setup) gSink = gSink + engine.process(c).filled;
}

Result runWorkload(const Workload& w, const Config& cfg) {
    Result res;
    res.name = w.name;
    res.ops = w.timed.size();
    const bool hasAfter = !w.afterEach.empty();
    const std::size_t expected = w.setup.size() + w.timed.size() + w.afterEach.size();

    auto once = [&](bool perOp, std::vector<std::int64_t>* samples, std::size_t* rejected) -> std::int64_t {
        MatchingEngine engine(nullptr, expected);
        prepare(engine, w);
        std::int64_t totalNs = 0;
        if (!perOp && !hasAfter) {
            const auto t0 = Clock::now();
            for (const auto& c : w.timed) gSink = gSink + engine.process(c).filled;
            totalNs = nsBetween(t0, Clock::now());
        } else {
            for (std::size_t i = 0; i < w.timed.size(); ++i) {
                const auto t0 = Clock::now();
                const ExecutionResult r = engine.process(w.timed[i]);
                const auto t1 = Clock::now();
                const std::int64_t ns = nsBetween(t0, t1);
                totalNs += ns;
                if (samples) samples->push_back(ns);
                if (rejected && !r.accepted()) ++*rejected;
                gSink = gSink + r.filled;
                if (hasAfter) gSink = gSink + engine.process(w.afterEach[i]).filled;
            }
        }
        return totalNs;
    };

    for (int i = 0; i < cfg.warmup; ++i) once(false, nullptr, nullptr);

    std::int64_t bestNs = INT64_MAX;
    for (int i = 0; i < cfg.throughputRuns; ++i) bestNs = std::min(bestNs, once(false, nullptr, nullptr));
    res.throughputMops = static_cast<double>(res.ops) / (static_cast<double>(bestNs) / 1e9) / 1e6;

    std::vector<std::int64_t> samples;
    samples.reserve(res.ops * static_cast<std::size_t>(cfg.latencyRuns));
    std::size_t rejected = 0;
    for (int i = 0; i < cfg.latencyRuns; ++i) once(true, &samples, i == 0 ? &rejected : nullptr);
    std::sort(samples.begin(), samples.end());
    res.p50 = percentile(samples, 50);
    res.p95 = percentile(samples, 95);
    res.p99 = percentile(samples, 99);
    res.p999 = percentile(samples, 99.9);
    res.max = samples.empty() ? 0 : samples.back();
    res.rejectedPct = 100.0 * static_cast<double>(rejected) / static_cast<double>(res.ops);
    return res;
}

double asyncThroughput(const std::vector<Command>& stream, int producers) {
    AsyncMatchingEngine engine(nullptr, stream.size());
    const std::size_t per = stream.size() / static_cast<std::size_t>(producers);
    const auto t0 = Clock::now();
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            const std::size_t begin = static_cast<std::size_t>(p) * per;
            for (std::size_t i = begin; i < begin + per; ++i) engine.submit(stream[i]);
        });
    }
    for (auto& t : threads) t.join();
    engine.sync();
    const auto ns = nsBetween(t0, Clock::now());
    return static_cast<double>(per * static_cast<std::size_t>(producers)) / (static_cast<double>(ns) / 1e9) / 1e6;
}

std::int64_t clockOverheadNs() {
    std::vector<std::int64_t> v;
    v.reserve(100'000);
    for (int i = 0; i < 100'000; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        v.push_back(nsBetween(a, b));
    }
    std::sort(v.begin(), v.end());
    return percentile(v, 50);
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (argc > 1 && std::strcmp(argv[1], "--quick") == 0) {
        cfg.scale = 20'000;
        cfg.throughputRuns = 1;
        cfg.latencyRuns = 1;
    }

#if defined(__clang__)
    const char* compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
    const char* compiler = "gcc " __VERSION__;
#else
    const char* compiler = "unknown";
#endif
#ifdef NDEBUG
    const char* asserts = "NDEBUG (release)";
#else
    const char* asserts = "assertions enabled (not a release build!)";
#endif

    std::printf("orderbook_bench\n");
    std::printf("  compiler        : %s\n", compiler);
    std::printf("  build           : %s\n", asserts);
    std::printf("  hw threads      : %u\n", std::thread::hardware_concurrency());
    std::printf("  scale           : %zu, warm-up %d, throughput runs %d (best), latency runs %d (pooled)\n",
                cfg.scale, cfg.warmup, cfg.throughputRuns, cfg.latencyRuns);
    std::printf("  clock overhead  : ~%lld ns per steady_clock::now() pair (median, included below)\n\n",
                static_cast<long long>(clockOverheadNs()));

    const std::vector<Workload> workloads = {
        insertPassive(cfg.scale), cancelRandom(cfg.scale), modifyMixed(cfg.scale),
        aggressiveMatch(cfg.scale), mixedFlow(cfg.scale * 5), burst(cfg.scale),
    };

    std::printf("%-17s %9s %10s %8s %8s %8s %9s %9s %7s\n", "workload", "ops", "Mops/s", "p50 ns",
                "p95 ns", "p99 ns", "p99.9 ns", "max ns", "rej %");
    for (const auto& w : workloads) {
        const Result r = runWorkload(w, cfg);
        std::printf("%-17s %9zu %10.2f %8lld %8lld %8lld %9lld %9lld %7.1f\n", r.name.c_str(), r.ops,
                    r.throughputMops, static_cast<long long>(r.p50), static_cast<long long>(r.p95),
                    static_cast<long long>(r.p99), static_cast<long long>(r.p999),
                    static_cast<long long>(r.max), r.rejectedPct);
    }
    std::printf("\nWorkload definitions:\n");
    for (const auto& w : workloads) std::printf("  %-17s %s\n", w.name.c_str(), w.description.c_str());

    std::printf("\nAsyncMatchingEngine end-to-end throughput (mixed stream, submit -> processed):\n");
    const Workload mixed = mixedFlow(cfg.scale * 2);
    std::vector<Command> stream = mixed.setup;
    stream.insert(stream.end(), mixed.timed.begin(), mixed.timed.end());
    for (int producers : {1, 2, 4}) {
        double best = 0;
        for (int i = 0; i < std::max(1, cfg.throughputRuns); ++i) best = std::max(best, asyncThroughput(stream, producers));
        std::printf("  %d producer(s): %.2f Mcommands/s (best of %d)\n", producers, best, std::max(1, cfg.throughputRuns));
    }
    return 0;
}
