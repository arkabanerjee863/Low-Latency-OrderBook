#include <vector>

#include "TestHelpers.hpp"
#include "orderbook/MatchingEngine.hpp"
#include "orderbook/StopOrderScheduler.hpp"

using namespace orderbook;
using orderbook::testing::RecordingListener;
using orderbook::testing::requireValid;


TEST_CASE("Scheduler validates and stores stops", "[stop][scheduler]") {
    StopOrderScheduler s;
    CHECK(s.add(1, Side::Buy, 100, 0) == RejectReason::InvalidQuantity);
    CHECK(s.add(1, Side::Buy, 0, 5) == RejectReason::InvalidStopPrice);
    CHECK(s.add(1, Side::Buy, 100, 5) == RejectReason::None);
    CHECK(s.add(1, Side::Sell, 90, 5) == RejectReason::DuplicateOrderId);
    CHECK(s.size() == 1);
    REQUIRE(s.find(1).has_value());
    CHECK(s.find(1)->stopPrice == 100);
}

TEST_CASE("Scheduler trigger conditions use the correct side of the book", "[stop][scheduler]") {
    StopOrderScheduler s;
    s.add(1, Side::Buy, 100, 5);  
    s.add(2, Side::Sell, 90, 5);  
    std::vector<StopOrder> out;

    SECTION("empty book never triggers (original bug: empty bid read as 0.0)") {
        CHECK_FALSE(s.hasTriggered(std::nullopt, std::nullopt));
        s.collectTriggered(std::nullopt, std::nullopt, out);
        CHECK(out.empty());
        CHECK(s.size() == 2);
    }
    SECTION("an empty bid side does not trigger sell stops") {
        s.collectTriggered(std::nullopt, Price{99}, out);
        CHECK(out.empty());
    }
    SECTION("thresholds are inclusive") {
        s.collectTriggered(Price{91}, Price{99}, out);
        CHECK(out.empty());
        s.collectTriggered(Price{90}, Price{100}, out);
        REQUIRE(out.size() == 2);
        CHECK(out[0].id == 1); 
        CHECK(out[1].id == 2);
        CHECK(s.empty());
    }
    SECTION("bid side does not trigger buy stops and ask side does not trigger sell stops") {
        s.collectTriggered(Price{150}, Price{50}, out);  
        CHECK(out.empty());
    }
}

TEST_CASE("Scheduler releases in acceptance order, at most once", "[stop][scheduler]") {
    StopOrderScheduler s;
    s.add(10, Side::Buy, 105, 1);
    s.add(11, Side::Buy, 101, 1);
    s.add(12, Side::Sell, 95, 1);
    s.add(13, Side::Buy, 103, 1);
    std::vector<StopOrder> out;
    s.collectTriggered(Price{95}, Price{104}, out);  
    REQUIRE(out.size() == 3);
    CHECK(out[0].id == 11);  
    CHECK(out[1].id == 12);
    CHECK(out[2].id == 13);
    out.clear();
    s.collectTriggered(Price{95}, Price{104}, out);
    CHECK(out.empty());  
    CHECK(s.contains(10));
}

TEST_CASE("Cancelled stop can never trigger", "[stop][scheduler]") {
    StopOrderScheduler s;
    s.add(1, Side::Sell, 100, 7);
    const auto removed = s.cancel(1);
    REQUIRE(removed.has_value());
    CHECK(removed->quantity == 7);
    CHECK_FALSE(s.cancel(1).has_value());
    std::vector<StopOrder> out;
    s.collectTriggered(Price{1}, Price{2}, out);
    CHECK(out.empty());
}


TEST_CASE("Original demo scenario: buy stop triggers against a new ask", "[stop][engine]") {
    RecordingListener l;
    MatchingEngine e(&l);
    const auto stop = e.submit(OrderRequest::stop(30, Side::Buy, 150, 10));
    CHECK(stop.status == OrderStatus::PendingStop);
    CHECK(e.stops().contains(30));

    e.submit(OrderRequest::limit(31, Side::Sell, 155, 10));
    REQUIRE(l.triggered == std::vector<OrderId>{30});
    REQUIRE(l.trades.size() == 1);
    CHECK(l.trades[0] == Trade{30, 31, Side::Buy, 155, 10});
    REQUIRE(l.stopResults.size() == 1);
    CHECK(l.stopResults[0].status == OrderStatus::Filled);
    CHECK(e.book().empty());
    CHECK(e.stops().empty());
    requireValid(e.book());
}

TEST_CASE("Stop below the market does not trigger until reached", "[stop][engine]") {
    RecordingListener l;
    MatchingEngine e(&l);
    e.submit(OrderRequest::limit(1, Side::Sell, 100, 5));
    e.submit(OrderRequest::limit(2, Side::Sell, 110, 5));
    e.submit(OrderRequest::stop(3, Side::Buy, 105, 2));
    CHECK(l.triggered.empty());

    e.submit(OrderRequest::market(4, Side::Buy, 5));  
    REQUIRE(l.triggered == std::vector<OrderId>{3});
    REQUIRE(l.trades.size() == 2);
    CHECK(l.trades[1] == Trade{3, 2, Side::Buy, 110, 2});
    CHECK(e.book().find(2)->remaining == 3);
    requireValid(e.book());
}

TEST_CASE("Sell stop triggers when the bid falls; empty bid side does not trigger", "[stop][engine]") {
    RecordingListener l;
    MatchingEngine e(&l);
    e.submit(OrderRequest::stop(1, Side::Sell, 90, 5));
    CHECK(l.triggered.empty());  
    e.submit(OrderRequest::limit(2, Side::Buy, 95, 5));
    e.submit(OrderRequest::limit(3, Side::Buy, 90, 5));
    CHECK(l.triggered.empty());
    e.cancel(2); 
    REQUIRE(l.triggered == std::vector<OrderId>{1});
    REQUIRE(l.trades.size() == 1);
    CHECK(l.trades[0] == Trade{1, 3, Side::Sell, 90, 5});
    CHECK(e.book().empty());
    requireValid(e.book());
}

TEST_CASE("Stop placed through the market triggers immediately", "[stop][engine]") {
    RecordingListener l;
    MatchingEngine e(&l);
    e.submit(OrderRequest::limit(1, Side::Sell, 120, 5));
    const auto r = e.submit(OrderRequest::stop(2, Side::Buy, 100, 3));
    CHECK(r.status == OrderStatus::PendingStop);  
    CHECK(l.triggered == std::vector<OrderId>{2});
    CHECK(e.book().find(1)->remaining == 2);
}

TEST_CASE("Triggered stop with insufficient liquidity cancels its residual", "[stop][engine]") {
    RecordingListener l;
    MatchingEngine e(&l);
    e.submit(OrderRequest::stop(1, Side::Buy, 100, 10));
    e.submit(OrderRequest::limit(2, Side::Sell, 100, 4));
    REQUIRE(l.stopResults.size() == 1);
    CHECK(l.stopResults[0].status == OrderStatus::Cancelled);
    CHECK(l.stopResults[0].filled == 4);
    CHECK(l.stopResults[0].cancelled == 6);
    CHECK_FALSE(e.isLive(1));
}

TEST_CASE("Stops cascade deterministically", "[stop][engine]") {
    RecordingListener l;
    MatchingEngine e(&l);
    e.submit(OrderRequest::limit(1, Side::Sell, 100, 1));
    e.submit(OrderRequest::limit(2, Side::Sell, 101, 1));
    e.submit(OrderRequest::limit(3, Side::Sell, 102, 1));
    e.submit(OrderRequest::limit(4, Side::Sell, 103, 5));
    e.submit(OrderRequest::stop(20, Side::Buy, 102, 1));
    e.submit(OrderRequest::stop(21, Side::Buy, 101, 1));
    e.submit(OrderRequest::stop(22, Side::Buy, 103, 1));
    CHECK(l.triggered.empty());

    e.submit(OrderRequest::market(9, Side::Buy, 1));
    CHECK(l.triggered == std::vector<OrderId>{21, 20, 22});
    REQUIRE(l.trades.size() == 4);
    CHECK(l.trades[1] == Trade{21, 2, Side::Buy, 101, 1});
    CHECK(l.trades[2] == Trade{20, 3, Side::Buy, 102, 1});
    CHECK(l.trades[3] == Trade{22, 4, Side::Buy, 103, 1});
    CHECK(e.stops().empty());
    requireValid(e.book());
}

TEST_CASE("Simultaneously triggered stops release in acceptance order", "[stop][engine]") {
    RecordingListener l;
    MatchingEngine e(&l);
    e.submit(OrderRequest::limit(1, Side::Sell, 100, 50));
    e.submit(OrderRequest::limit(2, Side::Sell, 200, 50));
    e.submit(OrderRequest::stop(10, Side::Buy, 150, 1));  
    e.submit(OrderRequest::stop(11, Side::Buy, 120, 1));
    e.submit(OrderRequest::market(9, Side::Buy, 50));    
    CHECK(l.triggered == std::vector<OrderId>{10, 11});
}

TEST_CASE("Cancelled stop never triggers in the engine", "[stop][engine]") {
    RecordingListener l;
    MatchingEngine e(&l);
    e.submit(OrderRequest::stop(1, Side::Buy, 100, 5));
    const auto c = e.cancel(1);
    CHECK(c.status == OrderStatus::Cancelled);
    CHECK(c.cancelled == 5);
    e.submit(OrderRequest::limit(2, Side::Sell, 100, 5));
    CHECK(l.triggered.empty());
    CHECK(l.trades.empty());
    CHECK(e.cancel(1).reason == RejectReason::UnknownOrderId);
}

TEST_CASE("Engine-level id uniqueness and stop validation", "[stop][engine][validation]") {
    MatchingEngine e;
    e.submit(OrderRequest::limit(1, Side::Buy, 100, 5));
    e.submit(OrderRequest::stop(2, Side::Sell, 50, 5));
    CHECK(e.submit(OrderRequest::stop(1, Side::Sell, 50, 5)).reason == RejectReason::DuplicateOrderId);
    CHECK(e.submit(OrderRequest::limit(2, Side::Buy, 99, 5)).reason == RejectReason::DuplicateOrderId);
    CHECK(e.submit(OrderRequest::market(2, Side::Buy, 5)).reason == RejectReason::DuplicateOrderId);
    CHECK(e.submit(OrderRequest::stop(3, Side::Sell, 0, 5)).reason == RejectReason::InvalidStopPrice);
    CHECK(e.submit(OrderRequest::stop(3, Side::Sell, 50, 0)).reason == RejectReason::InvalidQuantity);
    CHECK(e.modify(2, 60, 5).reason == RejectReason::UnsupportedOperation);
    CHECK(e.book().orderCount() == 1);
    CHECK(e.stops().size() == 1);
}

TEST_CASE("Modify interacting with stops", "[stop][engine][modify]") {
    RecordingListener l;
    MatchingEngine e(&l);
    e.submit(OrderRequest::limit(1, Side::Buy, 100, 5));
    e.submit(OrderRequest::limit(2, Side::Buy, 95, 5));
    e.submit(OrderRequest::stop(3, Side::Sell, 96, 2));
    CHECK(l.triggered.empty());
    e.modify(1, 94, 5);
    REQUIRE(l.triggered == std::vector<OrderId>{3});
    REQUIRE(l.trades.size() == 1);
    CHECK(l.trades[0] == Trade{3, 2, Side::Sell, 95, 2});
    requireValid(e.book());
}

TEST_CASE("Command variant dispatch and reset", "[engine]") {
    MatchingEngine e;
    CHECK(e.process(Command{OrderRequest::limit(1, Side::Buy, 100, 5)}).status == OrderStatus::Resting);
    CHECK(e.process(Command{ModifyRequest{1, 101, 4}}).status == OrderStatus::Resting);
    CHECK(e.book().find(1)->price == 101);
    CHECK(e.process(Command{CancelRequest{1}}).status == OrderStatus::Cancelled);
    e.submit(OrderRequest::limit(2, Side::Buy, 100, 5));
    e.submit(OrderRequest::stop(3, Side::Buy, 500, 5));
    e.reset();
    CHECK(e.book().empty());
    CHECK(e.stops().empty());
    CHECK_FALSE(e.isLive(2));
    CHECK_FALSE(e.isLive(3));
}
