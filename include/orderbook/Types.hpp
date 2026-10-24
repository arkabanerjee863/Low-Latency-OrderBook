#pragma once

// Core vocabulary types shared by every component.

#include <cstdint>
#include <string_view>
#include <variant>

namespace orderbook {

using OrderId = std::uint64_t;
using Price = std::int64_t;     
using Quantity = std::int64_t;  

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,  
    Market,  
    IOC,    
    Stop,  
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidQuantity,      
    InvalidPrice,          
    InvalidStopPrice,      
    DuplicateOrderId,      
    UnknownOrderId,        
    UnsupportedOrderType, 
    UnsupportedOperation, 
};

enum class OrderStatus : std::uint8_t {
    Rejected,    
    Resting,      
    Filled,     
    Cancelled,   
    PendingStop,  
};

struct Trade {
    OrderId aggressorId;
    OrderId restingId;
    Side aggressorSide;
    Price price;
    Quantity quantity;

    friend bool operator==(const Trade& a, const Trade& b) noexcept {
        return a.aggressorId == b.aggressorId && a.restingId == b.restingId &&
               a.aggressorSide == b.aggressorSide && a.price == b.price &&
               a.quantity == b.quantity;
    }
    friend bool operator!=(const Trade& a, const Trade& b) noexcept { return !(a == b); }
};

struct OrderRequest {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Quantity quantity = 0;
    Price price = 0;
    Price stopPrice = 0;

    static constexpr OrderRequest limit(OrderId id, Side side, Price price, Quantity qty) noexcept {
        return {id, side, OrderType::Limit, qty, price, 0};
    }
    static constexpr OrderRequest ioc(OrderId id, Side side, Price price, Quantity qty) noexcept {
        return {id, side, OrderType::IOC, qty, price, 0};
    }
    static constexpr OrderRequest market(OrderId id, Side side, Quantity qty) noexcept {
        return {id, side, OrderType::Market, qty, 0, 0};
    }
    static constexpr OrderRequest stop(OrderId id, Side side, Price stopPrice, Quantity qty) noexcept {
        return {id, side, OrderType::Stop, qty, 0, stopPrice};
    }
};

struct CancelRequest {
    OrderId id = 0;
};

struct ModifyRequest {
    OrderId id = 0;
    Price newPrice = 0;
    Quantity newQuantity = 0;
};

using Command = std::variant<OrderRequest, CancelRequest, ModifyRequest>;

struct ExecutionResult {
    OrderStatus status = OrderStatus::Rejected;
    RejectReason reason = RejectReason::None;
    OrderId id = 0;
    Quantity filled = 0;
    Quantity resting = 0;
    Quantity cancelled = 0;

    [[nodiscard]] bool accepted() const noexcept { return status != OrderStatus::Rejected; }

    friend bool operator==(const ExecutionResult& a, const ExecutionResult& b) noexcept {
        return a.status == b.status && a.reason == b.reason && a.id == b.id &&
               a.filled == b.filled && a.resting == b.resting && a.cancelled == b.cancelled;
    }
    friend bool operator!=(const ExecutionResult& a, const ExecutionResult& b) noexcept {
        return !(a == b);
    }

    static constexpr ExecutionResult reject(OrderId id, RejectReason why) noexcept {
        return {OrderStatus::Rejected, why, id, 0, 0, 0};
    }
};

struct OrderInfo {
    OrderId id;
    Side side;
    Price price;
    Quantity remaining;
};

struct LevelInfo {
    Price price;
    Quantity totalQuantity;
    std::uint32_t orderCount;
};

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

constexpr std::string_view toString(Side s) noexcept { return s == Side::Buy ? "buy" : "sell"; }

constexpr std::string_view toString(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit: return "limit";
        case OrderType::Market: return "market";
        case OrderType::IOC: return "ioc";
        case OrderType::Stop: return "stop";
    }
    return "?";
}

constexpr std::string_view toString(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::Rejected: return "rejected";
        case OrderStatus::Resting: return "resting";
        case OrderStatus::Filled: return "filled";
        case OrderStatus::Cancelled: return "cancelled";
        case OrderStatus::PendingStop: return "pending-stop";
    }
    return "?";
}

constexpr std::string_view toString(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None: return "none";
        case RejectReason::InvalidQuantity: return "invalid-quantity";
        case RejectReason::InvalidPrice: return "invalid-price";
        case RejectReason::InvalidStopPrice: return "invalid-stop-price";
        case RejectReason::DuplicateOrderId: return "duplicate-order-id";
        case RejectReason::UnknownOrderId: return "unknown-order-id";
        case RejectReason::UnsupportedOrderType: return "unsupported-order-type";
        case RejectReason::UnsupportedOperation: return "unsupported-operation";
    }
    return "?";
}

} 