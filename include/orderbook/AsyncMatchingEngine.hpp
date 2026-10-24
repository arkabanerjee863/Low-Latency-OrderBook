#pragma once

// Thread-safe front end: many producer threads, one matching thread.

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "orderbook/MatchingEngine.hpp"

namespace orderbook {

class AsyncMatchingEngine {
public:
    explicit AsyncMatchingEngine(EngineListener* listener = nullptr, std::size_t expectedOrders = 0);
    ~AsyncMatchingEngine();

    AsyncMatchingEngine(const AsyncMatchingEngine&) = delete;
    AsyncMatchingEngine& operator=(const AsyncMatchingEngine&) = delete;

    bool submit(Command command);

    void sync();

    void stop();

    template <class Fn>
    decltype(auto) inspect(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(engineMutex_);
        return std::forward<Fn>(fn)(static_cast<const MatchingEngine&>(engine_));
    }

    [[nodiscard]] std::uint64_t processedCount() const;

private:
    void workerLoop();

    EngineListener* listener_;

    mutable std::mutex queueMutex_;
    std::condition_variable workAvailable_;
    std::condition_variable progress_;
    std::vector<Command> pending_;        
    std::uint64_t submitted_ = 0;         
    std::uint64_t processed_ = 0;         
    bool stopping_ = false;               
    bool failed_ = false;                
    std::exception_ptr error_;           

    mutable std::mutex engineMutex_;
    MatchingEngine engine_;               

    std::mutex joinMutex_;               
    std::thread worker_;                  
};

} 
