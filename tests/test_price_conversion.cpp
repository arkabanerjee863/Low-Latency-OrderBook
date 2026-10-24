#include <cmath>
#include <limits>

#include "catch.hpp"
#include "orderbook/PriceConversion.hpp"

using namespace orderbook;

TEST_CASE("toTicks converts on-grid prices exactly", "[price]") {
    CHECK(toTicks(100.0, 0.01) == Price{10000});
    CHECK(toTicks(100.07, 0.01) == Price{10007});  
    CHECK(toTicks(0.01, 0.01) == Price{1});
    CHECK(toTicks(0.0, 0.01) == Price{0});
    CHECK(toTicks(-1.5, 0.5) == Price{-3});
    CHECK(toTicks(12.25, 0.25) == Price{49});
}

TEST_CASE("toTicks rejects off-grid and invalid input", "[price]") {
    CHECK_FALSE(toTicks(100.005, 0.01).has_value());
    CHECK_FALSE(toTicks(1.0, 0.0).has_value());
    CHECK_FALSE(toTicks(1.0, -0.01).has_value());
    CHECK_FALSE(toTicks(std::numeric_limits<double>::quiet_NaN(), 0.01).has_value());
    CHECK_FALSE(toTicks(std::numeric_limits<double>::infinity(), 0.01).has_value());
    CHECK_FALSE(toTicks(1e300, 1e-10).has_value());  
}

TEST_CASE("toDecimal round-trips", "[price]") {
    for (Price t : {Price{1}, Price{999}, Price{10007}, Price{123456789}}) {
        const auto back = toTicks(toDecimal(t, 0.01), 0.01);
        REQUIRE(back.has_value());
        CHECK(*back == t);
    }
}
