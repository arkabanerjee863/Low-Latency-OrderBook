#include "orderbook/MatchingEngine.hpp"

#include <type_traits>
#include <variant>

namespace orderbook {

MatchingEngine::MatchingEngine(EngineListener* listener, std::size_t expectedOrders)
    : book_(expectedOrders), listener_(listener) {
    trades_.reserve(256);
    triggered_.reserve(64);
}

void MatchingEngine::publishTrades() {
    struct ClearOnExit {
        std::vector<Trade>& buffer;
        ~ClearOnExit() { buffer.clear(); }
    } guard{trades_};
    if (listener_ != nullptr) {
        for (const Trade& t : trades_) listener_->onTrade(t);
    }
}

void MatchingEngine::runStopCascade() {
    while (stops_.hasTriggered(book_.bestBid(), book_.bestAsk())) {
        triggered_.clear();
        stops_.collectTriggered(book_.bestBid(), book_.bestAsk(), triggered_);
        for (const StopOrder& stop : triggered_) {
            if (listener_ != nullptr) listener_->onStopTriggered(stop);
            const ExecutionResult result =
                book_.submit(OrderRequest::market(stop.id, stop.side, stop.quantity), trades_);
            publishTrades();
            if (listener_ != nullptr) listener_->onStopExecuted(stop, result);
        }
    }
    triggered_.clear();
}

ExecutionResult MatchingEngine::submit(const OrderRequest& request) {
    const OrderId id = request.id;
    if (request.quantity <= 0) return ExecutionResult::reject(id, RejectReason::InvalidQuantity);
    if (request.type == OrderType::Stop) {
        if (request.stopPrice <= 0) return ExecutionResult::reject(id, RejectReason::InvalidStopPrice);
    } else if (request.type != OrderType::Market && request.price <= 0) {
        return ExecutionResult::reject(id, RejectReason::InvalidPrice);
    }
    if (isLive(id)) return ExecutionResult::reject(id, RejectReason::DuplicateOrderId);

    ExecutionResult result;
    if (request.type == OrderType::Stop) {
        const RejectReason why = stops_.add(id, request.side, request.stopPrice, request.quantity);
        if (why != RejectReason::None) return ExecutionResult::reject(id, why);
        result = {OrderStatus::PendingStop, RejectReason::None, id, 0, 0, 0};
    } else {
        result = book_.submit(request, trades_);
        publishTrades();
    }
    runStopCascade();
    return result;
}

ExecutionResult MatchingEngine::cancel(OrderId id) {
    ExecutionResult result = book_.cancel(id);
    if (!result.accepted()) {
        if (auto stop = stops_.cancel(id)) {
            return {OrderStatus::Cancelled, RejectReason::None, id, 0, 0, stop->quantity};
        }
        return result;  
    }
    runStopCascade();
    return result;
}

ExecutionResult MatchingEngine::modify(OrderId id, Price newPrice, Quantity newQuantity) {
    if (!book_.contains(id) && stops_.contains(id)) {
        return ExecutionResult::reject(id, RejectReason::UnsupportedOperation);
    }
    ExecutionResult result = book_.modify(id, newPrice, newQuantity, trades_);
    publishTrades();
    if (result.accepted()) runStopCascade();
    return result;
}

ExecutionResult MatchingEngine::process(const Command& command) {
    return std::visit(
        [this](const auto& c) -> ExecutionResult {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, OrderRequest>) return submit(c);
            else if constexpr (std::is_same_v<T, CancelRequest>) return cancel(c.id);
            else return modify(c.id, c.newPrice, c.newQuantity);
        },
        command);
}

void MatchingEngine::reset() noexcept {
    book_.clear();
    stops_.clear();
    trades_.clear();
    triggered_.clear();
}

}
