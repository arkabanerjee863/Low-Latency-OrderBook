#include "orderbook/OrderBook.hpp"

#include <algorithm>
#include <stdexcept>

namespace orderbook {

OrderBook::OrderBook(std::size_t expectedOrders) {
    if (expectedOrders > 0) {
        nodes_.reserve(expectedOrders);
        freeSlots_.reserve(expectedOrders);
        index_.reserve(expectedOrders);
    }
}

// Node pool

OrderBook::Slot OrderBook::allocateNode() {
    if (!freeSlots_.empty()) {
        const Slot slot = freeSlots_.back();
        freeSlots_.pop_back();
        return slot;
    }
    if (nodes_.size() >= static_cast<std::size_t>(kNil)) {
        throw std::length_error("OrderBook: too many live orders");
    }
    nodes_.emplace_back();  
    return static_cast<Slot>(nodes_.size() - 1);
}

void OrderBook::releaseNode(Slot slot) noexcept {
    nodes_[slot] = Node{};
    try {
        freeSlots_.push_back(slot);
    } catch (...) {
    }
}

void OrderBook::unlink(Slot slot) noexcept {
    Node& node = nodes_[slot];
    Level& level = *node.level;
    if (node.prev != kNil) nodes_[node.prev].next = node.next; else level.head = node.next;
    if (node.next != kNil) nodes_[node.next].prev = node.prev; else level.tail = node.prev;
    level.totalQuantity -= node.remaining;
    --level.orderCount;
    node.prev = node.next = kNil;
}

void OrderBook::removeNode(Slot slot) {
    Node& node = nodes_[slot];
    const Side side = node.side;
    const Price price = node.price;
    const bool levelEmpties = node.level->orderCount == 1;
    unlink(slot);
    index_.erase(node.id);
    releaseNode(slot);
    if (levelEmpties) {
        if (side == Side::Buy) bids_.erase(price); else asks_.erase(price);
    }
}

// Matching

template <class Levels>
Quantity OrderBook::matchAgainst(Levels& opposite, OrderId aggressorId, Side aggressorSide,
                                 Quantity quantity, const std::optional<Price>& limit,
                                 std::vector<Trade>& trades) {
    const auto beyondLimit = opposite.key_comp();
    while (quantity > 0 && !opposite.empty()) {
        auto levelIt = opposite.begin();
        if (limit && beyondLimit(*limit, levelIt->first)) break;

        Level& level = levelIt->second;
        while (quantity > 0 && level.head != kNil) {
            const Slot slot = level.head;
            Node& resting = nodes_[slot];
            const Quantity fill = std::min(quantity, resting.remaining);
            trades.push_back(Trade{aggressorId, resting.id, aggressorSide, resting.price, fill});
            quantity -= fill;
            resting.remaining -= fill;
            level.totalQuantity -= fill;
            if (resting.remaining == 0) {
                level.head = resting.next;
                if (level.head != kNil) nodes_[level.head].prev = kNil; else level.tail = kNil;
                --level.orderCount;
                index_.erase(resting.id);
                releaseNode(slot);
            }
        }
        if (level.head == kNil) opposite.erase(levelIt);
    }
    return quantity;
}

Quantity OrderBook::match(Side aggressorSide, OrderId aggressorId, Quantity quantity,
                          const std::optional<Price>& limit, std::vector<Trade>& trades) {
    return aggressorSide == Side::Buy
               ? matchAgainst(asks_, aggressorId, aggressorSide, quantity, limit, trades)
               : matchAgainst(bids_, aggressorId, aggressorSide, quantity, limit, trades);
}

template <class Levels>
void OrderBook::rest(Levels& levels, OrderId id, Side side, Price price, Quantity quantity) {
    auto [levelIt, insertedLevel] = levels.try_emplace(price);
    Slot slot = kNil;
    try {
        slot = allocateNode();
        index_.emplace(id, slot);
    } catch (...) {
        if (slot != kNil) releaseNode(slot);
        if (insertedLevel) levels.erase(levelIt);
        throw;
    }
    Level& level = levelIt->second;
    Node& node = nodes_[slot];
    node.id = id;
    node.price = price;
    node.remaining = quantity;
    node.side = side;
    node.level = &level;
    node.prev = level.tail;
    node.next = kNil;
    if (level.tail != kNil) nodes_[level.tail].next = slot; else level.head = slot;
    level.tail = slot;
    level.totalQuantity += quantity;
    ++level.orderCount;
}

void OrderBook::restOrder(OrderId id, Side side, Price price, Quantity quantity) {
    if (side == Side::Buy) rest(bids_, id, side, price, quantity);
    else rest(asks_, id, side, price, quantity);
}

// Public mutations

ExecutionResult OrderBook::submit(const OrderRequest& request, std::vector<Trade>& trades) {
    const OrderId id = request.id;
    if (request.type == OrderType::Stop) return ExecutionResult::reject(id, RejectReason::UnsupportedOrderType);
    if (request.quantity <= 0) return ExecutionResult::reject(id, RejectReason::InvalidQuantity);
    if (request.type != OrderType::Market && request.price <= 0) {
        return ExecutionResult::reject(id, RejectReason::InvalidPrice);
    }
    if (contains(id)) return ExecutionResult::reject(id, RejectReason::DuplicateOrderId);

    std::optional<Price> limit;
    if (request.type != OrderType::Market) limit = request.price;

    const Quantity remaining = match(request.side, id, request.quantity, limit, trades);
    ExecutionResult result{OrderStatus::Filled, RejectReason::None, id, request.quantity - remaining, 0, 0};
    if (remaining == 0) return result;

    if (request.type == OrderType::Limit) {
        restOrder(id, request.side, request.price, remaining);
        result.status = OrderStatus::Resting;
        result.resting = remaining;
    } else {
        result.status = OrderStatus::Cancelled;
        result.cancelled = remaining;
    }
    return result;
}

ExecutionResult OrderBook::cancel(OrderId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) return ExecutionResult::reject(id, RejectReason::UnknownOrderId);
    const Quantity removed = nodes_[it->second].remaining;
    removeNode(it->second);
    return {OrderStatus::Cancelled, RejectReason::None, id, 0, 0, removed};
}

ExecutionResult OrderBook::modify(OrderId id, Price newPrice, Quantity newQuantity,
                                  std::vector<Trade>& trades) {
    if (newQuantity <= 0) return ExecutionResult::reject(id, RejectReason::InvalidQuantity);
    if (newPrice <= 0) return ExecutionResult::reject(id, RejectReason::InvalidPrice);
    const auto it = index_.find(id);
    if (it == index_.end()) return ExecutionResult::reject(id, RejectReason::UnknownOrderId);

    const Slot slot = it->second;
    Node& node = nodes_[slot];
    if (newPrice == node.price && newQuantity <= node.remaining) {
        node.level->totalQuantity -= node.remaining - newQuantity;
        node.remaining = newQuantity;
        return {OrderStatus::Resting, RejectReason::None, id, 0, newQuantity, 0};
    }

    const Side side = node.side;
    removeNode(slot);
    return submit(OrderRequest::limit(id, side, newPrice, newQuantity), trades);
}

void OrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    nodes_.clear();
    freeSlots_.clear();
    index_.clear();
}

// Queries

std::optional<Price> OrderBook::bestBid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<OrderInfo> OrderBook::find(OrderId id) const {
    const auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    const Node& n = nodes_[it->second];
    return OrderInfo{n.id, n.side, n.price, n.remaining};
}

std::size_t OrderBook::levelCount(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

Quantity OrderBook::quantityAt(Side side, Price price) const noexcept {
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.totalQuantity;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.totalQuantity;
}

std::vector<LevelInfo> OrderBook::depth(Side side, std::size_t maxLevels) const {
    std::vector<LevelInfo> out;
    auto collect = [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            if (out.size() >= maxLevels) break;
            out.push_back(LevelInfo{price, level.totalQuantity, level.orderCount});
        }
    };
    if (side == Side::Buy) collect(bids_); else collect(asks_);
    return out;
}

std::vector<OrderInfo> OrderBook::ordersAt(Side side, Price price) const {
    std::vector<OrderInfo> out;
    const Level* level = nullptr;
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        if (it != bids_.end()) level = &it->second;
    } else {
        const auto it = asks_.find(price);
        if (it != asks_.end()) level = &it->second;
    }
    if (level == nullptr) return out;
    for (Slot s = level->head; s != kNil; s = nodes_[s].next) {
        const Node& n = nodes_[s];
        out.push_back(OrderInfo{n.id, n.side, n.price, n.remaining});
    }
    return out;
}

// Validation

template <class Levels>
bool OrderBook::validateSide(const Levels& levels, Side side, std::size_t& seen,
                             std::string* error) const {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    const char* sideName = side == Side::Buy ? "bid" : "ask";
    for (const auto& [price, level] : levels) {
        if (price <= 0) return fail(std::string(sideName) + " level with non-positive price");
        if (level.head == kNil || level.orderCount == 0)
            return fail(std::string(sideName) + " empty level at " + std::to_string(price));
        Quantity total = 0;
        std::uint32_t count = 0;
        Slot prev = kNil;
        for (Slot s = level.head; s != kNil; s = nodes_[s].next) {
            if (s >= nodes_.size()) return fail("slot out of range");
            const Node& n = nodes_[s];
            if (n.prev != prev) return fail("broken prev link at order " + std::to_string(n.id));
            if (n.side != side || n.price != price || n.level != &level)
                return fail("order " + std::to_string(n.id) + " on wrong side/level");
            if (n.remaining <= 0) return fail("order " + std::to_string(n.id) + " has remaining <= 0");
            const auto it = index_.find(n.id);
            if (it == index_.end() || it->second != s)
                return fail("order " + std::to_string(n.id) + " missing from index");
            total += n.remaining;
            ++count;
            prev = s;
            if (count > nodes_.size()) return fail("cycle in level list");
        }
        if (level.tail != prev) return fail("tail mismatch at " + std::to_string(price));
        if (total != level.totalQuantity) return fail("totalQuantity mismatch at " + std::to_string(price));
        if (count != level.orderCount) return fail("orderCount mismatch at " + std::to_string(price));
        seen += count;
    }
    return true;
}

bool OrderBook::validate(std::string* error) const {
    std::size_t seen = 0;
    if (!validateSide(bids_, Side::Buy, seen, error)) return false;
    if (!validateSide(asks_, Side::Sell, seen, error)) return false;
    if (seen != index_.size()) {
        if (error) *error = "index size " + std::to_string(index_.size()) + " != linked orders " + std::to_string(seen);
        return false;
    }
    if (nodes_.size() != index_.size() + freeSlots_.size()) {
        if (error) *error = "node pool accounting mismatch";
        return false;
    }
    const auto bid = bestBid();
    const auto ask = bestAsk();
    if (bid && ask && *bid >= *ask) {
        if (error) *error = "book crossed: bid " + std::to_string(*bid) + " >= ask " + std::to_string(*ask);
        return false;
    }
    return true;
}

}
