#!/usr/bin/env bash
# Build pyxchange_adapter.so. Clones pavelschon/PyXchange at a pinned commit,
# applies a set of MINIMAL, DOCUMENTED engine patches, and compiles the matcher
# core + this adapter into a single .so at the harness repo root.
#
# PyXchange (https://github.com/pavelschon/PyXchange) is a limit-orderbook
# matching engine: a C++ Boost.MultiIndex price-time book (Order +
# OrderContainer + the OrderBook*.cpp match loop) under a Boost.Python / Twisted
# server. This adapter drives the C++ core directly and ignores the
# Python/Twisted layer. The matching algorithm (handleExecution / insertOrder /
# cancelOrder<>) and the data structure (OrderContainer, a
# boost::multi_index_container) are used UNMODIFIED.
#
# The patches fall in two buckets:
#
#   (A) Python-edge DECOUPLING (a build necessity, not a correctness change).
#       The C++ core is welded to the Python runtime through its I/O edges:
#       <boost/python.hpp> in PyXchange.hpp, py::dict order constructors, and
#       Python `logging` / per-trader notify in Logger / Trader / Client. To
#       build and drive the core with no interpreter we drop those edges and
#       drop in plain-typed, no-op replacements for the edge translation units,
#       and add non-Python entry points (newOrder / newOrderIOC / cancel +
#       best/depth queries) that dispatch into the EXISTING private templated
#       match workers. None of the match logic, prices, or quantities change.
#
#   (B) The ENGINE CORRECTNESS FIX ("with fix"; the one behaviour change). The
#       book's primary match-walk index is an ordered_unique on a composite
#       (price, time) key, where Order::time was stamped from
#       std::chrono::high_resolution_clock::now(). Two same-price orders that
#       land on an equal timestamp (clock resolution under a burst) collide on
#       the unique key: the second insert() returns .second == false and the
#       engine silently DROPS the order. The fix makes prio_t a strictly-
#       increasing process-wide uint64 FIFO counter — the FIFO time priority the
#       engine already intends — so equal-tick same-price orders get distinct
#       keys and none are dropped. Without it the adapter is INVALID (dropped
#       orders -> hash + state-audit mismatch); with it, VALID. See README.md
#       "Source patch" for the full description.
#
# Every patch is explained inline below and re-stated in the adapter header and
# README. Each is applied AFTER `git reset --hard` to the pin (so the reset can
# never clobber it), with loud-fail verbatim anchors, and is idempotent (a
# re-run, e.g. under ME_PYXCHANGE_SRC, restores originals first / no-ops).
#
# Override the upstream checkout: ME_PYXCHANGE_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

PYXCHANGE_URL="https://github.com/pavelschon/PyXchange.git"
PYXCHANGE_REF="b35f0ebeb8ce008e605987305a2d52194785fbb8"   # HEAD, "add const"

if [ -n "${ME_PYXCHANGE_SRC:-}" ]; then
    SRC="$ME_PYXCHANGE_SRC"
else
    SRC="$TP/pyxchange_PyXchange"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$PYXCHANGE_URL" "$SRC"
    fi
fi
git -C "$SRC" reset --hard --quiet "$PYXCHANGE_REF"

# C++ toolchain: this repo's C++ adapters use the system g++ (>= C++17) and the
# header-only Boost.MultiIndex from libboost-dev. No toolchain is auto-installed;
# fail loud if either is missing.
command -v g++ >/dev/null 2>&1 || { echo "g++ not found (need a C++17 compiler)" >&2; exit 1; }
[ -e /usr/include/boost/multi_index_container.hpp ] || \
    echo "warning: boost/multi_index_container.hpp not found under /usr/include; install libboost-dev" >&2

# ---------------------------------------------------------------------------
# Engine patches (idempotent: the reset --hard above restores originals first).
# ---------------------------------------------------------------------------
python3 - "$SRC" <<'PY'
import sys, pathlib
root = pathlib.Path(sys.argv[1]) / "src"

def patch(rel, transform):
    p = root / rel
    s = p.read_text()
    s2 = transform(s)
    p.write_text(s2)

# (B) ENGINE CORRECTNESS FIX + (A) Python decoupling, both in PyXchange.hpp:
#     drop <boost/python.hpp> so the core builds with no Python runtime; drop
#     the boost::python hasattr helper; and make prio_t a monotonic uint64 FIFO
#     counter instead of a wall-clock time_point. The book's primary index is
#     ordered_unique on (price,time); equal timestamps under a burst would make
#     the second same-price insert() fail and the order be dropped. A strictly-
#     increasing counter is the FIFO priority the engine intends and removes
#     that tie/drop hazard. (Matching logic is untouched.)
def fix_pyxchange_hpp(s):
    s = s.replace("#include <boost/python.hpp>\n", "")
    s = s.replace(
        "typedef std::chrono::time_point<std::chrono::high_resolution_clock>     prio_t;",
        "typedef std::uint64_t                                                   prio_t;")
    # Remove the hasattr helper (uses boost::python::object). Cut from its doc
    # comment through its closing brace.
    start = s.index("/**\n * @brief Check whether object has attribute")
    end = s.index("}", s.index("PyObject_HasAttrString")) + 1
    s = s[:start] + s[end:]
    return s
patch("PyXchange.hpp", fix_pyxchange_hpp)

# (A) Order.hpp / Order.cpp: replace the py::dict constructor + the py::dict
#     extract* statics with a plain-typed constructor. The matching-relevant
#     methods (comparePrice, getPrice, getTime, getId, getTrader, getUnique)
#     are unchanged; time is now assigned from the monotonic counter (see B).
def fix_order_hpp(s):
    s = s.replace(
"""                        Order( const TraderPtr& trader_,
                               const boost::python::dict& decoded,
                               const bool isMarketOrder_ );""",
"""                        Order( const TraderPtr& trader_,
                               const side_t side_, const orderId_t orderId_,
                               const price_t price_, const quantity_t quantity_,
                               const bool isMarketOrder_ );""")
    # Drop the py::dict extract* declarations.
    s = s.replace(
"""    static side_t       extractSide( const boost::python::dict& decoded );
    static orderId_t    extractOrderId( const boost::python::dict& decoded );
    static quantity_t   extractQuantity( const boost::python::dict& decoded );
    static price_t      extractPrice( const bool isMarketOrder_, const side_t side_,
                                      const boost::python::dict& decoded );
""", "")
    return s
patch("order/Order.hpp", fix_order_hpp)

def fix_order_cpp(s):
    # A fresh, minimal Order.cpp: plain constructor + the unchanged
    # match-relevant accessors. No Python. A process-wide monotonic counter
    # supplies strictly-increasing FIFO time priority.
    return r'''/**
 * @brief   Implementation of Order (non-Python core, adapter build)
 * @file    Order.cpp
 *
 * Copyright (c) 2016 Pavel Schon <pavel@schon.cz>
 */

#include "order/Order.hpp"
#include "client/Trader.hpp"
#include "utils/Side.hpp"

#include <atomic>

namespace pyxchange
{

namespace {
// Strictly-increasing FIFO time priority (replaces high_resolution_clock::now;
// see PyXchange.hpp / build.sh). Monotonic across the process.
std::atomic<std::uint64_t> g_time_seq{ 1 };
}

Order::Order( const TraderPtr& trader_, const side_t side_, const orderId_t orderId_,
              const price_t price_, const quantity_t quantity_, const bool isMarketOrder_ ):
      isMarketOrder{ isMarketOrder_ }
    , trader{ trader_ }
    , time{ g_time_seq.fetch_add( 1, std::memory_order_relaxed ) }
    , side{ side_ }
    , orderId{ orderId_ }
    , price{ price_ }
    , quantity{ quantity_ }
{

}

std::string Order::toString( void ) const
{
    return std::string();
}

bool Order::comparePrice( const OrderConstPtr& order ) const
{
    if( side::isBid( side ) && side::isAsk( order->side ) )
    {
        return price >= order->price;
    }
    else if( side::isAsk( side ) && side::isBid( order->side ) )
    {
        return price <= order->price;
    }
    else
    {
        return false;
    }
}

price_t     Order::getPrice( void ) const { return price; }
prio_t      Order::getTime( void )  const { return time; }
orderId_t   Order::getId( void )    const { return orderId; }
TraderPtr   Order::getTrader( void ) const { return trader.lock(); }
TraderOrderId Order::getUnique( void ) const { return std::make_tuple( trader.lock(), orderId ); }

} /* namespace pyxchange */
'''
patch("order/Order.cpp", fix_order_cpp)

# (A) OrderBook.hpp: declare the non-Python entry points used by the adapter.
#     They dispatch into the EXISTING private templated workers (insertOrder /
#     handleExecution / cancelOrder<>), so the match logic is reached unchanged.
def fix_orderbook_hpp(s):
    # Drop the three py::dict public entry points (their definitions were trimmed
    # from OrderBook.cpp / OrderBookCancel.cpp; the adapter uses the non-Python
    # entry points below instead).
    s = s.replace(
        "    void        createOrder( const TraderPtr& trader, const boost::python::dict& decoded );\n", "")
    s = s.replace(
        "    void        marketOrder( const TraderPtr& trader, const boost::python::dict& decoded );\n", "")
    s = s.replace(
        "    void        cancelOrder( const TraderPtr& trader, const boost::python::dict& decoded );\n", "")
    anchor = "    void        aggregateAllPriceLevels( const ClientPtr& client ) const;\n"
    inject = anchor + (
        "\n"
        "    // ---- non-Python harness entry points (adapter build) ----\n"
        "    // Insert a limit order: match crossing qty against the opposite book,\n"
        "    // rest the remainder. Returns the resting quantity (0 if fully filled).\n"
        "    quantity_t  newOrder( const TraderPtr& trader, const side_t side_,\n"
        "                          const orderId_t orderId, const price_t price,\n"
        "                          const quantity_t quantity );\n"
        "    // IOC: match crossing qty against the opposite book, never rest the\n"
        "    // remainder. Returns the filled quantity.\n"
        "    quantity_t  newOrderIOC( const TraderPtr& trader, const side_t side_,\n"
        "                             const orderId_t orderId, const price_t price,\n"
        "                             const quantity_t quantity );\n"
        "    // Cancel a resting order by (trader,orderId). Returns true if removed.\n"
        "    bool        cancel( const TraderPtr& trader, const orderId_t orderId );\n"
        "    // Top-of-book / depth queries (return false / 0 when empty).\n"
        "    bool        bestBid( int64_t& priceOut ) const;\n"
        "    bool        bestAsk( int64_t& priceOut ) const;\n"
        "    uint64_t    depthAt( const price_t price, const side_t side_ ) const;\n")
    return s.replace(anchor, inject)
patch("orderbook/OrderBook.hpp", fix_orderbook_hpp)

# (A) OrderBookExec.cpp: redirect the single per-fill hook notifyExecution() to
#     the adapter (pyx_on_trade). This is the engine's native "one call per
#     fill" point; handleExecution (the match loop) is UNCHANGED. The Python
#     per-trader / per-client notify path is dropped. Also drop the now-unused
#     client/Client.hpp include (its body pulled BaseClient -> boost::python).
def fix_orderbook_exec(s):
    s = s.replace('#include "client/Client.hpp"\n', "")
    old = '''void OrderBook::notifyExecution( const OrderConstPtr& order, const OrderConstPtr& oppOrder,
                                 const quantity_t matchQty ) const
{
    const TraderPtr& trader    = order->getTrader();
    const TraderPtr& oppTrader = oppOrder->getTrader();
    const price_t matchPrice   = oppOrder->price;

    logger.debug( format::f2::logExecution, matchQty, matchPrice );

    if( trader ) // it's created from weak_ptr, so we must check for nullptr
    {
        trader->notifyTrade( order->orderId, matchPrice, matchQty );
    }

    if( oppTrader ) // it's created from weak_ptr, so we must check for nullptr
    {
        oppTrader->notifyTrade( oppOrder->orderId, matchPrice, matchQty );
    }

    Client::notifyTrade( clients, order->time, matchPrice, matchQty );
}'''
    new = '''extern "C" void pyx_on_trade( uint64_t taker_id, uint64_t maker_id,
                              int64_t price, uint32_t qty );

void OrderBook::notifyExecution( const OrderConstPtr& order, const OrderConstPtr& oppOrder,
                                 const quantity_t matchQty ) const
{
    // order = aggressor (taker), oppOrder = resting (maker); fill at maker price.
    pyx_on_trade( static_cast<uint64_t>( order->orderId ),
                  static_cast<uint64_t>( oppOrder->orderId ),
                  static_cast<int64_t>( oppOrder->price ),
                  static_cast<uint32_t>( matchQty ) );
}'''
    assert old in s, "notifyExecution body not found verbatim"
    return s.replace(old, new)
patch("orderbook/OrderBookExec.cpp", fix_orderbook_exec)

# (A) Logger.cpp -> no-op (originally calls Python logging.getLogger()).
(root / "logger" / "Logger.cpp").write_text(r'''/**
 * @brief  Logger (no-op core, adapter build)
 * @file   Logger.cpp
 */
#include "logger/Logger.hpp"

namespace pyxchange
{
const std::string Logger::name = "pyxchange";
Logger::Logger() : logger() {}
void Logger::log( const std::string&, const boost::format& ) const {}
} /* namespace pyxchange */
''')

# (A) Logger.hpp keeps a boost::python::object member + a getLogger() decl. Strip
#     the Python bits so the no-op Logger.cpp links with no interpreter.
def fix_logger_hpp(s):
    s = s.replace("    static boost::python::object getLogger();\n", "")
    s = s.replace("    const boost::python::object logger;\n",
                  "    const int logger;\n")
    return s
patch("logger/Logger.hpp", fix_logger_hpp)

# (A) Client.hpp: replace the BaseClient-derived, Python-laden Client with a
#     minimal struct declaring only the two static market-data broadcast
#     functions OrderBookAggr.cpp / (historically) OrderBookExec.cpp reference.
#     They are no-ops (the clients vector is empty). The member notifyOrderBook
#     and the BaseClient base are dropped; OrderBookAggrAll.cpp (the only user of
#     the member form / aggregateAllPriceLevels) is excluded from the build.
(root / "client" / "Client.hpp").write_text(r'''/**
 * @brief  Client market-data broadcast (no-op core, adapter build)
 * @file   Client.hpp
 */
#ifndef CLIENT_HPP
#define CLIENT_HPP

#include "PyXchange.hpp"

namespace pyxchange
{

struct Client
{
    static void notifyOrderBook( const ClientVectorConstPtr& clients,
                                 const price_t priceLevel, const side_t side_,
                                 const quantity_t quantity );
    static void notifyTrade( const ClientVectorConstPtr& clients, const prio_t time,
                             const price_t price, const quantity_t quantity );
};

} /* namespace pyxchange */

#endif /* CLIENT_HPP */
''')

# (A) OrderBook.cpp / OrderBookCancel.cpp include utils/Exception.hpp, whose
#     inline raise()/translate() use boost::python and won't compile without it.
#     Neither file's RETAINED code uses Exception, so drop the include. Also trim
#     the py::dict public cancelOrder() from OrderBookCancel.cpp (it calls the
#     removed Order::extractOrderId and takes a py::dict); the templated
#     cancelOrder<> worker the adapter uses is kept verbatim.
patch("orderbook/OrderBook.cpp",
      lambda s: s.replace('#include "utils/Exception.hpp"\n', ""))

def fix_orderbook_cancel(s):
    s = s.replace('#include "utils/Exception.hpp"\n', "")
    s = s.replace("namespace py = boost::python;\n", "")
    # Remove the py::dict public cancelOrder( trader, decoded ) method.
    start = s.index("/**\n * @brief Cancel order from decoded message, notify trader on decoding error\n * @param trader canceling order")
    end = s.index("/**\n * @brief Cancel order from decoded message, notify trader on decoding error\n * @param orders")
    s = s[:start] + s[end:]
    # The removed public method was what implicitly instantiated the templated
    # cancelOrder<> worker for both books; OrderBookHarness.cpp (a separate TU)
    # now calls them, so force explicit instantiation here (as Insert/Exec do).
    s = s.replace("} /* namespace pyxchange */",
        "template size_t OrderBook::cancelOrder( const BidOrderContainerPtr& orders, const TraderPtr& trader, const orderId_t orderId );\n"
        "template size_t OrderBook::cancelOrder( const AskOrderContainerPtr& orders, const TraderPtr& trader, const orderId_t orderId );\n\n"
        "} /* namespace pyxchange */", 1)
    return s
patch("orderbook/OrderBookCancel.cpp", fix_orderbook_cancel)

print("patches applied")
PY

# ---------------------------------------------------------------------------
# Drop in the non-Python edge translation units (full files, auditable):
#   - a minimal Trader (identity only; notify* are no-ops — the report stream is
#     emitted by the adapter, trades via pyx_on_trade),
#   - a minimal Client (the market-data broadcast statics, no-op: the clients
#     vector is empty so they are never reached with work anyway),
#   - the harness entry points that dispatch into the private match workers.
# ---------------------------------------------------------------------------

cat > "$SRC/src/client/Trader.cpp" <<'EOF'
/**
 * @brief  Trader (identity-only core, adapter build)
 * @file   Trader.cpp
 *
 * The harness uses one synthetic Trader for every order; (trader,orderId)
 * collapses to orderId. notify* are no-ops — the adapter emits the harness
 * report stream itself (trades arrive via OrderBook::notifyExecution ->
 * pyx_on_trade). No Python.
 */
#include "client/Trader.hpp"

namespace pyxchange
{

void Trader::notifyPong() {}
void Trader::notifyError( const std::string& ) {}
void Trader::notifyCancelOrderSuccess( const orderId_t, const quantity_t ) {}
void Trader::notifyCreateOrderSuccess( const orderId_t, const quantity_t ) {}
void Trader::notifyTrade( const orderId_t, const price_t, const quantity_t ) {}

} /* namespace pyxchange */
EOF

# Trader.hpp: BaseClient pulls in the Python handler machinery. Replace Trader
# with a standalone identity class (name + no-op notify*). getTrader()/getUnique()
# return shared_ptr<Trader>, used by the book's (trader,orderId) index — pointer
# identity is all that is needed.
cat > "$SRC/src/client/Trader.hpp" <<'EOF'
/**
 * @brief  Trader (identity-only core, adapter build)
 * @file   Trader.hpp
 */
#ifndef TRADER_HPP
#define TRADER_HPP

#include "PyXchange.hpp"

namespace pyxchange
{

class Trader
{
public:
    explicit    Trader( const std::string& name_ ): name{ name_ } {}
                Trader( const Trader& ) = delete;
    Trader&     operator=( const Trader& ) = delete;

    std::string toString() const { return name; }

    void        notifyPong();
    void        notifyError( const std::string& text );
    void        notifyCancelOrderSuccess( const orderId_t orderId, const quantity_t quantity );
    void        notifyCreateOrderSuccess( const orderId_t orderId, const quantity_t quantity );
    void        notifyTrade( const orderId_t orderId, const price_t price, const quantity_t quantity );

private:
    const std::string name;
};

} /* namespace pyxchange */

#endif /* TRADER_HPP */
EOF

# Client.cpp: define the two market-data broadcast statics as no-ops (the
# clients vector is empty, so OrderBook never broadcasts real work). Declared in
# the patched (non-Python) Client.hpp above.
cat > "$SRC/src/client/Client.cpp" <<'EOF'
/**
 * @brief  Client market-data broadcast (no-op core, adapter build)
 * @file   Client.cpp
 */
#include "client/Client.hpp"

namespace pyxchange
{

void Client::notifyOrderBook( const ClientVectorConstPtr&, const price_t,
                              const side_t, const quantity_t ) {}
void Client::notifyTrade( const ClientVectorConstPtr&, const prio_t,
                          const price_t, const quantity_t ) {}

} /* namespace pyxchange */
EOF

# Harness entry points: dispatch into the existing private templated workers.
cat > "$SRC/src/orderbook/OrderBookHarness.cpp" <<'EOF'
/**
 * @brief  Non-Python harness entry points for OrderBook (adapter build)
 * @file   OrderBookHarness.cpp
 *
 * These thin members build a plain Order and dispatch into OrderBook's existing
 * private templated workers (insertOrder / handleExecution / cancelOrder<>), so
 * the match logic and the OrderContainer data structure are reached unchanged.
 */
#include "orderbook/OrderBook.hpp"
#include "order_container/OrderContainer.hpp"
#include "client/Trader.hpp"
#include "utils/Side.hpp"

namespace pyxchange
{

quantity_t OrderBook::newOrder( const TraderPtr& trader, const side_t side_,
                                const orderId_t orderId, const price_t price,
                                const quantity_t quantity )
{
    const OrderPtr order = std::make_shared<Order>( trader, side_, orderId, price, quantity, false );

    if( side::isBid( side_ ) )
    {
        insertOrder( bidOrders, askOrders, trader, order );
    }
    else
    {
        insertOrder( askOrders, bidOrders, trader, order );
    }

    return order->quantity;   // resting remainder (0 if fully filled / not rested)
}

quantity_t OrderBook::newOrderIOC( const TraderPtr& trader, const side_t side_,
                                   const orderId_t orderId, const price_t price,
                                   const quantity_t quantity )
{
    const OrderPtr order = std::make_shared<Order>( trader, side_, orderId, price, quantity, false );

    // Match against the opposite book only; never insert -> residual is dropped.
    if( side::isBid( side_ ) )
    {
        handleExecution( askOrders, order );
    }
    else
    {
        handleExecution( bidOrders, order );
    }

    return quantity - order->quantity;   // filled quantity
}

bool OrderBook::cancel( const TraderPtr& trader, const orderId_t orderId )
{
    // Order side is unknown here, so try both books (as the engine's own
    // cancelOrder does).
    size_t n = 0;
    n += cancelOrder( bidOrders, trader, orderId );
    n += cancelOrder( askOrders, trader, orderId );
    return n > 0;
}

bool OrderBook::bestBid( int64_t& priceOut ) const
{
    const auto& idx = bidOrders->container.get<tags::idxPrice>();
    if( idx.empty() ) return false;
    priceOut = static_cast<int64_t>( ( *idx.begin() )->getPrice() );
    return true;
}

bool OrderBook::bestAsk( int64_t& priceOut ) const
{
    const auto& idx = askOrders->container.get<tags::idxPrice>();
    if( idx.empty() ) return false;
    priceOut = static_cast<int64_t>( ( *idx.begin() )->getPrice() );
    return true;
}

uint64_t OrderBook::depthAt( const price_t price, const side_t side_ ) const
{
    uint64_t total = 0;
    if( side::isBid( side_ ) )
    {
        const auto& idx = bidOrders->container.get<tags::idxPrice>();
        const auto  end = idx.upper_bound( price );
        for( auto it = idx.lower_bound( price ); it != end; ++it )
            total += static_cast<uint64_t>( (*it)->quantity );
    }
    else
    {
        const auto& idx = askOrders->container.get<tags::idxPrice>();
        const auto  end = idx.upper_bound( price );
        for( auto it = idx.lower_bound( price ); it != end; ++it )
            total += static_cast<uint64_t>( (*it)->quantity );
    }
    return total;
}

} /* namespace pyxchange */
EOF

# ---------------------------------------------------------------------------
# Compile: matcher core + harness entry points + adapter -> one .so.
# OrderBook.cpp still contains createOrder/marketOrder (which reference py::dict)
# -> compile a trimmed copy without them. They are not used by the adapter.
# ---------------------------------------------------------------------------
python3 - "$SRC" <<'PY'
import sys, pathlib
root = pathlib.Path(sys.argv[1]) / "src"
p = root / "orderbook" / "OrderBook.cpp"
s = p.read_text()
# Keep only the constructor; drop createOrder/marketOrder (py::dict entry points
# superseded by the non-Python newOrder/newOrderIOC). Cut from the createOrder
# doc comment to just before the closing namespace.
start = s.index("/**\n * @brief Create order from decoded message")
end = s.index("} /* namespace pyxchange */")
s = s[:start] + s[end:]
# The constructor no longer needs the py alias / unused includes, but leaving the
# includes is harmless (they no longer pull Python after the PyXchange.hpp patch).
s = s.replace("namespace py = boost::python;\n", "")
p.write_text(s)
print("OrderBook.cpp trimmed")
PY

cd "$DIR"
g++ -std=c++17 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" \
    -I"$SRC/src" \
    -o "$REPO/pyxchange_adapter.so" \
    "$DIR/pyxchange_adapter.cpp" \
    "$SRC/src/order/Order.cpp" \
    "$SRC/src/client/Trader.cpp" \
    "$SRC/src/client/Client.cpp" \
    "$SRC/src/logger/Logger.cpp" \
    "$SRC/src/orderbook/OrderBook.cpp" \
    "$SRC/src/orderbook/OrderBookInsert.cpp" \
    "$SRC/src/orderbook/OrderBookExec.cpp" \
    "$SRC/src/orderbook/OrderBookCancel.cpp" \
    "$SRC/src/orderbook/OrderBookCancelAll.cpp" \
    "$SRC/src/orderbook/OrderBookAggr.cpp" \
    "$SRC/src/orderbook/OrderBookHarness.cpp"

echo "built: $REPO/pyxchange_adapter.so"
