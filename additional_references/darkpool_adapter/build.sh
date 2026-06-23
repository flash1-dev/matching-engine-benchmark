#!/usr/bin/env bash
# Build darkpool_adapter.so. Clones dendisuhubdy/dark_pool at the pinned commit,
# applies one engine correctness patch to the ordermatch matcher (the maker-price
# fill fix documented below and filed upstream as dark_pool#1), and compiles its
# `ordermatch` Market source + this adapter into a single .so at the harness repo
# root.
#
# The engine that actually compiles and runs in dark_pool is the QuickFIX-style
# `ordermatch` book in src/ordermatch/ (Market.cpp + Order.h): a price/time-
# priority limit order book over two std::multimaps. The repo's README mentions
# Liquibook, but Liquibook is not what builds here.
#
# Patch (applied AFTER `git reset --hard` to the pin, so the reset can never
# clobber it; idempotent and fails loud if its anchors drift). This adapter is
# classified "with fix": the patch is the engine correctness fix and is applied
# unconditionally, so darkpool_adapter.so is the fixed engine.
#
# Override the upstream checkout: ME_DARKPOOL_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

DARKPOOL_URL="https://github.com/dendisuhubdy/dark_pool.git"
DARKPOOL_REF="92bc3382bda9375829a2267ac3e96a80802b60cf"

if [ -n "${ME_DARKPOOL_SRC:-}" ]; then
    SRC="$ME_DARKPOOL_SRC"
else
    SRC="$TP/dendisuhubdy_dark_pool"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$DARKPOOL_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$DARKPOOL_REF"
fi
# Restore a pristine ordermatch tree before patching, so a leftover patched
# checkout (e.g. under an ME_DARKPOOL_SRC override) cannot make the anchor checks
# below fail. The git reset above already does this on the default clone; this
# also drops any untracked engine artifacts.
git -C "$SRC" clean -fdq -- src/ordermatch 2>/dev/null || true

# --- ENGINE CORRECTNESS FIX -----------------------------------------------
# Filed upstream: https://github.com/dendisuhubdy/dark_pool/issues/1
#
# Market::match(Order& bid, Order& ask) prices every fill unconditionally at the
# ask (`double price = ask.getPrice();`, Market.cpp:109), so when a resting BUY
# sits above an incoming SELL the print is the aggressor's lower price, not the
# resting maker's. Under price-time priority the resting (maker) order sets the
# execution price; the price improvement belongs to whichever side improved.
#
# The fix prices each fill at the MAKER's price. The maker is the order already
# resting when the aggressor arrived — i.e. the side opposite the just-inserted
# aggressor. The engine already learns the aggressor's side in Market::insert()
# (the only order inserted before each match() pass), so we record it there
# (m_lastInsertedSide) and price the inner match() at the resting side. The
# PUBLIC API is unchanged — match(queue&) keeps its signature, so this is a pure
# engine-internal correction. On the existing (accidentally-correct) aggressive-
# buy path it is a no-op, since there the ask already is the maker.
python3 - "$SRC/src/ordermatch/Market.h" "$SRC/src/ordermatch/Market.cpp" <<'PY'
import sys
hpath, cpath = sys.argv[1], sys.argv[2]

# --- Market.h: remember the aggressor (last-inserted) side as a member ---
h = open(hpath).read()
if "m_lastInsertedSide" in h:
    print("Market.h already patched (m_lastInsertedSide)")
else:
    anchor = "  std::queue < Order > m_orderUpdates;"
    if anchor not in h:
        sys.exit("darkpool build.sh maker-price patch: m_orderUpdates member "
                 "anchor not found in Market.h (upstream changed?)")
    h = h.replace(
        anchor,
        "  Order::Side m_lastInsertedSide;   // side of the just-inserted aggressor\n"
        + anchor)
    open(hpath, "w").write(h)
    print("patched Market.h: m_lastInsertedSide member")

# --- Market.cpp: record the side in insert(); price the fill at the maker ---
c = open(cpath).read()
if "m_lastInsertedSide" in c:
    print("Market.cpp already patched (maker-priced fill)")
    sys.exit(0)

# 1) record the aggressor side at the end of insert()
ins_old = (
    "bool Market::insert( const Order& order )\n"
    "{\n"
    "  if ( order.getSide() == Order::buy )\n"
    "    m_bidOrders.insert( BidOrders::value_type( order.getPrice(), order ) );\n"
    "  else\n"
    "    m_askOrders.insert( AskOrders::value_type( order.getPrice(), order ) );\n"
    "  return true;\n"
    "}\n")
ins_new = (
    "bool Market::insert( const Order& order )\n"
    "{\n"
    "  m_lastInsertedSide = order.getSide();   // remember the aggressor's side\n"
    "  if ( order.getSide() == Order::buy )\n"
    "    m_bidOrders.insert( BidOrders::value_type( order.getPrice(), order ) );\n"
    "  else\n"
    "    m_askOrders.insert( AskOrders::value_type( order.getPrice(), order ) );\n"
    "  return true;\n"
    "}\n")
if ins_old not in c:
    sys.exit("darkpool build.sh maker-price patch: insert() body anchor not "
             "found verbatim in Market.cpp (upstream changed?)")
c = c.replace(ins_old, ins_new)

# 2) price the inner match() at the maker (the side opposite the aggressor)
m_old = (
    "void Market::match( Order& bid, Order& ask )\n"
    "{\n"
    "  double price = ask.getPrice();\n")
m_new = (
    "void Market::match( Order& bid, Order& ask )\n"
    "{\n"
    "  // Price at the maker (resting) order: the side opposite the aggressor.\n"
    "  // The aggressor is the order just inserted before this match pass; the\n"
    "  // resting maker sets the execution price under price-time priority.\n"
    "  double price = ( m_lastInsertedSide == Order::buy )\n"
    "                 ? ask.getPrice()   // aggressor bought -> ask is the maker\n"
    "                 : bid.getPrice();  // aggressor sold   -> bid is the maker\n")
if m_old not in c:
    sys.exit("darkpool build.sh maker-price patch: match(bid,ask) prologue "
             "anchor not found verbatim in Market.cpp (upstream changed?)")
c = c.replace(m_old, m_new)
open(cpath, "w").write(c)
print("patched Market.cpp: maker-priced fill (m_lastInsertedSide)")
PY

# C++ toolchain: this repo's C++ adapters use the system g++. No toolchain is
# auto-installed; fail loud if g++ is missing.
command -v g++ >/dev/null 2>&1 || { echo "g++ not found (need a C++17 compiler)" >&2; exit 1; }

cd "$DIR"
g++ -std=c++17 -O3 -march=native -fPIC -shared \
    -I"$SRC/src/ordermatch" -I"$REPO/api" \
    -o "$REPO/darkpool_adapter.so" \
    darkpool_adapter.cpp \
    "$SRC/src/ordermatch/Market.cpp" \
    -pthread
echo "built: darkpool_adapter.so"
