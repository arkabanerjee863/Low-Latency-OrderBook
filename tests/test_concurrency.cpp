#include <atomic>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include "TestHelpers.hpp"
#include "orderbook/AsyncMatchingEngine.hpp"

using namespace orderbook;
using orderbook::testing::RecordingListener;
using orderbook::testing::requireValid;

namespace {

class LoggingListener : public RecordingListener {
public:
    void onCommandProcessed(const Command& c, const ExecutionResult& r) override {
        commands.push_back(c);
        results.push_back(r);
    }
    std::vector<Command> commands;
    std::vector<ExecutionResult> results;
};

constexpr OrderId kProducerShift = 32;

OrderId makeId(int producer, int seq) {
    return (static_cast<OrderId>(producer + 1) << kProducerShift) | static_cast<OrderId>(seq);
}

int producerOf(OrderId id) { return static_cast<int>(id >> kProducerShift) - 1; }

int produce(AsyncMatchingEngine& engine, int producer, int count) {
    int refused = 0;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(producer + 1);
    auto rnd = [&state](int n) { 
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<int>(state % static_cast<std::uint64_t>(n));
    };
    for (int i = 1; i <= count; ++i) {
        const OrderId id = makeId(producer, i);
        const Side side = rnd(2) == 0 ? Side::Buy : Side::Sell;
        const Price price = 1000 + rnd(21) - 10;
        const Quantity qty = 1 + rnd(50);
        const int kind = rnd(100);
        Command cmd;
        if (kind < 55) cmd = OrderRequest::limit(id, side, price, qty);
        else if (kind < 63) cmd = OrderRequest::market(id, side, qty);
        else if (kind < 71) cmd = OrderRequest::ioc(id, side, price, qty);
        else if (kind < 78) cmd = OrderRequest::stop(id, side, 1000 + rnd(31) - 15, qty);
        else if (kind < 90) {
            const int targetProducer = rnd(4);  
            const int targetSeq = 1 + rnd(i);
            cmd = CancelRequest{makeId(targetProducer, targetSeq)};
        } else {
            const int targetSeq = 1 + rnd(i);
            cmd = ModifyRequest{makeId(producer, targetSeq), price, qty};
        }
        if (!engine.submit(cmd)) ++refused;
    }
    return refused;
}

using Row = std::tuple<OrderId, Price, Quantity>;
std::vector<Row> snapshot(const MatchingEngine& e, Side side) {
    std::vector<Row> rows;
    for (const auto& level : e.book().depth(side))
        for (const auto& o : e.book().ordersAt(side, level.price)) rows.emplace_back(o.id, o.price, o.remaining);
    return rows;
}

}  

TEST_CASE("Concurrent producers: processing is a linearizable total order", "[concurrency]") {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 4000;

    LoggingListener live;
    AsyncMatchingEngine engine(&live, 1 << 16);

    std::atomic<int> refused{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
        producers.emplace_back([&engine, &refused, p] { refused += produce(engine, p, kPerProducer); });
    for (auto& t : producers) t.join();
    engine.sync();
    REQUIRE(refused.load() == 0);

    REQUIRE(engine.processedCount() == static_cast<std::uint64_t>(kProducers * kPerProducer));
    REQUIRE(live.commands.size() == static_cast<std::size_t>(kProducers * kPerProducer));

    std::vector<OrderId> lastSeen(kProducers, 0);
    for (const Command& c : live.commands) {
        if (const auto* o = std::get_if<OrderRequest>(&c)) {
            const int p = producerOf(o->id);
            REQUIRE(o->id > lastSeen[static_cast<std::size_t>(p)]);
            lastSeen[static_cast<std::size_t>(p)] = o->id;
        }
    }

    RecordingListener replayEvents;
    MatchingEngine replay(&replayEvents);
    for (std::size_t i = 0; i < live.commands.size(); ++i) {
        INFO("command " << i);
        REQUIRE(replay.process(live.commands[i]) == live.results[i]);
    }
    REQUIRE(replayEvents.trades == live.trades);
    REQUIRE(replayEvents.triggered == live.triggered);

    engine.inspect([&](const MatchingEngine& e) {
        requireValid(e.book());
        REQUIRE(snapshot(e, Side::Buy) == snapshot(replay, Side::Buy));
        REQUIRE(snapshot(e, Side::Sell) == snapshot(replay, Side::Sell));
        REQUIRE(e.stops().size() == replay.stops().size());
    });

    Quantity filled = 0;
    for (const auto& r : live.results) filled += r.filled;
    for (const auto& r : live.stopResults) filled += r.filled;
    Quantity traded = 0;
    for (const auto& t : live.trades) traded += t.quantity;
    REQUIRE(filled == traded);

    engine.stop();
}

TEST_CASE("inspect() always observes a consistent book while producers run", "[concurrency]") {
    AsyncMatchingEngine engine(nullptr, 1 << 15);
    std::atomic<bool> done{false};
    std::atomic<int> inspections{0};
    std::atomic<int> inconsistencies{0};

    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            engine.inspect([&](const MatchingEngine& e) {
                if (!e.book().validate()) inconsistencies.fetch_add(1);
            });
            inspections.fetch_add(1);
        }
    });
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) producers.emplace_back([&engine, p] { (void)produce(engine, p, 3000); });
    for (auto& t : producers) t.join();
    engine.sync();
    done.store(true, std::memory_order_release);
    reader.join();

    CHECK(inspections.load() > 0);
    CHECK(inconsistencies.load() == 0);
    CHECK(engine.processedCount() == 12000u);
}

TEST_CASE("Concurrent cancels of the same order: exactly one succeeds", "[concurrency]") {
    LoggingListener live;
    AsyncMatchingEngine engine(&live);
    for (OrderId id = 1; id <= 200; ++id) engine.submit(OrderRequest::limit(id, Side::Buy, 100, 1));
    std::vector<std::thread> cancellers;
    for (int t = 0; t < 4; ++t) {
        cancellers.emplace_back([&engine] {
            for (OrderId id = 1; id <= 200; ++id) engine.submit(CancelRequest{id});
        });
    }
    for (auto& t : cancellers) t.join();
    engine.sync();
    int succeeded = 0;
    for (std::size_t i = 0; i < live.commands.size(); ++i) {
        if (std::holds_alternative<CancelRequest>(live.commands[i])) {
            if (live.results[i].accepted()) ++succeeded;
            else REQUIRE(live.results[i].reason == RejectReason::UnknownOrderId);
        }
    }
    CHECK(succeeded == 200);
    engine.inspect([](const MatchingEngine& e) { CHECK(e.book().empty()); });
}

TEST_CASE("Shutdown drains accepted commands; submit after stop is refused", "[concurrency][lifecycle]") {
    LoggingListener live;
    {
        AsyncMatchingEngine engine(&live);
        for (OrderId id = 1; id <= 1000; ++id) REQUIRE(engine.submit(OrderRequest::limit(id, Side::Sell, 100 + static_cast<Price>(id % 10), 1)));
        engine.stop();
        CHECK(engine.processedCount() == 1000u);
        CHECK_FALSE(engine.submit(CancelRequest{1}));
        engine.stop();  
        engine.sync();  
    }
    CHECK(live.commands.size() == 1000);

    LoggingListener viaDestructor;
    {
        AsyncMatchingEngine engine(&viaDestructor);
        for (OrderId id = 1; id <= 500; ++id) engine.submit(OrderRequest::limit(id, Side::Buy, 100, 1));
    }  
    CHECK(viaDestructor.commands.size() == 500);
}

TEST_CASE("Concurrent sync() callers all return", "[concurrency][lifecycle]") {
    AsyncMatchingEngine engine;
    std::vector<std::thread> threads;
    for (int p = 0; p < 4; ++p) {
        threads.emplace_back([&engine, p] {
            for (int round = 0; round < 20; ++round) {
                for (int i = 1; i <= 50; ++i) engine.submit(OrderRequest::market(makeId(p, round * 100 + i), Side::Buy, 1));
                engine.sync();
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK(engine.processedCount() == 4u * 20u * 50u);
}

namespace {
class ThrowingListener : public EngineListener {
public:
    void onTrade(const Trade&) override { throw std::runtime_error("listener failure"); }
};
}  

TEST_CASE("Worker exceptions surface in sync() and stop()", "[concurrency][errors]") {
    ThrowingListener listener;
    AsyncMatchingEngine engine(&listener);
    engine.submit(OrderRequest::limit(1, Side::Buy, 100, 1));
    engine.submit(OrderRequest::limit(2, Side::Sell, 100, 1));  
    CHECK_THROWS_AS(engine.sync(), std::runtime_error);
    CHECK_FALSE(engine.submit(OrderRequest::limit(3, Side::Buy, 90, 1)));
    CHECK_THROWS_AS(engine.stop(), std::runtime_error);
    CHECK_NOTHROW(engine.stop());  // reported once
    engine.inspect([](const MatchingEngine& e) { requireValid(e.book()); });
}
