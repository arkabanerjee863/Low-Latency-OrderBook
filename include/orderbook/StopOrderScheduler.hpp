#pragma once

// Holds pending stop-market orders and decides when they trigger.

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "orderbook/Types.hpp"

namespace orderbook {

struct StopOrder {
    OrderId id;
    Side side;
    Price stopPrice;
    Quantity quantity;
    std::uint64_t sequence; 
};

class StopOrderScheduler {
public:
    RejectReason add(OrderId id, Side side, Price stopPrice, Quantity quantity);

    std::optional<StopOrder> cancel(OrderId id);

    void collectTriggered(std::optional<Price> bestBid, std::optional<Price> bestAsk,
                          std::vector<StopOrder>& out);

    [[nodiscard]] bool hasTriggered(std::optional<Price> bestBid,
                                    std::optional<Price> bestAsk) const noexcept;

    [[nodiscard]] bool contains(OrderId id) const noexcept { return index_.count(id) != 0; }
    [[nodiscard]] std::optional<StopOrder> find(OrderId id) const;
    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

    [[nodiscard]] std::vector<StopOrder> pending() const;

    void clear() noexcept;

private:
    using Key = std::pair<Price, std::uint64_t>;  

    struct BuyOrder {
        bool operator()(const Key& a, const Key& b) const noexcept { return a < b; }
    };
    struct SellOrder {
        bool operator()(const Key& a, const Key& b) const noexcept {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        }
    };

    std::map<Key, StopOrder, BuyOrder> buyStops_;
    std::map<Key, StopOrder, SellOrder> sellStops_;
    std::unordered_map<OrderId, std::pair<Side, Key>> index_;
    std::uint64_t nextSequence_ = 0;
};

}
