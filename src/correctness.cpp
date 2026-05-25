/*
 * correctness.cpp — canonical report serialization and SHA-256 verification.
 *
 * The harness verifies the engine's WHOLE output stream, not only its trades.
 * Canonical form: one report per line, stable-sorted by (sequence_number,
 * type), joined by '\n' with NO trailing newline. The line format is per type;
 * the leading field is the me_report_type_t value:
 *
 *   0  OrderAck      0,seq,side,order_id,price_ticks,quantity
 *   1  Trade         1,seq,price_ticks,quantity,maker_order_id,taker_order_id
 *   2  CancelAck     2,seq,side,order_id,price_ticks
 *   3  ModifyAck     3,seq,side,order_id,price_ticks,quantity
 *   4  CancelReject  4,seq,order_id
 *   5  ModifyReject  5,seq,order_id
 *
 * Sorting by (seq, type) makes the canonical order independent of the order in
 * which an engine happens to emit one message's reports — engines may interleave
 * acks and trades differently within a single message, and the (seq, type) sort
 * normalises that variation. Each workload message has a unique seq, so the
 * sort groups a message's reports together; stable_sort keeps the trades within
 * a message in emission (match) order. CancelAck omits quantity (an IOC
 * residual is implied by the trades and the OrderAck); the rejects carry only
 * identity, since the order is gone.
 *
 * The published correctness hash is the SHA-256 of those UTF-8 bytes.
 */
#include "harness.h"
#include "sha256.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>

namespace {

/* Stable-sort by (seq, type) and render the canonical text. */
std::string build_canonical_text(std::vector<CollectedReport>& reports) {
    std::stable_sort(reports.begin(), reports.end(),
        [](const CollectedReport& a, const CollectedReport& b) {
            if (a.seq != b.seq) return a.seq < b.seq;
            return a.type < b.type;
        });

    std::string text;
    text.reserve(reports.size() * 48);
    char line[160];
    bool first = true;
    for (const CollectedReport& r : reports) {
        int n = 0;
        switch (r.type) {
            case ME_ORDER_ACK:
                n = std::snprintf(line, sizeof(line), "0,%llu,%u,%llu,%lld,%u",
                    (unsigned long long)r.seq, (unsigned)r.side,
                    (unsigned long long)r.order_id, (long long)r.price_ticks,
                    (unsigned)r.quantity);
                break;
            case ME_TRADE:
                n = std::snprintf(line, sizeof(line), "1,%llu,%lld,%u,%llu,%llu",
                    (unsigned long long)r.seq, (long long)r.price_ticks,
                    (unsigned)r.quantity, (unsigned long long)r.maker_order_id,
                    (unsigned long long)r.taker_order_id);
                break;
            case ME_CANCEL_ACK:
                n = std::snprintf(line, sizeof(line), "2,%llu,%u,%llu,%lld",
                    (unsigned long long)r.seq, (unsigned)r.side,
                    (unsigned long long)r.order_id, (long long)r.price_ticks);
                break;
            case ME_MODIFY_ACK:
                n = std::snprintf(line, sizeof(line), "3,%llu,%u,%llu,%lld,%u",
                    (unsigned long long)r.seq, (unsigned)r.side,
                    (unsigned long long)r.order_id, (long long)r.price_ticks,
                    (unsigned)r.quantity);
                break;
            case ME_CANCEL_REJECT:
                n = std::snprintf(line, sizeof(line), "4,%llu,%llu",
                    (unsigned long long)r.seq, (unsigned long long)r.order_id);
                break;
            case ME_MODIFY_REJECT:
                n = std::snprintf(line, sizeof(line), "5,%llu,%llu",
                    (unsigned long long)r.seq, (unsigned long long)r.order_id);
                break;
            default:
                continue;   /* unknown report type — not part of the contract */
        }
        if (!first) text.push_back('\n');
        text.append(line, static_cast<size_t>(n));
        first = false;
    }
    return text;
}

}  // namespace

std::string compute_canonical_hash(std::vector<CollectedReport>& reports) {
    std::string text = build_canonical_text(reports);
    char hex[65];
    sha256_hex(text.data(), text.size(), hex);
    return std::string(hex);
}

bool write_canonical_output(std::vector<CollectedReport>& reports,
                            const std::string& path) {
    std::string text = build_canonical_text(reports);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = text.empty() ||
              std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}
