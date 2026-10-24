#include "orderbook/StopOrderScheduler.hpp"

#include <algorithm>

namespace orderbook {

RejectReason StopOrderScheduler::add(OrderId id, Side side, Price stopPrice, Quantity quantity) {
    if (quantity <= 0) return RejectReason::InvalidQuantity;
    if (stopPrice <= 0) return RejectReason::InvalidStopPrice;
    if (contains(id)) return RejectReason::DuplicateOrderId;

    const Key key{stopPrice, nextSequence_};
    const StopOrder order{id, side, stopPrice, quantity, nextSequence_};
    index_.emplace(id, std::make_pair(side, key));
    try {
        if (side == Side::Buy) buyStops_.emplace(key, order);
        else sellStops_.emplace(key, order);
    } catch (...) {
        index_.erase(id);
        throw;
    }
    ++nextSequence_;
    return RejectReason::None;
}

std::optional<StopOrder> StopOrderScheduler::cancel(OrderId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    const auto [side, key] = it->second;
    std::optional<StopOrder> removed;
    if (side == Side::Buy) {
        const auto s = buyStops_.find(key);
        removed = s->second;
        buyStops_.erase(s);
    } else {
        const auto s = sellStops_.find(key);
        removed = s->second;
        sellStops_.erase(s);
    }
    index_.erase(it);
    return removed;
}

bool StopOrderScheduler::hasTriggered(std::optional<Price> bestBid,
                                      std::optional<Price> bestAsk) const noexcept {
    if (bestAsk && !buyStops_.empty() && buyStops_.begin()->first.first <= *bestAsk) return true;
    if (bestBid && !sellStops_.empty() && sellStops_.begin()->first.first >= *bestBid) return true;
    return false;
}

void StopOrderScheduler::collectTriggered(std::optional<Price> bestBid,
                                          std::optional<Price> bestAsk,
                                          std::vector<StopOrder>& out) {
    const std::size_t first = out.size();
    if (bestAsk) {
        auto it = buyStops_.begin();
        while (it != buyStops_.end() && it->first.first <= *bestAsk) {
            out.push_back(it->second);
            index_.erase(it->second.id);
            it = buyStops_.erase(it);
        }
    }
    if (bestBid) {
        auto it = sellStops_.begin();
        while (it != sellStops_.end() && it->first.first >= *bestBid) {
            out.push_back(it->second);
            index_.erase(it->second.id);
            it = sellStops_.erase(it);
        }
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
              [](const StopOrder& a, const StopOrder& b) { return a.sequence < b.sequence; });
}

std::optional<StopOrder> StopOrderScheduler::find(OrderId id) const {
    const auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    const auto& [side, key] = it->second;
    if (side == Side::Buy) return buyStops_.at(key);
    return sellStops_.at(key);
}

std::vector<StopOrder> StopOrderScheduler::pending() const {
    std::vector<StopOrder> out;
    out.reserve(index_.size());
    for (const auto& kv : buyStops_) out.push_back(kv.second);
    for (const auto& kv : sellStops_) out.push_back(kv.second);
    std::sort(out.begin(), out.end(),
              [](const StopOrder& a, const StopOrder& b) { return a.sequence < b.sequence; });
    return out;
}

void StopOrderScheduler::clear() noexcept {
    buyStops_.clear();
    sellStops_.clear();
    index_.clear();
}

} 
