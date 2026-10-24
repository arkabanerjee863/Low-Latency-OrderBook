#include <limits>
#include <vector>

#include "TestHelpers.hpp"
#include "orderbook/OrderBook.hpp"

using namespace orderbook;
using orderbook::testing::requireValid;

namespace {

struct Fixture {
    OrderBook book;
    std::vector<Trade> trades;

    ExecutionResult limit(OrderId id, Side s, Price p, Quantity q) {
        return book.submit(OrderRequest::limit(id, s, p, q), trades);
    }
    ExecutionResult market(OrderId id, Side s, Quantity q) {
        return book.submit(OrderRequest::market(id, s, q), trades);
    }
    ExecutionResult ioc(OrderId id, Side s, Price p, Quantity q) {
        return book.submit(OrderRequest::ioc(id, s, p, q), trades);
    }
    ExecutionResult modify(OrderId id, Price p, Quantity q) { return book.modify(id, p, q, trades); }
};

}  

TEST_CASE("Empty book", "[book]") {
    Fixture f;
    CHECK(f.book.empty());
    CHECK_FALSE(f.book.bestBid().has_value());
    CHECK_FALSE(f.book.bestAsk().has_value());
    CHECK(f.book.levelCount(Side::Buy) == 0);
    CHECK(f.book.depth(Side::Sell).empty());
    CHECK(f.book.quantityAt(Side::Buy, 100) == 0);
    CHECK_FALSE(f.book.find(1).has_value());
    CHECK(f.book.cancel(1).reason == RejectReason::UnknownOrderId);
    CHECK(f.modify(1, 100, 10).reason == RejectReason::UnknownOrderId);

    SECTION("market order into an empty book is cancelled, not rested") {
        const auto r = f.market(2, Side::Buy, 10);
        CHECK(r.status == OrderStatus::Cancelled);
        CHECK(r.filled == 0);
        CHECK(r.cancelled == 10);
        CHECK(f.book.empty());
        CHECK(f.trades.empty());
    }
    requireValid(f.book);
}

TEST_CASE("Best bid and ask track the top of each side", "[book]") {
    Fixture f;
    f.limit(1, Side::Buy, 99, 10);
    f.limit(2, Side::Buy, 101, 10);
    f.limit(3, Side::Buy, 100, 10);
    f.limit(4, Side::Sell, 105, 10);
    f.limit(5, Side::Sell, 103, 10);
    f.limit(6, Side::Sell, 104, 10);
    CHECK(f.book.bestBid() == Price{101});
    CHECK(f.book.bestAsk() == Price{103});
    CHECK(f.book.levelCount(Side::Buy) == 3);
    CHECK(f.book.levelCount(Side::Sell) == 3);
    const auto bids = f.book.depth(Side::Buy);
    REQUIRE(bids.size() == 3);
    CHECK(bids[0].price == 101);
    CHECK(bids[1].price == 100);
    CHECK(bids[2].price == 99);
    const auto asks = f.book.depth(Side::Sell, 2);
    REQUIRE(asks.size() == 2);
    CHECK(asks[0].price == 103);
    CHECK(asks[1].price == 104);
    CHECK(f.trades.empty());
    requireValid(f.book);
}

TEST_CASE("Full match removes both orders and executes at the resting price", "[book][match]") {
    Fixture f;
    f.limit(1, Side::Buy, 100, 10);
    const auto r = f.limit(2, Side::Sell, 95, 10);  
    CHECK(r.status == OrderStatus::Filled);
    CHECK(r.filled == 10);
    REQUIRE(f.trades.size() == 1);
    CHECK(f.trades[0] == Trade{2, 1, Side::Sell, 100, 10});  
    CHECK(f.book.empty());
    requireValid(f.book);
}

TEST_CASE("Buy aggressor executes at resting ask prices", "[book][match]") {
    Fixture f;
    f.limit(1, Side::Sell, 101, 5);
    f.limit(2, Side::Sell, 102, 5);
    const auto r = f.limit(3, Side::Buy, 110, 8);
    CHECK(r.status == OrderStatus::Filled);
    REQUIRE(f.trades.size() == 2);
    CHECK(f.trades[0] == Trade{3, 1, Side::Buy, 101, 5});
    CHECK(f.trades[1] == Trade{3, 2, Side::Buy, 102, 3});
    CHECK(f.book.bestAsk() == Price{102});
    CHECK(f.book.quantityAt(Side::Sell, 102) == 2);
    CHECK_FALSE(f.book.bestBid().has_value());
    requireValid(f.book);
}

TEST_CASE("Partial fills keep accurate remaining quantities", "[book][match]") {
    Fixture f;
    SECTION("resting order partially filled") {
        f.limit(1, Side::Buy, 150, 20);
        const auto r = f.limit(2, Side::Sell, 150, 7);
        CHECK(r.status == OrderStatus::Filled);
        REQUIRE(f.book.find(1).has_value());
        CHECK(f.book.find(1)->remaining == 13);
        CHECK(f.book.quantityAt(Side::Buy, 150) == 13);
    }
    SECTION("aggressor partially filled rests the residual at its limit") {
        f.limit(1, Side::Sell, 100, 4);
        const auto r = f.limit(2, Side::Buy, 101, 10);
        CHECK(r.status == OrderStatus::Resting);
        CHECK(r.filled == 4);
        CHECK(r.resting == 6);
        CHECK(f.book.bestBid() == Price{101});
        CHECK_FALSE(f.book.bestAsk().has_value());
        CHECK(f.book.find(2)->remaining == 6);
    }
    requireValid(f.book);
}

TEST_CASE("Price-time priority within a level is FIFO", "[book][priority]") {
    Fixture f;
    f.limit(1, Side::Sell, 100, 5);
    f.limit(2, Side::Sell, 100, 5);
    f.limit(3, Side::Sell, 100, 5);
    f.limit(4, Side::Sell, 99, 1);  

    const auto level = f.book.ordersAt(Side::Sell, 100);
    REQUIRE(level.size() == 3);
    CHECK(level[0].id == 1);
    CHECK(level[1].id == 2);
    CHECK(level[2].id == 3);

    f.limit(10, Side::Buy, 100, 8);
    REQUIRE(f.trades.size() == 3);
    CHECK(f.trades[0].restingId == 4);
    CHECK(f.trades[1] == Trade{10, 1, Side::Buy, 100, 5});
    CHECK(f.trades[2] == Trade{10, 2, Side::Buy, 100, 2});
    CHECK(f.book.find(2)->remaining == 3);
    CHECK_FALSE(f.book.find(1).has_value());
    const auto after = f.book.ordersAt(Side::Sell, 100);
    REQUIRE(after.size() == 2);
    CHECK(after[0].id == 2);  
    CHECK(after[1].id == 3);
    requireValid(f.book);
}

TEST_CASE("Limit orders never trade through their limit", "[book][match]") {
    Fixture f;
    f.limit(1, Side::Sell, 101, 5);
    const auto r = f.limit(2, Side::Buy, 100, 5);
    CHECK(r.status == OrderStatus::Resting);
    CHECK(f.trades.empty());
    CHECK(f.book.bestBid() == Price{100});
    CHECK(f.book.bestAsk() == Price{101});
    requireValid(f.book);
}

TEST_CASE("Empty price levels are removed", "[book][levels]") {
    Fixture f;
    f.limit(1, Side::Sell, 100, 5);
    f.limit(2, Side::Sell, 101, 5);
    f.limit(3, Side::Buy, 90, 5);
    f.market(4, Side::Buy, 5);
    CHECK(f.book.levelCount(Side::Sell) == 1);
    CHECK(f.book.quantityAt(Side::Sell, 100) == 0);
    CHECK(f.book.ordersAt(Side::Sell, 100).empty());
    f.book.cancel(2);
    CHECK(f.book.levelCount(Side::Sell) == 0);
    f.modify(3, 91, 5);
    CHECK(f.book.levelCount(Side::Buy) == 1);
    CHECK(f.book.quantityAt(Side::Buy, 90) == 0);
    CHECK(f.book.quantityAt(Side::Buy, 91) == 5);
    requireValid(f.book);
}

TEST_CASE("Market orders sweep levels and cancel the residual", "[book][market]") {
    Fixture f;
    f.limit(1, Side::Buy, 100, 3);
    f.limit(2, Side::Buy, 99, 3);
    f.limit(3, Side::Buy, 50, 3);
    SECTION("fully filled") {
        const auto r = f.market(9, Side::Sell, 7);
        CHECK(r.status == OrderStatus::Filled);
        CHECK(r.filled == 7);
        REQUIRE(f.trades.size() == 3);
        CHECK(f.trades[2] == Trade{9, 3, Side::Sell, 50, 1});  
        CHECK(f.book.find(3)->remaining == 2);
    }
    SECTION("residual cancelled, never rests") {
        const auto r = f.market(9, Side::Sell, 20);
        CHECK(r.status == OrderStatus::Cancelled);
        CHECK(r.filled == 9);
        CHECK(r.cancelled == 11);
        CHECK(f.book.empty());
        CHECK_FALSE(f.book.contains(9));
    }
    SECTION("price field is ignored for market orders") {
        OrderRequest req = OrderRequest::market(9, Side::Sell, 3);
        req.price = -5;
        const auto r = f.book.submit(req, f.trades);
        CHECK(r.status == OrderStatus::Filled);
    }
    requireValid(f.book);
}

TEST_CASE("IOC orders respect the limit and cancel the residual", "[book][ioc]") {
    Fixture f;
    f.limit(1, Side::Sell, 100, 5);
    f.limit(2, Side::Sell, 102, 5);
    SECTION("partial fill up to the limit") {
        const auto r = f.ioc(9, Side::Buy, 101, 8);
        CHECK(r.status == OrderStatus::Cancelled);
        CHECK(r.filled == 5);
        CHECK(r.cancelled == 3);
        CHECK(r.resting == 0);
        CHECK_FALSE(f.book.contains(9));
        CHECK_FALSE(f.book.bestBid().has_value());
        CHECK(f.book.bestAsk() == Price{102});
    }
    SECTION("complete fill") {
        const auto r = f.ioc(9, Side::Buy, 102, 10);
        CHECK(r.status == OrderStatus::Filled);
        CHECK(r.filled == 10);
        CHECK(f.book.empty());
    }
    SECTION("no fill") {
        const auto r = f.ioc(9, Side::Buy, 99, 10);
        CHECK(r.status == OrderStatus::Cancelled);
        CHECK(r.filled == 0);
        CHECK(r.cancelled == 10);
        CHECK(f.trades.empty());
    }
    requireValid(f.book);
}

TEST_CASE("Cancellation", "[book][cancel]") {
    Fixture f;
    f.limit(1, Side::Buy, 100, 5);
    f.limit(2, Side::Buy, 100, 6);
    f.limit(3, Side::Buy, 100, 7);

    const auto r = f.book.cancel(2); 
    CHECK(r.status == OrderStatus::Cancelled);
    CHECK(r.cancelled == 6);
    CHECK(f.book.quantityAt(Side::Buy, 100) == 12);
    const auto level = f.book.ordersAt(Side::Buy, 100);
    REQUIRE(level.size() == 2);
    CHECK(level[0].id == 1);
    CHECK(level[1].id == 3);

    SECTION("cancel twice") { CHECK(f.book.cancel(2).reason == RejectReason::UnknownOrderId); }
    SECTION("cancelled order never trades") {
        f.market(9, Side::Sell, 100);
        for (const auto& t : f.trades) CHECK(t.restingId != 2);
        CHECK(f.trades.size() == 2);
    }
    SECTION("cancel a partially filled order removes only its remainder") {
        f.market(9, Side::Sell, 2);
        CHECK(f.book.cancel(1).cancelled == 3);
    }
    SECTION("filled order can no longer be cancelled") {
        f.market(9, Side::Sell, 5);
        CHECK(f.book.cancel(1).reason == RejectReason::UnknownOrderId);
    }
    requireValid(f.book);
}

TEST_CASE("Modification", "[book][modify]") {
    Fixture f;
    f.limit(1, Side::Sell, 100, 10);
    f.limit(2, Side::Sell, 100, 10);

    SECTION("reducing quantity at the same price keeps priority") {
        const auto r = f.modify(1, 100, 4);
        CHECK(r.status == OrderStatus::Resting);
        CHECK(r.resting == 4);
        CHECK(f.book.ordersAt(Side::Sell, 100).front().id == 1);
        CHECK(f.book.quantityAt(Side::Sell, 100) == 14);
    }
    SECTION("increasing quantity loses priority") {
        f.modify(1, 100, 11);
        const auto level = f.book.ordersAt(Side::Sell, 100);
        REQUIRE(level.size() == 2);
        CHECK(level[0].id == 2);
        CHECK(level[1].id == 1);
        CHECK(f.book.quantityAt(Side::Sell, 100) == 21);
    }
    SECTION("changing price moves the order to the back of the new level") {
        f.limit(3, Side::Sell, 101, 1);
        f.modify(1, 101, 10);
        CHECK(f.book.ordersAt(Side::Sell, 101).back().id == 1);
        CHECK(f.book.find(1)->price == 101);
        CHECK(f.book.quantityAt(Side::Sell, 100) == 10);
    }
    SECTION("modifying to a crossing price executes immediately as aggressor") {
        f.limit(5, Side::Buy, 95, 3);
        const auto r = f.modify(5, 100, 15);
        CHECK(r.status == OrderStatus::Filled);
        CHECK(r.filled == 15);
        REQUIRE(f.trades.size() == 2);
        CHECK(f.trades[0] == Trade{5, 1, Side::Buy, 100, 10});
        CHECK(f.trades[1] == Trade{5, 2, Side::Buy, 100, 5});
        CHECK_FALSE(f.book.contains(5));
        CHECK(f.book.find(2)->remaining == 5);
    }
    SECTION("crossing modify rests its residual") {
        f.limit(5, Side::Buy, 95, 3);
        const auto r = f.modify(5, 100, 25);
        CHECK(r.status == OrderStatus::Resting);
        CHECK(r.filled == 20);
        CHECK(r.resting == 5);
        CHECK(f.book.bestBid() == Price{100});
        CHECK_FALSE(f.book.bestAsk().has_value());
    }
    SECTION("invalid modifications are rejected and change nothing") {
        CHECK(f.modify(1, 100, 0).reason == RejectReason::InvalidQuantity);
        CHECK(f.modify(1, 0, 5).reason == RejectReason::InvalidPrice);
        CHECK(f.modify(1, -3, 5).reason == RejectReason::InvalidPrice);
        CHECK(f.modify(99, 100, 5).reason == RejectReason::UnknownOrderId);
        CHECK(f.book.find(1)->remaining == 10);
        CHECK(f.book.ordersAt(Side::Sell, 100).front().id == 1);
    }
    SECTION("repeated modifications leave exactly one entry") {
        for (int i = 0; i < 50; ++i) f.modify(1, 100 + (i % 5), 1 + i);
        CHECK(f.book.orderCount() == 2);
        CHECK(f.book.find(1)->remaining == 50);
    }
    requireValid(f.book);
}

TEST_CASE("Invalid new orders are rejected without side effects", "[book][validation]") {
    Fixture f;
    f.limit(1, Side::Buy, 100, 10);
    CHECK(f.limit(2, Side::Buy, 100, 0).reason == RejectReason::InvalidQuantity);
    CHECK(f.limit(3, Side::Buy, 100, -1).reason == RejectReason::InvalidQuantity);
    CHECK(f.limit(4, Side::Buy, 0, 10).reason == RejectReason::InvalidPrice);
    CHECK(f.limit(5, Side::Sell, -100, 10).reason == RejectReason::InvalidPrice);
    CHECK(f.ioc(6, Side::Sell, 0, 10).reason == RejectReason::InvalidPrice);
    CHECK(f.market(7, Side::Sell, 0).reason == RejectReason::InvalidQuantity);
    CHECK(f.limit(1, Side::Sell, 200, 10).reason == RejectReason::DuplicateOrderId);
    CHECK(f.market(1, Side::Sell, 10).reason == RejectReason::DuplicateOrderId);
    CHECK(f.book.submit(OrderRequest::stop(8, Side::Buy, 100, 1), f.trades).reason ==
          RejectReason::UnsupportedOrderType);
    CHECK(f.book.orderCount() == 1);
    CHECK(f.book.find(1)->remaining == 10);
    CHECK(f.trades.empty());
    requireValid(f.book);
}

TEST_CASE("Ids become reusable once an order is no longer live", "[book][lifecycle]") {
    Fixture f;
    f.limit(1, Side::Buy, 100, 10);
    f.book.cancel(1);
    CHECK(f.limit(1, Side::Sell, 105, 3).status == OrderStatus::Resting);
    f.market(2, Side::Buy, 3);
    CHECK(f.limit(1, Side::Buy, 90, 1).status == OrderStatus::Resting);
    requireValid(f.book);
}

TEST_CASE("Boundary values", "[book][boundary]") {
    Fixture f;
    constexpr Price kMaxPrice = std::numeric_limits<Price>::max();
    constexpr Quantity kBigQty = std::numeric_limits<Quantity>::max() / 4;
    f.limit(1, Side::Sell, kMaxPrice, kBigQty);
    f.limit(2, Side::Buy, 1, kBigQty);
    CHECK(f.book.bestAsk() == kMaxPrice);
    CHECK(f.book.bestBid() == Price{1});
    const auto r = f.market(3, Side::Buy, kBigQty);
    CHECK(r.status == OrderStatus::Filled);
    CHECK(f.trades.back().price == kMaxPrice);
    CHECK(f.limit(std::numeric_limits<OrderId>::max(), Side::Sell, 2, 1).status == OrderStatus::Resting);
    CHECK(f.book.cancel(std::numeric_limits<OrderId>::max()).cancelled == 1);
    requireValid(f.book);
}

TEST_CASE("clear() empties the book and slots are recycled", "[book][lifecycle]") {
    Fixture f;
    for (OrderId id = 1; id <= 100; ++id) f.limit(id, id % 2 ? Side::Buy : Side::Sell, id % 2 ? 50 : 150, 1);
    f.book.clear();
    CHECK(f.book.empty());
    CHECK(f.book.levelCount(Side::Buy) == 0);
    requireValid(f.book);
    for (int round = 0; round < 5; ++round) {
        for (OrderId id = 1; id <= 100; ++id) f.limit(id, Side::Buy, 100 + static_cast<Price>(id % 7), 1);
        for (OrderId id = 1; id <= 100; ++id) REQUIRE(f.book.cancel(id).accepted());
        requireValid(f.book);
    }
    CHECK(f.book.empty());
}
