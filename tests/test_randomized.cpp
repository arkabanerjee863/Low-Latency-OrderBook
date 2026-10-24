#include <random>
#include <tuple>
#include <vector>

#include "ReferenceEngine.hpp"
#include "TestHelpers.hpp"

using namespace orderbook;
using orderbook::testing::RecordingListener;
using orderbook::testing::ReferenceEngine;

namespace {

using Row = std::tuple<OrderId, Price, Quantity>;

std::vector<Row> engineSide(const MatchingEngine& e, Side side) {
    std::vector<Row> rows;
    for (const auto& level : e.book().depth(side)) {
        Quantity total = 0;
        for (const auto& o : e.book().ordersAt(side, level.price)) {
            rows.emplace_back(o.id, o.price, o.remaining);
            total += o.remaining;
        }
        REQUIRE(total == level.totalQuantity);
    }
    return rows;
}

std::vector<Row> referenceSide(const ReferenceEngine& r, Side side) {
    std::vector<Row> rows;
    for (const auto& o : r.sorted(side)) rows.emplace_back(o.id, o.price, o.qty);
    return rows;
}

struct Generator {
    explicit Generator(std::uint64_t seed) : rng(seed) {}

    std::mt19937_64 rng;
    OrderId nextId = 1;
    std::vector<OrderId> issued;  

    int uniform(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

    OrderId existingOrNewId() {
        if (!issued.empty() && uniform(0, 99) < 90) return issued[static_cast<std::size_t>(uniform(0, static_cast<int>(issued.size()) - 1))];
        return 1'000'000 + static_cast<OrderId>(uniform(0, 1000));  
    }

    Command next() {
        const int roll = uniform(0, 99);
        const Side side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
        const Price price = 95 + uniform(0, 10);         
        Quantity qty = uniform(1, 20);
        if (uniform(0, 99) < 2) qty = uniform(-2, 0);    
        Price limitPrice = price;
        if (uniform(0, 99) < 2) limitPrice = uniform(-1, 0); 

        auto newId = [&] {
            if (!issued.empty() && uniform(0, 99) < 5) return existingOrNewId();
            const OrderId id = nextId++;
            issued.push_back(id);
            return id;
        };

        if (roll < 40) {
            const OrderId id = newId();
            return OrderRequest::limit(id, side, limitPrice, qty);
        }
        if (roll < 48) {
            const OrderId id = newId();
            return OrderRequest::market(id, side, qty);
        }
        if (roll < 56) {
            const OrderId id = newId();
            return OrderRequest::ioc(id, side, limitPrice, qty);
        }
        if (roll < 66) {
            const OrderId id = newId();
            const Price stopPrice = 93 + uniform(0, 14);
            return OrderRequest::stop(id, side, stopPrice, qty);
        }
        const OrderId target = existingOrNewId();
        if (roll < 84) return CancelRequest{target};
        return ModifyRequest{target, limitPrice, qty};
    }
};

void runDifferential(std::uint64_t seed, int steps) {
    RecordingListener listener;
    MatchingEngine engine(&listener);
    ReferenceEngine reference;
    Generator gen(seed);

    for (int step = 0; step < steps; ++step) {
        const Command cmd = gen.next();
        const std::size_t engineTradesBefore = listener.trades.size();
        const std::size_t refTradesBefore = reference.trades.size();

        const ExecutionResult got = engine.process(cmd);
        const ExecutionResult want = reference.process(cmd);

        INFO("seed=" << seed << " step=" << step << " command index=" << cmd.index());
        REQUIRE(got == want);

        const std::vector<Trade> gotTrades(listener.trades.begin() + static_cast<std::ptrdiff_t>(engineTradesBefore),
                                           listener.trades.end());
        const std::vector<Trade> wantTrades(reference.trades.begin() + static_cast<std::ptrdiff_t>(refTradesBefore),
                                            reference.trades.end());
        REQUIRE(gotTrades == wantTrades);

        REQUIRE(engine.book().bestBid() == reference.best(Side::Buy));
        REQUIRE(engine.book().bestAsk() == reference.best(Side::Sell));
        REQUIRE(engineSide(engine, Side::Buy) == referenceSide(reference, Side::Buy));
        REQUIRE(engineSide(engine, Side::Sell) == referenceSide(reference, Side::Sell));

        const auto gotStops = engine.stops().pending();
        const auto wantStops = reference.pendingStops();
        REQUIRE(gotStops.size() == wantStops.size());
        for (std::size_t i = 0; i < gotStops.size(); ++i) {
            REQUIRE(gotStops[i].id == wantStops[i].id);
            REQUIRE(gotStops[i].side == wantStops[i].side);
            REQUIRE(gotStops[i].stopPrice == wantStops[i].stop);
            REQUIRE(gotStops[i].quantity == wantStops[i].qty);
        }

        std::string error;
        const bool valid = engine.book().validate(&error);
        INFO(error);
        REQUIRE(valid);
    }
}

}  

TEST_CASE("Randomized differential test against reference model", "[random][reference]") {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) runDifferential(seed, 3000);
}

TEST_CASE("Randomized long run against reference model", "[random][reference][long]") {
    runDifferential(0xC0FFEE, 30000);
}
