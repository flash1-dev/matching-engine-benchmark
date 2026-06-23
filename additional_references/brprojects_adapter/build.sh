#!/usr/bin/env bash
# Build brprojects_adapter.so. Clones brprojects/Limit-Order-Book at a pinned
# commit, applies one source patch (a per-fill notification hook — documented
# below), and compiles the engine's three matcher translation units + this
# adapter into a single .so at the harness repo root.
#
# Patch (applied AFTER `git reset --hard` to the pin, so the reset can never
# clobber it; idempotent and fails loud if an anchor drifts):
#
#   Per-fill trade hook (adapter instrumentation, Limit_Order_Book/Book.cpp).
#   The engine ships with NO trade/fill notification of any kind:
#   Book::marketOrderHelper executes and deletes resting orders silently and
#   only bumps an int counter. The harness needs one Trade per fill carrying
#   the maker's resting price + maker/taker ids, which is matcher information
#   only the engine sees, and re-deriving fills in the adapter would mean
#   reimplementing matching (forbidden by the adapter mandate). The patch
#   forward-declares `extern "C" void (*g_brp_fill_hook)(taker_id, maker_id,
#   maker_price, qty)` and inserts one call to it at each of marketOrderHelper's
#   two fill sites (the fully-consumed-maker loop and the partial-fill tail),
#   reading the maker id / maker price / fill qty BEFORE the engine mutates the
#   order. The adapter installs the hook in engine_init and clears it in
#   engine_shutdown; when unset (the engine's own standalone main) every call
#   site is a null-checked no-op, so the patch is inert outside the harness.
#   This is the "a hook the engine should call but doesn't" pattern also used by
#   jxm35_adapter and kautenja_adapter. The matching logic, prices, and
#   quantities are otherwise byte-identical to the pinned source. This is NOT a
#   correctness fix — the engine is consensus-conforming as shipped (see
#   CORRECTNESS_FINDINGS.md: "No fix required"); it only surfaces the per-fill
#   stream the engine never exposes.
#
# Override the upstream checkout: ME_BRPROJECTS_SRC=/path/to/existing/clone.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
TP="$REPO/third_party"
mkdir -p "$TP"

BRP_URL="https://github.com/brprojects/Limit-Order-Book.git"
BRP_REF="af6e5349874649fe196bd6c26653d357f5a751f2"

if [ -n "${ME_BRPROJECTS_SRC:-}" ]; then
    SRC="$ME_BRPROJECTS_SRC"
else
    SRC="$TP/brprojects_limit_order_book"
    if [ ! -d "$SRC/.git" ]; then
        git clone --quiet "$BRP_URL" "$SRC"
    fi
    git -C "$SRC" reset --hard --quiet "$BRP_REF"
fi

LOB="$SRC/Limit_Order_Book"
BOOK="$LOB/Book.cpp"

if [ ! -f "$BOOK" ]; then
    echo "error: engine source not found at $BOOK" >&2
    echo "       (set ME_BRPROJECTS_SRC to a brprojects/Limit-Order-Book checkout)" >&2
    exit 1
fi

# Apply the fill-hook patch (idempotent via str.replace on the pristine source;
# the git reset --hard above restores Book.cpp before each re-apply on the
# default checkout, and the marker check makes a re-run under an
# ME_BRPROJECTS_SRC override a no-op).
python3 - "$BOOK" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()

if "g_brp_fill_hook" in s:
    print("Book.cpp already patched (fill hook installed)")
    sys.exit(0)

HOOK_DEF = (
    '#include "Limit.hpp"\n'
    '\n'
    '// [harness patch] Per-fill notification hook. The engine ships with no\n'
    '// trade/fill callback; the adapter installs this to receive one call per\n'
    '// fill (taker id, maker id, maker resting price, filled qty). Null when the\n'
    '// engine runs standalone, so the call sites below are inert no-ops then.\n'
    'extern "C" void (*g_brp_fill_hook)(int taker_id, int maker_id, '
    'int maker_price, int qty) = nullptr;\n'
)
# Inject the hook definition right after the Limit.hpp include (unique anchor).
assert s.count('#include "Limit.hpp"\n') == 1, \
    "Limit.hpp include anchor not found exactly once (upstream changed?)"
s = s.replace('#include "Limit.hpp"\n', HOOK_DEF, 1)

# Full-fill site: hook each fully-consumed maker before execute() unlinks it.
FULL_OLD = ('        Order* headOrder = bookEdge->getHeadOrder();\n'
            '        shares -= headOrder->getShares();\n'
            '        headOrder->execute();\n')
FULL_NEW = ('        Order* headOrder = bookEdge->getHeadOrder();\n'
            '        if (g_brp_fill_hook) g_brp_fill_hook(orderId, '
            'headOrder->getOrderId(), bookEdge->getLimitPrice(), '
            'headOrder->getShares());  // [harness patch]\n'
            '        shares -= headOrder->getShares();\n'
            '        headOrder->execute();\n')
assert s.count(FULL_OLD) == 1, \
    "full-fill anchor not found exactly once (upstream changed?)"
s = s.replace(FULL_OLD, FULL_NEW, 1)

# Partial-fill site: hook the partially-filled maker at the tail.
PART_OLD = ('    if (bookEdge != nullptr && shares != 0)\n'
            '    {\n'
            '        bookEdge->getHeadOrder()->partiallyFillOrder(shares);\n')
PART_NEW = ('    if (bookEdge != nullptr && shares != 0)\n'
            '    {\n'
            '        if (g_brp_fill_hook) g_brp_fill_hook(orderId, '
            'bookEdge->getHeadOrder()->getOrderId(), bookEdge->getLimitPrice(), '
            'shares);  // [harness patch]\n'
            '        bookEdge->getHeadOrder()->partiallyFillOrder(shares);\n')
assert s.count(PART_OLD) == 1, \
    "partial-fill anchor not found exactly once (upstream changed?)"
s = s.replace(PART_OLD, PART_NEW, 1)

open(p, 'w').write(s)
print("patched Book.cpp: fill hook installed at 2 sites")
PY

cd "$DIR"
g++ -std=c++20 -O3 -march=native -fPIC -shared \
    -I"$REPO/api" \
    -I"$LOB" \
    -o "$REPO/brprojects_adapter.so" \
    brprojects_adapter.cpp \
    "$LOB/Book.cpp" \
    "$LOB/Limit.cpp" \
    "$LOB/Order.cpp"

echo "built: brprojects_adapter.so"
