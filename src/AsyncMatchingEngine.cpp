#include "orderbook/AsyncMatchingEngine.hpp"

namespace orderbook {

AsyncMatchingEngine::AsyncMatchingEngine(EngineListener* listener, std::size_t expectedOrders)
    : listener_(listener), engine_(listener, expectedOrders) {
    pending_.reserve(1024);
    worker_ = std::thread([this] { workerLoop(); });
}

AsyncMatchingEngine::~AsyncMatchingEngine() {
    try {
        stop();
    } catch (...) {
    }
}

bool AsyncMatchingEngine::submit(Command command) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopping_ || failed_) return false;
        pending_.push_back(std::move(command));
        ++submitted_;
    }
    workAvailable_.notify_one();
    return true;
}

void AsyncMatchingEngine::sync() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    const std::uint64_t target = submitted_;
    progress_.wait(lock, [&] { return processed_ >= target || failed_; });
    if (failed_ && error_) std::rethrow_exception(error_);
}

std::uint64_t AsyncMatchingEngine::processedCount() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return processed_;
}

void AsyncMatchingEngine::stop() {
    std::lock_guard<std::mutex> joinLock(joinMutex_);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopping_ = true;
    }
    workAvailable_.notify_one();
    if (worker_.joinable()) worker_.join();

    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        error = std::exchange(error_, nullptr);
    }
    if (error) std::rethrow_exception(error);
}

void AsyncMatchingEngine::workerLoop() {
    std::vector<Command> batch;
    batch.reserve(1024);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            workAvailable_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) return;  
            batch.swap(pending_);          
        }

        try {
            std::lock_guard<std::mutex> lock(engineMutex_);
            for (const Command& command : batch) {
                const ExecutionResult result = engine_.process(command);
                if (listener_ != nullptr) listener_->onCommandProcessed(command, result);
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                failed_ = true;
                error_ = std::current_exception();
                pending_.clear();
            }
            progress_.notify_all();
            return;
        }

        const auto count = static_cast<std::uint64_t>(batch.size());
        batch.clear();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            processed_ += count;
        }
        progress_.notify_all();
    }
}

} 
