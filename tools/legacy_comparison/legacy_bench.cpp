#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#include "OrderBook.hpp" 
#include "orderbook/MatchingEngine.hpp"

namespace {

using Clk = std::chrono::steady_clock;

long long nsBetween(Clk::time_point a, Clk::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

struct Op {
    int id;
    bool buy;
    int price;
    int qty;
};

void report(const char* who, const char* what, std::vector<long long>& v) {
    std::sort(v.begin(), v.end());
    auto pct = [&](double q) {
        const auto rank = static_cast<std::size_t>(std::ceil(q / 100.0 * static_cast<double>(v.size())));
        return v[std::min(v.size() - 1, rank - 1)];
    };
    long long total = 0;
    for (long long x : v) total += x;
    std::fprintf(stderr, "%-11s %-10s n=%zu  mean=%lld  p50=%lld  p99=%lld  p99.9=%lld ns\n", who, what, v.size(),
                 total / static_cast<long long>(v.size()), pct(50), pct(99), pct(99.9));
}

}  // namespace

int main(int argc, char** argv) {
    const bool quiet = argc > 1 && std::strcmp(argv[1], "quiet") == 0;
    if (quiet) std::cout.setstate(std::ios::failbit);

    constexpr int N = 100000;
    std::mt19937_64 rng(1);
    std::uniform_int_distribution<int> off(1, 50), q(1, 100), sd(0, 1);
    std::vector<Op> ops;
    for (int i = 0; i < N; ++i) {
        const bool buy = sd(rng) == 0;
        const int price = buy ? 100000 - off(rng) : 100000 + off(rng);
        ops.push_back({i + 1, buy, price, q(rng)});
    }
    std::vector<int> cancelOrder(N);
    std::iota(cancelOrder.begin(), cancelOrder.end(), 1);
    std::shuffle(cancelOrder.begin(), cancelOrder.end(), rng);
    std::vector<Op> iocs;
    for (int i = 0; i < N / 2; ++i) {
        const bool buy = sd(rng) == 0;
        iocs.push_back({N + i + 1, buy, buy ? 100050 : 99950, q(rng)});
    }

    for (int rep = 0; rep < 2; ++rep) {
        std::vector<long long> a, c, m, a2, c2, m2;
        {
            OrderBook ob;
            for (const auto& o : ops) {
                const auto t0 = Clk::now();
                ob.addOrder(o.id, o.price, o.qty, o.buy ? "buy" : "sell", OrderType::Limit);
                a.push_back(nsBetween(t0, Clk::now()));
            }
            for (int id : cancelOrder) {
                const auto t0 = Clk::now();
                ob.cancelOrder(id);
                c.push_back(nsBetween(t0, Clk::now()));
            }
            for (const auto& o : ops) ob.addOrder(o.id, o.price, o.qty, o.buy ? "buy" : "sell", OrderType::Limit);
            for (const auto& o : iocs) {
                const auto t0 = Clk::now();
                ob.addOrder(o.id, o.price, o.qty, o.buy ? "buy" : "sell", OrderType::IOC);
                m.push_back(nsBetween(t0, Clk::now()));
            }
        }
        {
            using namespace orderbook;
            MatchingEngine e(nullptr, N);
            auto side = [](const Op& o) { return o.buy ? Side::Buy : Side::Sell; };
            for (const auto& o : ops) {
                const auto t0 = Clk::now();
                e.submit(OrderRequest::limit(static_cast<OrderId>(o.id), side(o), o.price, o.qty));
                a2.push_back(nsBetween(t0, Clk::now()));
            }
            for (int id : cancelOrder) {
                const auto t0 = Clk::now();
                e.cancel(static_cast<OrderId>(id));
                c2.push_back(nsBetween(t0, Clk::now()));
            }
            for (const auto& o : ops) e.submit(OrderRequest::limit(static_cast<OrderId>(o.id), side(o), o.price, o.qty));
            for (const auto& o : iocs) {
                const auto t0 = Clk::now();
                e.submit(OrderRequest::ioc(static_cast<OrderId>(o.id), side(o), o.price, o.qty));
                m2.push_back(nsBetween(t0, Clk::now()));
            }
        }
        if (rep == 1) {
            const char* tag = quiet ? "original(q)" : "original";
            report(tag, "insert", a);
            report(tag, "cancel", c);
            report(tag, "ioc_match", m);
            report("refactored", "insert", a2);
            report("refactored", "cancel", c2);
            report("refactored", "ioc_match", m2);
        }
    }
    return 0;
}
