// Replays full match, partial fill, market order, no match, cancellation, modification, IOC, stop order on the
// synchronous MatchingEngine, then shows the multi-producer AsyncMatchingEngine.
// Prices are displayed in decimal using a tick size of 0.01.

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "orderbook/AsyncMatchingEngine.hpp"
#include "orderbook/MatchingEngine.hpp"
#include "orderbook/PriceConversion.hpp"

namespace ob = orderbook;

namespace {

constexpr double kTickSize = 0.01;

ob::Price px(double decimal) {
    const auto ticks = ob::toTicks(decimal, kTickSize);
    if (!ticks) throw std::invalid_argument("price not on tick grid");
    return *ticks;
}

std::string fmt(ob::Price ticks) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << ob::toDecimal(ticks, kTickSize);
    return os.str();
}

std::string fmt(const std::optional<ob::Price>& p) { return p ? fmt(*p) : std::string("none"); }

class PrintingListener final : public ob::EngineListener {
public:
    void onTrade(const ob::Trade& t) override {
        std::cout << "  TRADE  aggressor=" << t.aggressorId << " (" << ob::toString(t.aggressorSide)
                  << ") resting=" << t.restingId << " qty=" << t.quantity << " @ " << fmt(t.price) << '\n';
    }
    void onStopTriggered(const ob::StopOrder& s) override {
        std::cout << "  STOP   id=" << s.id << " (" << ob::toString(s.side) << ", stop " << fmt(s.stopPrice)
                  << ") triggered -> released as market order\n";
    }
    void onStopExecuted(const ob::StopOrder& s, const ob::ExecutionResult& r) override {
        std::cout << "  STOP   id=" << s.id << " result: " << ob::toString(r.status) << " filled=" << r.filled
                  << " cancelled=" << r.cancelled << '\n';
    }
};

void printResult(const char* what, const ob::ExecutionResult& r) {
    std::cout << "  " << what << " id=" << r.id << " -> " << ob::toString(r.status);
    if (r.status == ob::OrderStatus::Rejected) std::cout << " (" << ob::toString(r.reason) << ')';
    std::cout << " filled=" << r.filled << " resting=" << r.resting << " cancelled=" << r.cancelled << '\n';
}

void printBook(const ob::MatchingEngine& engine) {
    const ob::OrderBook& book = engine.book();
    std::cout << "  Book:\n";
    for (ob::Side side : {ob::Side::Sell, ob::Side::Buy}) {
        auto levels = book.depth(side);
        if (side == ob::Side::Sell) std::reverse(levels.begin(), levels.end());  // asks top-down
        for (const auto& level : levels) {
            std::cout << "    " << (side == ob::Side::Buy ? "BID " : "ASK ") << fmt(level.price) << "  [";
            bool first = true;
            for (const auto& o : book.ordersAt(side, level.price)) {
                std::cout << (first ? "" : ", ") << "id=" << o.id << " qty=" << o.remaining;
                first = false;
            }
            std::cout << "]\n";
        }
    }
    std::cout << "  Best bid: " << fmt(book.bestBid()) << ", best ask: " << fmt(book.bestAsk())
              << ", pending stops: " << engine.stops().size() << "\n\n";
}

void scenario(const char* title) { std::cout << "=== " << title << " ===\n"; }

void runSynchronousScenarios() {
    PrintingListener listener;
    ob::MatchingEngine engine(&listener);
    using ob::OrderRequest;
    using ob::Side;

    scenario("1. Full match");
    engine.reset();
    printResult("limit buy ", engine.submit(OrderRequest::limit(1, Side::Buy, px(100), 10)));
    printResult("limit sell", engine.submit(OrderRequest::limit(2, Side::Sell, px(100), 10)));
    printBook(engine);

    scenario("2. Partial fill");
    engine.reset();
    printResult("limit buy ", engine.submit(OrderRequest::limit(3, Side::Buy, px(150), 20)));
    printResult("limit sell", engine.submit(OrderRequest::limit(4, Side::Sell, px(150), 10)));
    printBook(engine);

    scenario("3. Market order");
    engine.reset();
    printResult("limit buy  ", engine.submit(OrderRequest::limit(8, Side::Buy, px(150), 10)));
    printResult("market sell", engine.submit(OrderRequest::market(5, Side::Sell, 5)));
    printBook(engine);

    scenario("4. No match");
    engine.reset();
    printResult("limit buy ", engine.submit(OrderRequest::limit(6, Side::Buy, px(80), 5)));
    printResult("limit sell", engine.submit(OrderRequest::limit(7, Side::Sell, px(120), 5)));
    printBook(engine);

    scenario("5. Cancellation");
    engine.reset();
    printResult("limit buy ", engine.submit(OrderRequest::limit(10, Side::Buy, px(110), 10)));
    printResult("cancel    ", engine.cancel(10));
    printResult("cancel again", engine.cancel(10));
    printBook(engine);

    scenario("6. Modification (price change loses priority)");
    engine.reset();
    printResult("limit sell", engine.submit(OrderRequest::limit(11, Side::Sell, px(130), 10)));
    printResult("modify    ", engine.modify(11, px(125), 15));
    printBook(engine);

    scenario("7. IOC with no liquidity (residual cancelled)");
    engine.reset();
    printResult("ioc sell  ", engine.submit(OrderRequest::ioc(20, Side::Sell, px(100), 5)));
    printBook(engine);

    scenario("8. Stop order (buy stop 150 triggers when best ask >= 150)");
    engine.reset();
    printResult("stop buy  ", engine.submit(OrderRequest::stop(30, Side::Buy, px(150), 10)));
    printResult("limit sell", engine.submit(OrderRequest::limit(31, Side::Sell, px(155), 10)));
    printBook(engine);

    scenario("9. Invalid input");
    engine.reset();
    printResult("zero qty  ", engine.submit(OrderRequest::limit(40, Side::Buy, px(100), 0)));
    printResult("zero price", engine.submit(OrderRequest::limit(41, Side::Buy, 0, 10)));
    printResult("limit buy ", engine.submit(OrderRequest::limit(42, Side::Buy, px(100), 10)));
    printResult("dup id    ", engine.submit(OrderRequest::limit(42, Side::Buy, px(101), 10)));
    printBook(engine);
}


class CountingListener final : public ob::EngineListener {
public:
    void onTrade(const ob::Trade& t) override {
        ++trades;
        volume += t.quantity;
    }
    std::uint64_t trades = 0;  
    ob::Quantity volume = 0;
};

void runAsyncScenario() {
    scenario("10. AsyncMatchingEngine: 4 producers x 5000 orders");
    CountingListener listener;
    ob::AsyncMatchingEngine engine(&listener, 1 << 16);

    constexpr int kProducers = 4;
    constexpr int kOrdersPerProducer = 5000;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&engine, p] {
            for (int i = 0; i < kOrdersPerProducer; ++i) {
                const auto id = static_cast<ob::OrderId>(p * kOrdersPerProducer + i + 1);
                const ob::Side side = (i % 2 == 0) ? ob::Side::Buy : ob::Side::Sell;
                const ob::Price price = 10'000 + ((i * 7 + p * 13) % 41) - 20;
                engine.submit(ob::OrderRequest::limit(id, side, price, 1 + (i % 10)));
            }
        });
    }
    for (auto& t : producers) t.join();
    engine.sync();
    engine.stop();  

    engine.inspect([&](const ob::MatchingEngine& e) {
        std::string error;
        const bool ok = e.book().validate(&error);
        std::cout << "  processed=" << engine.processedCount() << " trades=" << listener.trades
                  << " volume=" << listener.volume << " resting=" << e.book().orderCount()
                  << " bid=" << fmt(e.book().bestBid()) << " ask=" << fmt(e.book().bestAsk())
                  << " invariants=" << (ok ? "ok" : error) << "\n";
    });
}

} 

int main() {
    try {
        runSynchronousScenarios();
        runAsyncScenario();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
