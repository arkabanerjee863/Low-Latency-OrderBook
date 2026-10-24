#pragma once

// Single-threaded limit order book with price-time priority.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "orderbook/Types.hpp"

namespace orderbook {

class OrderBook {
public:
    explicit OrderBook(std::size_t expectedOrders = 0);

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;
    ~OrderBook() = default;

    ExecutionResult submit(const OrderRequest& request, std::vector<Trade>& trades);

    ExecutionResult cancel(OrderId id);

    ExecutionResult modify(OrderId id, Price newPrice, Quantity newQuantity,
                           std::vector<Trade>& trades);

    void clear() noexcept;


    [[nodiscard]] std::optional<Price> bestBid() const noexcept;
    [[nodiscard]] std::optional<Price> bestAsk() const noexcept;
    [[nodiscard]] bool contains(OrderId id) const noexcept { return index_.count(id) != 0; }
    [[nodiscard]] std::optional<OrderInfo> find(OrderId id) const;
    [[nodiscard]] std::size_t orderCount() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t levelCount(Side side) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

    [[nodiscard]] Quantity quantityAt(Side side, Price price) const noexcept;

    [[nodiscard]] std::vector<LevelInfo> depth(Side side, std::size_t maxLevels = SIZE_MAX) const;

    [[nodiscard]] std::vector<OrderInfo> ordersAt(Side side, Price price) const;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;

private:
    using Slot = std::uint32_t;
    static constexpr Slot kNil = UINT32_MAX;

    struct Level {
        Slot head = kNil;
        Slot tail = kNil;
        Quantity totalQuantity = 0;
        std::uint32_t orderCount = 0;
    };

    struct Node {
        OrderId id = 0;
        Price price = 0;
        Quantity remaining = 0;
        Level* level = nullptr;
        Slot prev = kNil;
        Slot next = kNil;
        Side side = Side::Buy;
    };

    using BidLevels = std::map<Price, Level, std::greater<Price>>;
    using AskLevels = std::map<Price, Level, std::less<Price>>;

    template <class Levels>
    Quantity matchAgainst(Levels& opposite, OrderId aggressorId, Side aggressorSide,
                          Quantity quantity, const std::optional<Price>& limit,
                          std::vector<Trade>& trades);

    Quantity match(Side aggressorSide, OrderId aggressorId, Quantity quantity,
                   const std::optional<Price>& limit, std::vector<Trade>& trades);

    template <class Levels>
    void rest(Levels& levels, OrderId id, Side side, Price price, Quantity quantity);

    void restOrder(OrderId id, Side side, Price price, Quantity quantity);

    void removeNode(Slot slot);

    Slot allocateNode();
    void releaseNode(Slot slot) noexcept;
    void unlink(Slot slot) noexcept;

    template <class Levels>
    bool validateSide(const Levels& levels, Side side, std::size_t& seen, std::string* error) const;

    BidLevels bids_;
    AskLevels asks_;
    std::vector<Node> nodes_;
    std::vector<Slot> freeSlots_;
    std::unordered_map<OrderId, Slot> index_;
};

}
