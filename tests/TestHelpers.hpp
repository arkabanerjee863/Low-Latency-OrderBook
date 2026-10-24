#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "orderbook/MatchingEngine.hpp"
#include "orderbook/Types.hpp"

namespace orderbook {

inline std::ostream& operator<<(std::ostream& os, const Trade& t) {
    return os << "Trade{aggr=" << t.aggressorId << " rest=" << t.restingId << " side=" << toString(t.aggressorSide)
              << " px=" << t.price << " qty=" << t.quantity << "}";
}
inline std::ostream& operator<<(std::ostream& os, const ExecutionResult& r) {
    return os << "Result{id=" << r.id << " " << toString(r.status) << " reason=" << toString(r.reason)
              << " filled=" << r.filled << " resting=" << r.resting << " cancelled=" << r.cancelled << "}";
}

namespace testing {

class RecordingListener : public EngineListener {
public:
    void onTrade(const Trade& t) override { trades.push_back(t); }
    void onStopTriggered(const StopOrder& s) override { triggered.push_back(s.id); }
    void onStopExecuted(const StopOrder&, const ExecutionResult& r) override { stopResults.push_back(r); }
    void clear() {
        trades.clear();
        triggered.clear();
        stopResults.clear();
    }
    std::vector<Trade> trades;
    std::vector<OrderId> triggered;
    std::vector<ExecutionResult> stopResults;
};

inline void requireValid(const OrderBook& book) {
    std::string error;
    const bool ok = book.validate(&error);
    INFO(error);
    REQUIRE(ok);
}

}  
} 
