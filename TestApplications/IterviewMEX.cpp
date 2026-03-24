// Market Data Ingestion — C++ Interview Exercise

//

// You are building a simplified market data gateway for a trading system.

// A feed simulator publishes pipe-delimited messages from a background thread.

// Your job:

//   1. Implement a thread-safe MessageQueue so the producer and consumer

//      can exchange messages without data races.

//   2. Implement MarketDataHandler to parse each message and maintain a

//      live snapshot of the latest quote per symbol, trade history, and

//      halt status — queryable safely from any thread.

//

// Message format (one per line):

//   Q|SYMBOL|BID_PX|BID_SZ|ASK_PX|ASK_SZ|TIMESTAMP_NS   — quote

//   T|SYMBOL|PRICE|SIZE|TIMESTAMP_NS                      — trade

//   H|TIMESTAMP_NS                                        — heartbeat (no state change)

//   S|SYMBOL|TRADING|TIMESTAMP_NS  or  S|SYMBOL|HALTED|…  — status

//

// Unknown message types and malformed lines: count them and skip — do not throw.

//

// Compile:

//   g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread -o marketdata marketdata.cpp

#include <atomic>

#include <chrono>

#include <condition_variable>

#include <functional>

#include <iomanip>

#include <iostream>

#include <mutex>

#include <optional>

#include <queue>

#include <shared_mutex>

#include <string>

#include <string_view>

#include <thread>

#include <unordered_map>

#include <unordered_set>

#include <vector>

// ─────────────────────────────────────────────────────────────────────────────

// Message types and structs  (provided — do not modify)

// ─────────────────────────────────────────────────────────────────────────────

enum class MsgType { Quote, Trade, Heartbeat, Status, Unknown };

struct Quote {

    std::string symbol;

    double      bidPx{ 0 }, askPx{ 0 };

    int64_t     bidSz{ 0 }, askSz{ 0 };

    int64_t     timestampNs{ 0 };

};

struct Trade {

    std::string symbol;

    double      price{ 0 };

    int64_t     size{ 0 };

    int64_t     timestampNs{ 0 };

};

struct StatusMsg {

    std::string symbol;

    bool        halted{ false };

    int64_t     timestampNs{ 0 };

};

// ─────────────────────────────────────────────────────────────────────────────

// MessageQueue

//

// A thread-safe queue of strings. The producer calls push(); the consumer

// calls pop(), which blocks until a message is available or the queue is shut down.

// ─────────────────────────────────────────────────────────────────────────────

class MessageQueue {

public:

    void push(std::string msg) {

        std::lock_guard lck(mu_);
        queue_.push(msg);
    }

    // Blocks until a message is available or the queue has been shut down.

    // Returns nullopt when the queue is drained and no more messages will arrive.

    std::optional<std::string> pop() {

        while (!done_ && queue_.empty()) {
            std::this_thread::yield();
        }

        if (done_) {
            return std::nullopt;
        }

        if (!queue_.empty()){
            std::lock_guard lck(mu_);

            std::optional<std::string> msg = queue_.front();
            queue_.pop();
            return msg;
        }

        return std::nullopt;
    }

    // Signal that no more messages will be pushed.

    // Unblocks any threads waiting in pop().

    void shutdown() {

        done_ = true;

    }

private:

    mutable std::mutex      mu_;

    std::condition_variable cv_;

    std::queue<std::string> queue_;

    std::atomic<bool> done_{ false };

};

// ─────────────────────────────────────────────────────────────────────────────

// MarketDataHandler

//

// Parses raw message lines and maintains state queryable from any thread.

// ingest() is called from the consumer thread only.

// ─────────────────────────────────────────────────────────────────────────────

void parse(std::vector < std::string> &result, const std::string& line) {
    std::string token;
    std::istringstream tokenStream(line);
    while (std::getline(tokenStream, token, '|'))
    {
        result.push_back(token);
    }
}

class MarketDataHandler {

public:

    // Register a callback fired on every successfully parsed quote update.

    // Must be called before the consumer thread starts.

    void subscribe(std::function<void(const Quote&)> cb) {

        subscribers_.push_back(std::move(cb));

    }

    // Parse one raw message line and update internal state.

    void ingest(std::string_view line) {

        // TODO(candidate)
        std::vector < std::string> result;
        parse(result, line.data());

        if (result.size() < 2) {
            ++malformedCount_;
            return;
        }

        if (result[0].size() != 1) {
            ++malformedCount_;
            return;
        }

        switch (result[0][0]) {
        case 'Q':
            if (result.size() != 7) {
                ++malformedCount_;
                return;
            }

            {
                Quote quote{
                    .symbol = result[1],
                    .bidPx = std::stod(result[2]),
                    .askPx = std::stod(result[4]),
                    .bidSz = std::stoll(result[3]),
                    .askSz = std::stoll(result[5]),
                    .timestampNs = std::stoll(result[6])
                };

                for (auto& callback : subscribers_) {
                    callback(quote);
                }

                products_[quote.symbol].quote.emplace(quote);

            }
            break;
        case 'H':
            if (result.size() != 2) {
                ++malformedCount_;
                return;
            }
            
            {
                int64_t timestampNs = std::stoll(result[1]);
            }
            break;
        case 'T':
            if (result.size() != 5) {
                ++malformedCount_;
                return;
            }
            {
                products_[result[1]].trades.emplace_back(result[1], std::stod(result[2]), std::stoll(result[3]), std::stoll(result[4]));
            }
            break;
        case 'S':
            if (result.size() != 4) {
                ++malformedCount_;
                return;
            }

            if (result[2] != "TRADING" && result[2] != "HALTED") {
                ++malformedCount_;
                return;
            }
            {
                StatusMsg status{
                    .symbol = result[1],
                    .halted = result[2] == "HALTED",
                    .timestampNs = std::stoll(result[3])
                };

                products_[status.symbol].status = status;
            }
            break;

        default:
            ++malformedCount_;
            return;                
        }
    }

    std::optional<Quote> getSnapshot(const std::string& sym) const {

        const auto it = products_.find(sym);

        if (it != products_.end()) {
            auto& productInfo = it->second;
            return productInfo.quote;
        }

        return std::nullopt;

    }

    double getMid(const std::string& sym) const {

        const auto it = products_.find(sym);

        if (it != products_.end()) {
            auto& productInfo = it->second;
            if (productInfo.quote) {
                const Quote& quote = productInfo.quote.value();
                return (quote.bidPx + quote.askPx) / 2.0;
            }
        }

        return 0.0;

    }

    bool isHalted(const std::string& sym) const {

        const auto it = products_.find(sym);

        if (it != products_.end()) {
            auto& productInfo = it->second;
            return productInfo.status.halted;
        }

        return false;

    }

    size_t malformedCount() const {

        return malformedCount_;
    }

    size_t tradeCount(const std::string& sym) const {

        const auto it = products_.find(sym);

        if (it != products_.end()) {
            auto& productInfo = it->second;
            return productInfo.trades.size();
        }

        return false;
    }

    // Extension: volume-weighted average price of all trades for sym

    double getVWAP(const std::string& sym) const {

        const auto it = products_.find(sym);

        if (it != products_.end()) {
            auto& productInfo = it->second;

            double sumPxSz = 0.0;
            size_t volume = 0;

            for (const auto& trade : productInfo.trades) {
                sumPxSz += trade.price * trade.size;
                volume += trade.size;
            }

            return sumPxSz / volume;
        }

        return 0.0;

    }

private:

    // TODO(candidate): declare your member variables

    size_t malformedCount_{ 0 };

    struct ProductInfo {
        std::optional<Quote> quote;
        std::vector<Trade> trades;
        StatusMsg status;
    };

    std::unordered_map<std::string, ProductInfo> products_;

    std::vector<std::function<void(const Quote&)>> subscribers_;

};

// ─────────────────────────────────────────────────────────────────────────────

// Parsing helpers — implement these

// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::string_view> split(std::string_view sv, char delim) {

    // TODO(candidate)

    (void)sv; (void)delim;

    return {};

}

static MsgType classifyLine(std::string_view line) {

    // TODO(candidate)

    (void)line;

    return MsgType::Unknown;

}

static std::optional<Quote> parseQuote(std::string_view line) {

    (void)line;

    return std::nullopt;

}

static std::optional<Trade> parseTrade(std::string_view line) {

    // TODO(candidate)

    (void)line;

    return std::nullopt;

}

static std::optional<StatusMsg> parseStatus(std::string_view line) {

    // TODO(candidate)

    (void)line;

    return std::nullopt;

}

// ─────────────────────────────────────────────────────────────────────────────

// Sample feed  (provided — do not modify)

// ─────────────────────────────────────────────────────────────────────────────

static constexpr const char* kFeedLines[] = {

    "Q|EURUSD|1.09500|1000000|1.09520|800000|1706789100000000000",

    "Q|GBPUSD|1.26800|500000|1.26820|400000|1706789100100000000",

    "T|EURUSD|1.09510|200000|1706789100200000000",

    "Q|EURUSD|1.09510|900000|1.09530|700000|1706789100300000000",

    "H|1706789100400000000",

    "Q|USDJPY|148.200|2000000|148.220|1500000|1706789100500000000",

    "T|GBPUSD|1.26810|100000|1706789100600000000",

    "Q|EURUSD|1.09520|850000|1.09540|650000|1706789100700000000",

    "S|USDJPY|HALTED|1706789100800000000",

    "Q|EURUSD|1.09530|800000|1.09550|600000|1706789100900000000",

    "BADMSG|garbage|here",

    "Q|GBPUSD|1.26820|450000|1.26840|350000|1706789101000000000",

    "T|EURUSD|1.09535|150000|1706789101100000000",

    "Q|EURUSD||1.09540|1706789101200000000",    // malformed — missing fields

};

// ─────────────────────────────────────────────────────────────────────────────

// Validation helpers  (provided — do not modify)

// ─────────────────────────────────────────────────────────────────────────────

static bool nearEq(double a, double b, double tol = 1e-5) {

    return std::abs(a - b) < tol;

}

static bool checkDouble(double got, double exp, const char* label, std::ostream& os) {

    if (!nearEq(got, exp)) {

        os << "[FAIL] " << label << ": expected=" << std::fixed << std::setprecision(5)
            << exp << " got=" << got << "\n";

        return false;

    }

    os << "[PASS] " << label << "\n";

    return true;

}

static bool checkBool(bool got, bool exp, const char* label, std::ostream& os) {

    if (got != exp) {

        os << "[FAIL] " << label << ": expected=" << (exp ? "true" : "false")
            << " got=" << (got ? "true" : "false") << "\n";

        return false;

    }

    os << "[PASS] " << label << "\n";

    return true;

}

static bool checkSize(size_t got, size_t exp, const char* label, std::ostream& os) {

    if (got != exp) {

        os << "[FAIL] " << label << ": expected=" << exp << " got=" << got << "\n";

        return false;

    }

    os << "[PASS] " << label << "\n";

    return true;

}

static void validateSnapshot(const MarketDataHandler& h, std::ostream& os) {

    os << "\n=== Validation ===\n";

    auto eu = h.getSnapshot("EURUSD");

    if (!eu) { os << "[FAIL] no EURUSD snapshot\n"; }

    else {

        checkDouble(eu->bidPx, 1.09530, "EURUSD bidPx", os);

        checkDouble(eu->askPx, 1.09550, "EURUSD askPx", os);

    }

    auto gb = h.getSnapshot("GBPUSD");

    if (!gb) { os << "[FAIL] no GBPUSD snapshot\n"; }

    else {

        checkDouble(gb->bidPx, 1.26820, "GBPUSD bidPx", os);

        checkDouble(gb->askPx, 1.26840, "GBPUSD askPx", os);

    }

    auto jp = h.getSnapshot("USDJPY");

    if (!jp) { os << "[FAIL] no USDJPY snapshot\n"; }

    else {

        checkDouble(jp->bidPx, 148.200, "USDJPY bidPx", os);

    }

    checkBool(h.isHalted("USDJPY"), true, "USDJPY halted", os);

    checkBool(h.isHalted("EURUSD"), false, "EURUSD not halted", os);

    checkDouble(h.getMid("EURUSD"), 1.09540, "EURUSD mid", os);

    checkSize(h.tradeCount("EURUSD"), 2, "EURUSD trade count", os);

    checkSize(h.tradeCount("GBPUSD"), 1, "GBPUSD trade count", os);

    checkSize(h.malformedCount(), 2, "malformed count", os);

    // VWAP(EURUSD) = (1.09510*200000 + 1.09535*150000) / 350000 ≈ 1.09521

    double expVwap = (1.09510 * 200000.0 + 1.09535 * 150000.0) / 350000.0;

    checkDouble(h.getVWAP("EURUSD"), expVwap, "EURUSD VWAP", os);

}

// ─────────────────────────────────────────────────────────────────────────────

// Main  (provided — do not modify)

// ─────────────────────────────────────────────────────────────────────────────

int main() {

    MessageQueue      queue;

    MarketDataHandler handler;

    // Register a subscriber — will fire on every valid quote update

    handler.subscribe([](const Quote& q) {

        std::cout << "  [sub] " << q.symbol
            << " bid=" << std::fixed << std::setprecision(5) << q.bidPx
            << " ask=" << q.askPx << "\n";

        });

    // Consumer thread: drain the queue and feed the handler

    std::thread consumer([&] {

        while (auto msg = queue.pop()) {

            handler.ingest(*msg);

        }

        });

    // Producer: push feed lines with tiny pacing to simulate a live stream

    std::cout << "=== Feed ===\n";

    for (const auto* line : kFeedLines) {

        std::cout << "  > " << line << "\n";

        queue.push(std::string(line));

        std::this_thread::sleep_for(std::chrono::microseconds(50));

    }

    queue.shutdown();

    consumer.join();

    validateSnapshot(handler, std::cout);

    return 0;

}

