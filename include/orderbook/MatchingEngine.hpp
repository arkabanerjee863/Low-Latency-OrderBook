#pragma once

// Single-threaded matching engine: the order book plus stop-order handling.

// This is the one place where order-book state and stop-order state are
// combined, so every command sees and leaves a consistent pair of states.

#include <cstddef>
#include <vector>

#include "orderbook/OrderBook.hpp"
#include "orderbook/StopOrderScheduler.hpp"
#include "orderbook/Types.hpp"

namespace orderbook {

class EngineListener {
public:
    virtual ~EngineListener() = default;
    virtual void onTrade(const Trade& /*trade*/) {}
    virtual void onStopTriggered(const StopOrder& /*stop*/) {}
    virtual void onStopExecuted(const StopOrder& /*stop*/, const ExecutionResult& /*result*/) {}
    virtual void onCommandProcessed(const Command& /*command*/, const ExecutionResult& /*result*/) {}
};

class MatchingEngine {
public:
    explicit MatchingEngine(EngineListener* listener = nullptr, std::size_t expectedOrders = 0);

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    ExecutionResult submit(const OrderRequest& request);
    ExecutionResult cancel(OrderId id);
    ExecutionResult modify(OrderId id, Price newPrice, Quantity newQuantity);
    ExecutionResult process(const Command& command);

    void reset() noexcept;

    void setListener(EngineListener* listener) noexcept { listener_ = listener; }

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const StopOrderScheduler& stops() const noexcept { return stops_; }

    [[nodiscard]] bool isLive(OrderId id) const noexcept {
        return book_.contains(id) || stops_.contains(id);
    }

private:
    void publishTrades();
    void runStopCascade();

    OrderBook book_;
    StopOrderScheduler stops_;
    EngineListener* listener_;
    std::vector<Trade> trades_;
    std::vector<StopOrder> triggered_;
};

}  
