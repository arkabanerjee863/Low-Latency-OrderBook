#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "orderbook/Types.hpp"

namespace orderbook::testing {

class ReferenceEngine {
public:
    struct Resting {
        OrderId id;
        Side side;
        Price price;
        Quantity qty;
        std::uint64_t time;
    };
    struct Stop {
        OrderId id;
        Side side;
        Price stop;
        Quantity qty;
        std::uint64_t seq;
    };

    std::vector<Trade> trades;

    ExecutionResult submit(const OrderRequest& r) {
        if (r.quantity <= 0) return ExecutionResult::reject(r.id, RejectReason::InvalidQuantity);
        if (r.type == OrderType::Stop) {
            if (r.stopPrice <= 0) return ExecutionResult::reject(r.id, RejectReason::InvalidStopPrice);
        } else if (r.type != OrderType::Market && r.price <= 0) {
            return ExecutionResult::reject(r.id, RejectReason::InvalidPrice);
        }
        if (live(r.id)) return ExecutionResult::reject(r.id, RejectReason::DuplicateOrderId);

        ExecutionResult res;
        if (r.type == OrderType::Stop) {
            stops_.push_back(Stop{r.id, r.side, r.stopPrice, r.quantity, clock_++});
            res = {OrderStatus::PendingStop, RejectReason::None, r.id, 0, 0, 0};
        } else {
            res = execute(r);
        }
        cascade();
        return res;
    }

    ExecutionResult cancel(OrderId id) {
        for (auto it = book_.begin(); it != book_.end(); ++it) {
            if (it->id == id) {
                const Quantity q = it->qty;
                book_.erase(it);
                cascade();
                return {OrderStatus::Cancelled, RejectReason::None, id, 0, 0, q};
            }
        }
        for (auto it = stops_.begin(); it != stops_.end(); ++it) {
            if (it->id == id) {
                const Quantity q = it->qty;
                stops_.erase(it);
                return {OrderStatus::Cancelled, RejectReason::None, id, 0, 0, q};
            }
        }
        return ExecutionResult::reject(id, RejectReason::UnknownOrderId);
    }

    ExecutionResult modify(OrderId id, Price price, Quantity qty) {
        auto it = std::find_if(book_.begin(), book_.end(), [&](const Resting& o) { return o.id == id; });
        if (it == book_.end()) {
            const bool isStop = std::any_of(stops_.begin(), stops_.end(), [&](const Stop& s) { return s.id == id; });
            if (isStop) return ExecutionResult::reject(id, RejectReason::UnsupportedOperation);
        }
        if (qty <= 0) return ExecutionResult::reject(id, RejectReason::InvalidQuantity);
        if (price <= 0) return ExecutionResult::reject(id, RejectReason::InvalidPrice);
        if (it == book_.end()) return ExecutionResult::reject(id, RejectReason::UnknownOrderId);

        if (price == it->price && qty <= it->qty) {
            it->qty = qty;  // keeps its time stamp
            return {OrderStatus::Resting, RejectReason::None, id, 0, qty, 0};
        }
        const Side side = it->side;
        book_.erase(it);
        ExecutionResult res = execute(OrderRequest::limit(id, side, price, qty));
        cascade();
        return res;
    }

    ExecutionResult process(const Command& c) {
        if (auto* o = std::get_if<OrderRequest>(&c)) return submit(*o);
        if (auto* x = std::get_if<CancelRequest>(&c)) return cancel(x->id);
        const auto& m = std::get<ModifyRequest>(c);
        return modify(m.id, m.newPrice, m.newQuantity);
    }

    std::optional<Price> best(Side side) const {
        std::optional<Price> b;
        for (const auto& o : book_) {
            if (o.side != side) continue;
            if (!b || (side == Side::Buy ? o.price > *b : o.price < *b)) b = o.price;
        }
        return b;
    }

    std::vector<Resting> sorted(Side side) const {
        std::vector<Resting> v;
        for (const auto& o : book_) if (o.side == side) v.push_back(o);
        std::sort(v.begin(), v.end(), [&](const Resting& a, const Resting& b) {
            if (a.price != b.price) return side == Side::Buy ? a.price > b.price : a.price < b.price;
            return a.time < b.time;
        });
        return v;
    }

    std::vector<Stop> pendingStops() const {
        auto v = stops_;
        std::sort(v.begin(), v.end(), [](const Stop& a, const Stop& b) { return a.seq < b.seq; });
        return v;
    }

private:
    bool live(OrderId id) const {
        return std::any_of(book_.begin(), book_.end(), [&](const Resting& o) { return o.id == id; }) ||
               std::any_of(stops_.begin(), stops_.end(), [&](const Stop& s) { return s.id == id; });
    }

    bool crosses(Side aggressor, Price restingPrice, std::optional<Price> limit) const {
        if (!limit) return true;
        return aggressor == Side::Buy ? restingPrice <= *limit : restingPrice >= *limit;
    }

    ExecutionResult execute(const OrderRequest& r) {
        std::optional<Price> limit;
        if (r.type != OrderType::Market) limit = r.price;
        Quantity left = r.quantity;
        const Side other = opposite(r.side);
        while (left > 0) {
            Resting* bestOrder = nullptr;
            for (auto& o : book_) {
                if (o.side != other) continue;
                if (!bestOrder) { bestOrder = &o; continue; }
                const bool better = other == Side::Buy ? o.price > bestOrder->price : o.price < bestOrder->price;
                if (better || (o.price == bestOrder->price && o.time < bestOrder->time)) bestOrder = &o;
            }
            if (!bestOrder || !crosses(r.side, bestOrder->price, limit)) break;
            const Quantity q = std::min(left, bestOrder->qty);
            trades.push_back(Trade{r.id, bestOrder->id, r.side, bestOrder->price, q});
            left -= q;
            bestOrder->qty -= q;
            if (bestOrder->qty == 0) {
                const OrderId gone = bestOrder->id;
                book_.erase(std::remove_if(book_.begin(), book_.end(), [&](const Resting& o) { return o.id == gone; }),
                            book_.end());
            }
        }
        ExecutionResult res{OrderStatus::Filled, RejectReason::None, r.id, r.quantity - left, 0, 0};
        if (left == 0) return res;
        if (r.type == OrderType::Limit) {
            book_.push_back(Resting{r.id, r.side, r.price, left, clock_++});
            res.status = OrderStatus::Resting;
            res.resting = left;
        } else {
            res.status = OrderStatus::Cancelled;
            res.cancelled = left;
        }
        return res;
    }

    void cascade() {
        for (;;) {
            const auto bid = best(Side::Buy);
            const auto ask = best(Side::Sell);
            std::vector<Stop> fired;
            for (auto it = stops_.begin(); it != stops_.end();) {
                const bool trig = it->side == Side::Buy ? (ask && *ask >= it->stop) : (bid && *bid <= it->stop);
                if (trig) {
                    fired.push_back(*it);
                    it = stops_.erase(it);
                } else {
                    ++it;
                }
            }
            if (fired.empty()) return;
            std::sort(fired.begin(), fired.end(), [](const Stop& a, const Stop& b) { return a.seq < b.seq; });
            for (const auto& s : fired) execute(OrderRequest::market(s.id, s.side, s.qty));
        }
    }

    std::vector<Resting> book_;
    std::vector<Stop> stops_;
    std::uint64_t clock_ = 0;
};

} 
