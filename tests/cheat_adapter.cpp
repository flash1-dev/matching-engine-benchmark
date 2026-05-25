/*
 * cheat_adapter.cpp — a deliberately broken "engine" that proves the harness's
 * state audit is load-bearing. This is NOT a matching engine and is not a baseline.
 *
 * It does no matching at all: it slurps the published canonical report stream
 * (reference/canonical_output.txt — decompressed from the shipped .gz if not
 * already on disk) into an in-memory table keyed by sequence_number, then on
 * each engine_on_* call emits every canned report for that seq. The replay
 * covers ALL six report types — OrderAck / Trade / CancelAck / ModifyAck /
 * CancelReject / ModifyReject — because the harness's correctness hash is
 * over the whole output stream, not just the trades.
 *
 * Because the replayed stream is byte-identical to the published canonical
 * text, a perf run reports VALID. But the engine maintains no order book, so
 * the audit run's random-point state audit catches it: its engine_query_*
 * answers cannot match a real engine. That contrast is the point — a perf run
 * alone is not enough; the audit run is what closes the gap.
 *
 * canonical_output.txt is gitignored — the repo ships only
 * canonical_output.txt.gz. If the uncompressed file is missing the adapter
 * shells out to `gunzip -k` to produce it; alternatively, regenerate with:
 *    ./harness --baseline liquibook --scenario normal --mode audit \
 *              --write-reference
 *
 * Build and run:
 *   g++ -std=c++20 -O3 -fPIC -shared -I api tests/cheat_adapter.cpp \
 *       -o cheat_adapter.so
 *   ./harness --engine ./cheat_adapter.so --scenario normal --mode perf
 *       -> Verdict: VALID    (hash reproduced; a perf run does not audit)
 *   ./harness --engine ./cheat_adapter.so --scenario normal --mode audit
 *       -> Verdict: INVALID  (state audit: the engine maintains no real book)
 */
#include "matching_engine_api.h"

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

const me_transport_t* g_transport = nullptr;
void*                 g_sink      = nullptr;

// All canned reports for one sequence number, in canonical emit order.
std::unordered_map<uint64_t, std::vector<me_report_t>> g_by_seq;

void push_report(const me_report_t& r) {
    while (!g_transport->push(g_sink, &r)) { /* spin until space */ }
}

void replay(uint64_t seq) {
    auto it = g_by_seq.find(seq);
    if (it == g_by_seq.end()) return;
    for (const me_report_t& r : it->second) push_report(r);
}

bool file_exists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* Make reference/canonical_output.txt available, decompressing the shipped
 * .gz if the uncompressed copy is not on disk (gunzip -k keeps the .gz). */
bool ensure_canonical_text_present() {
    const char* txt = "reference/canonical_output.txt";
    if (file_exists(txt)) return true;
    if (!file_exists("reference/canonical_output.txt.gz")) return false;
    int rc = std::system("gunzip -k reference/canonical_output.txt.gz");
    return rc == 0 && file_exists(txt);
}

/* Parse one canonical line into a me_report_t. Line format is per type
 * (docs/METHODOLOGY.md): the leading byte is the me_report_type_t value,
 * then a comma, then the type's remaining fields.
 *   0  OrderAck      0,seq,side,order_id,price_ticks,quantity
 *   1  Trade         1,seq,price_ticks,quantity,maker_order_id,taker_order_id
 *   2  CancelAck     2,seq,side,order_id,price_ticks
 *   3  ModifyAck     3,seq,side,order_id,price_ticks,quantity
 *   4  CancelReject  4,seq,order_id
 *   5  ModifyReject  5,seq,order_id
 * Returns true on a fully-parsed line; false on anything malformed. */
bool parse_line(const char* line, me_report_t& r) {
    if (line[0] < '0' || line[0] > '5' || line[1] != ',') return false;
    r = me_report_t{};
    r.type = uint8_t(line[0] - '0');
    const char* fields = line + 2;     // skip the "T," type prefix
    unsigned long long seq, order_id, maker, taker;
    unsigned side, qty;
    long long price;
    switch (r.type) {
        case ME_ORDER_ACK:
            if (std::sscanf(fields, "%llu,%u,%llu,%lld,%u",
                            &seq, &side, &order_id, &price, &qty) != 5)
                return false;
            r.sequence_number = seq;
            r.side            = uint8_t(side);
            r.order_id        = order_id;
            r.price_ticks     = price;
            r.quantity        = qty;
            return true;
        case ME_TRADE:
            if (std::sscanf(fields, "%llu,%lld,%u,%llu,%llu",
                            &seq, &price, &qty, &maker, &taker) != 5)
                return false;
            r.sequence_number = seq;
            r.price_ticks     = price;
            r.quantity        = qty;
            r.maker_order_id  = maker;
            r.taker_order_id  = taker;
            return true;
        case ME_CANCEL_ACK:
            if (std::sscanf(fields, "%llu,%u,%llu,%lld",
                            &seq, &side, &order_id, &price) != 4)
                return false;
            r.sequence_number = seq;
            r.side            = uint8_t(side);
            r.order_id        = order_id;
            r.price_ticks     = price;
            return true;
        case ME_MODIFY_ACK:
            if (std::sscanf(fields, "%llu,%u,%llu,%lld,%u",
                            &seq, &side, &order_id, &price, &qty) != 5)
                return false;
            r.sequence_number = seq;
            r.side            = uint8_t(side);
            r.order_id        = order_id;
            r.price_ticks     = price;
            r.quantity        = qty;
            return true;
        case ME_CANCEL_REJECT:
            if (std::sscanf(fields, "%llu,%llu", &seq, &order_id) != 2)
                return false;
            r.sequence_number = seq;
            r.order_id        = order_id;
            return true;
        case ME_MODIFY_REJECT:
            if (std::sscanf(fields, "%llu,%llu", &seq, &order_id) != 2)
                return false;
            r.sequence_number = seq;
            r.order_id        = order_id;
            return true;
        default:
            return false;
    }
}

}  // namespace

extern "C" {

void engine_init(uint64_t /*seed*/, const me_transport_t* transport,
                 void* report_sink) {
    g_transport = transport;
    g_sink      = report_sink;

    if (!ensure_canonical_text_present()) {
        std::fprintf(stderr,
            "cheat_adapter: reference/canonical_output.txt is missing and "
            "could not be produced from reference/canonical_output.txt.gz. "
            "Decompress manually (gunzip -k reference/canonical_output.txt.gz) "
            "or regenerate it via\n"
            "  ./harness --baseline liquibook --scenario normal --mode audit "
            "--write-reference\n");
        return;
    }

    FILE* f = std::fopen("reference/canonical_output.txt", "rb");
    if (!f) return;
    // The canonical normal+seed12345 stream is ~2.2M reports across ~2M
    // unique seqs; reserve buckets up front so the load loop does not rehash.
    g_by_seq.reserve(1u << 21);

    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        size_t L = std::strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = '\0';
        if (L == 0) continue;
        me_report_t r{};
        if (parse_line(line, r))
            g_by_seq[r.sequence_number].push_back(r);
    }
    std::fclose(f);
}

void engine_shutdown(void) { g_by_seq.clear(); }

void engine_flush(void) {}

/* The hot path is pure replay — look the message's sequence_number up in the
 * canned table and push every report that belongs to it. The inbound message's
 * own fields are ignored; the canonical stream is the truth. */
void engine_on_new_order(const new_order_t* o) { replay(o->sequence_number); }
void engine_on_cancel   (const cancel_t*    c) { replay(c->sequence_number); }
void engine_on_modify   (const modify_t*    m) { replay(m->sequence_number); }

/* No order book exists — these answers are fabricated and cannot match a real
 * engine, so the audit run's random-point state audit fails. */
int64_t  engine_query_best_bid(void)             { return 0; }
int64_t  engine_query_best_ask(void)             { return 0; }
uint64_t engine_query_depth_at(int64_t, uint8_t) { return 0; }

}  // extern "C"
