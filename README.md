# Low-Latency Order Book

A price-time priority limit order book and matching engine in C++17, with
limit, market, IOC and stop-market orders, a deterministic single-threaded
core, and a thread-safe front end for concurrent order entry.

No third-party runtime dependencies. Catch2 v2.13.10 (single header) is vendored
for the tests.

## Architecture

```
 producer threads ──submit()──▶ AsyncMatchingEngine ──(one worker thread)──▶ MatchingEngine
                                (mutex-guarded queue,                        ├── OrderBook          (limit / market / IOC matching)
                                 batch swap)                                 └── StopOrderScheduler (pending stop orders)
                                                                              │
                                                     EngineListener ◀─────────┘  trades, stop triggers, results
```

| File | Responsibility |
|---|---|
| `include/orderbook/Types.hpp` | Vocabulary types: `OrderId`, `Price` (ticks), `Quantity`, `Side`, `OrderType`, requests, `Trade`, `ExecutionResult`, reject reasons |
| `include/orderbook/PriceConversion.hpp` | Decimal ⇄ tick conversion with on-grid validation |
| `include/orderbook/OrderBook.hpp`, `src/OrderBook.cpp` | Single-threaded book: price levels, FIFO queues, matching, cancel, modify, queries, `validate()` |
| `include/orderbook/StopOrderScheduler.hpp`, `src/StopOrderScheduler.cpp` | Passive store of pending stop orders; decides deterministically which stops a given top of book triggers |
| `include/orderbook/MatchingEngine.hpp`, `src/MatchingEngine.cpp` | Combines book and stops; validates commands, runs the stop cascade, publishes events to an `EngineListener` |
| `include/orderbook/AsyncMatchingEngine.hpp`, `src/AsyncMatchingEngine.cpp` | Thread-safe multi-producer front end with a single matching thread |
| `apps/main.cpp` | Demo (`orderbook_demo`): the original demo scenarios plus a multi-producer run |
| `tests/` | Catch2 tests, including a naive `ReferenceEngine` for randomized differential testing |
| `bench/bench_orderbook.cpp` | Benchmark (`orderbook_bench`): throughput and latency percentiles |
| `third_party/catch2/` | Catch2 v2.13.10 single header + Boost license |
| `tools/legacy_comparison/` | Optional, not built by CMake: harness comparing against the original implementation (needs TBB) |

## Prices and quantities

* Prices are **integer ticks** (`std::int64_t`), quantities are **integer lots**.
  Matching, level lookup and comparisons are exact; there is no floating-point
  equality anywhere in the engine.
* `toTicks(price, tickSize)` converts external decimal prices. It rejects
  non-finite input, a non-positive tick size, values outside ±2^53 ticks, and
  prices that are not on the tick grid (tolerance 1e-6 of a tick, which absorbs
  binary error such as `100.07 / 0.01`). Off-grid prices are rejected, never
  rounded. `toDecimal(ticks, tickSize)` converts back.
* Valid orders need `quantity > 0`, and a limit price `> 0` (Limit, IOC,
  modify) or a stop price `> 0` (Stop).

## Matching rules and order types

* **Price-time priority.** Better price first; within a price, earlier arrival first (FIFO).
* **Execution price** is always the **resting** order's price.
* Incoming orders are matched **synchronously on arrival**, so the book is never
  crossed between commands (`bestBid < bestAsk` whenever both exist).

| Type | Behaviour | Unfilled quantity |
|---|---|---|
| Limit | Matches against prices at or better than its limit | Rests in the book at its limit price |
| Market | Matches at any price (price field ignored) | Cancelled; never rests (`status = Cancelled`, `cancelled = residual`) |
| IOC | Matches against prices at or better than its limit | Cancelled; never rests |
| Stop (stop-market) | Waits in the `StopOrderScheduler`; when triggered it is released as a Market order with the same id | As Market |

**Stop triggers**, based on the best bid/ask (the reference the original code used):

* Buy stop triggers when a best ask exists and `bestAsk >= stopPrice`.
* Sell stop triggers when a best bid exists and `bestBid <= stopPrice`.
* An empty side never triggers anything.
* Triggers are checked after every successful command, including cancels and
  modifies, because these can move the best price. A stop placed "through the
  market" triggers immediately.
* All stops triggered by the same market state are released together, in
  acceptance order. Their executions can trigger more stops, so this repeats
  until nothing else triggers (the cascade).
* A stop is removed when it triggers, so it cannot trigger twice. A cancelled
  stop is removed immediately, so it can never trigger.
* Stop orders can be cancelled but not modified (`UnsupportedOperation`).

**Cancel** removes a resting order or a pending stop; the result reports the removed quantity.

**Modify** (`id, newPrice, newQuantity`) applies to resting limit orders; `newQuantity` is the new *remaining* quantity:

* Same price and quantity reduced (or unchanged): updated in place and **keeps time priority**.
* Any other change: **loses priority**. The order is removed and re-entered as
  a Limit order with the same id. If the new price crosses, it executes
  immediately as the aggressor and any residual rests.

**Order ids** must be unique among *live* orders (resting orders plus pending
stops). Duplicates are rejected. An id can be reused once its order is filled
or cancelled.

**Results.** Every command returns an `ExecutionResult`
(`status`, `reason`, `filled`, `resting`, `cancelled`) describing that command.
Trades, stop triggers and stop executions go to the `EngineListener` in
execution order. For a Stop submission the result is the acceptance
(`PendingStop`), even if the stop triggers within the same call.

## Data structures and complexity

| Structure | Choice | Why |
|---|---|---|
| Price levels | `std::map<Price, Level>` per side (bids `greater`, asks `less`) | Sorted, unbounded price range; best price is `begin()` |
| Orders in a level | Intrusive doubly-linked FIFO over a node pool (32-bit slot indices) | O(1) append, O(1) removal anywhere in the queue, no per-order heap allocation in steady state |
| Order lookup | `std::unordered_map<OrderId, slot>` | O(1) average cancel/modify lookup |
| Each node | Stores a `Level*` (map nodes are address-stable) | Cancel needs no map search unless the level empties |
| Pending stops | `std::map<(stopPrice, seq), StopOrder>` per side + `unordered_map` index | Ordered trigger scan, O(1) "anything triggered?" check |

With `L` = price levels on a side, `S` = pending stops, `k` = orders or stops touched:

| Operation | Cost |
|---|---|
| Best bid / ask | O(1) |
| Add, no new level | O(1) average |
| Add, new level | O(log L) |
| Cancel | O(1) average, + O(log L) if the level empties |
| Modify, size-down in place | O(1) average |
| Modify, otherwise | cancel + add |
| Match | O(k) fills + O(log L) per emptied level |
| Stop check after a command (nothing triggers) | O(1) |
| Stop add / cancel | O(log S) |
| Stop release of `k` stops | O(k log S + k log k) |

Trade-offs: `std::map` allocates a node per new price level; a dense
price-indexed array would make level operations O(1) but needs a bounded price
range. `std::unordered_map` allocates per inserted id; an open-addressing table
would remove that allocation. Neither change was needed to meet the goals
here; both are listed under future work.

## Concurrency model

* `OrderBook`, `StopOrderScheduler` and `MatchingEngine` are **not thread-safe**.
  Each is owned by exactly one thread. Being single-writer is what makes
  matching deterministic and lock-free in the hot path.
* `AsyncMatchingEngine` is the **thread-safe** front end:
  * `submit()` may be called from any number of threads. Commands enter one
    mutex-protected queue, and the order in which they enter it is the
    processing order. Each producer's own commands are processed in the order
    it submitted them.
  * A single worker thread swaps out the whole queue (batching) and applies the
    commands to its `MatchingEngine`. Listener callbacks run on that thread.
  * `sync()` blocks until everything submitted before the call has been
    processed. `inspect(fn)` runs `fn(const MatchingEngine&)` between commands,
    so it always sees a consistent book.
  * `stop()` (also run by the destructor) drains queued commands and joins the
    worker. `submit()` after `stop()` returns `false`.
  * Lock rules: the queue mutex and the engine mutex are never held together.
    Listeners may call `submit()` but must not call `sync()`, `inspect()` or
    `stop()`, and should not throw. If the worker throws, the engine stops
    accepting work and the exception is rethrown from `sync()` / `stop()`.
* No lock-free data structures or custom atomics are used. Throughput is
  bounded by the one matching thread, which is intentional: matching is a
  serial process.

What the tests verify: the concurrent run is linearizable (replaying the
observed command order single-threaded reproduces every result, trade and the
final book); per-producer FIFO; `inspect()` always sees a valid book;
concurrent cancels of the same order succeed exactly once; shutdown drains
commands; worker exceptions are propagated. The whole suite passes under
ThreadSanitizer and AddressSanitizer + UBSan.

## Requirements

* A C++17 compiler. Verified with GCC 13.3 and Clang 18.1 (with both libstdc++ and
  libc++). Apple Clang has not been run, but any C++17-capable Xcode should work.
* CMake ≥ 3.16.
* macOS setup: `xcode-select --install` (compiler) and `brew install cmake`.
* No other libraries.

## Build, test, run

From the project root:

```bash
# 1. Configure (Release is the default for single-config generators)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 2. Compile
cmake --build build -j

# 3. Run the tests
ctest --test-dir build --output-on-failure     # CMake >= 3.20
./build/orderbook_tests                         # or run Catch2 directly
./build/orderbook_tests "[stop]"                # a subset, by tag

# 4. Run the demo
./build/orderbook_demo

# 5. Run the benchmark (use a Release build on an idle machine)
./build/orderbook_bench            # full run, about 1 minute
./build/orderbook_bench --quick    # smaller, a few seconds
```

Options: `-DORDERBOOK_BUILD_TESTS=OFF`, `-DORDERBOOK_BUILD_BENCHMARKS=OFF`,
`-DORDERBOOK_BUILD_DEMO=OFF`, `-DORDERBOOK_WARNINGS_AS_ERRORS=ON`.

Sanitizer builds (Clang / GCC):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DORDERBOOK_SANITIZE=address,undefined
cmake --build build-asan -j && ./build-asan/orderbook_tests

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DORDERBOOK_SANITIZE=thread
cmake --build build-tsan -j && ./build-tsan/orderbook_tests
```

## Tests

| File | Covers |
|---|---|
| `test_order_book.cpp` | Empty book, best bid/ask, price-time priority, resting execution price, partial and full fills, level cleanup, market, IOC, cancel, modify (priority rules, crossing modify), invalid input, id reuse, boundary values, pool recycling |
| `test_stop_orders.cpp` | Scheduler validation, trigger sides and inclusivity, empty-book behaviour, release order, no double trigger, cancelled stops, engine integration, cascades, triggers caused by cancel and modify |
| `test_randomized.cpp` | Differential test against `ReferenceEngine` (naive vectors and linear scans): 20 seeds × 3,000 commands plus one 30,000-command run. Every command's result, trades, the full book, the pending stops and `validate()` are compared |
| `test_concurrency.cpp` | The `AsyncMatchingEngine` guarantees listed above |
| `test_price_conversion.cpp` | Tick conversion rules |

The randomized command streams depend on `std::uniform_int_distribution`,
which is implementation-defined. libstdc++ (GCC) and libc++ (Apple Clang)
therefore produce different but equally valid streams; each one is
reproducible on its platform.

## Benchmark

`orderbook_bench` pre-generates all commands with fixed seeds and runs each
workload once as a warm-up. It then does 3 throughput runs (whole loop timed,
best run reported) and 3 latency runs (every command timed with
`steady_clock`, samples pooled). The clock overhead (~20 ns per pair here) is
included in the latency figures, not subtracted.

Measured on 2026-10-23 in a **shared 2-vCPU Linux cloud VM** (Intel Xeon @ 2.80 GHz,
GCC 13.3, `-O3 -DNDEBUG`). This is not your Mac, so expect different absolute
numbers; re-run it locally.

| Workload | Ops | Throughput | p50 | p95 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|---:|
| insert_passive | 200k | 9.7 M/s | 212 ns | 274 ns | 390 ns | 2.3 µs |
| cancel_random | 200k | 1.6 M/s | 697 ns | 1.1 µs | 1.3 µs | 20 µs |
| modify | 200k | 2.2 M/s | 556 ns | 973 ns | 1.2 µs | 5.5 µs |
| aggressive_match | 200k | 3.4 M/s | 163 ns | 859 ns | 1.2 µs | 1.9 µs |
| mixed | 1M | 9.0 M/s | 136 ns | 297 ns | 486 ns | 1.8 µs |
| burst | 100k | 1.8 M/s | 602 ns | 1.5 µs | 2.0 µs | 7.2 µs |

AsyncMatchingEngine end-to-end (mixed stream): 8.1 M commands/s with 1
producer, 7.7 M/s with 2, 6.6 M/s with 4. More producers than cores means
contention on the queue mutex.

Caveats: this VM is noisy. An earlier identical run measured insert_passive
p50 at 134 ns, and p99.9 values move by 2–4x between runs. Maximum latencies
(up to 3.2 ms) come from VM scheduling and page faults and are not meaningful
here. Random cancel and modify over 200k live orders are dominated by cache
misses. "mixed" includes 19% commands rejected because they target orders that
are already gone (realistic for cancel-after-fill races), and those are cheap.


## Known limitations and future work

* One instrument per engine. No sessions, persistence, or market-data feed.
* No self-trade prevention, iceberg/hidden, post-only, stop-limit or GTD orders.
* Stops trigger on best bid/ask, not last trade price. Switching to last-trade
  triggers would be a small change in `MatchingEngine::runStopCascade`.
* Under `std::bad_alloc` the book stays internally consistent (basic guarantee),
  but a cancel/replace modify can lose the order being modified. A listener that
  throws during a stop cascade can drop stops that had already triggered.
* Possible optimisations, not done because nothing here required them: a dense
  price-ladder array for bounded price ranges, an open-addressing id map, and an
  SPSC/MPSC ring buffer in place of the mutex queue.
