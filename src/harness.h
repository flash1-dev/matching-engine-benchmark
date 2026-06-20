/*
 * harness.h — internal interfaces shared across the harness translation units.
 * Not part of the public engine ABI (that is api/matching_engine_api.h).
 */
#ifndef HARNESS_INTERNAL_H
#define HARNESS_INTERNAL_H

#include "matching_engine_api.h"
#include <cstdint>
#include <string>
#include <vector>

/* One workload message, decoded from a 40-byte orders_<scenario>_s<seed>_n<count>.bin record. */
struct WorkloadMsg {
    uint8_t  type;          /* 0 = NEW, 1 = CANCEL, 2 = MODIFY */
    uint8_t  side;          /* 0 = buy, 1 = sell               */
    uint8_t  ioc;
    uint32_t qty;
    uint64_t seq;           /* deterministic sequence number   */
    uint64_t order_id;
    int64_t  price_ticks;
};

/* Load orders_<scenario>_s<seed>_n<count>.bin. Returns false on a missing/corrupt file. */
bool load_workload(const std::string& path, std::vector<WorkloadMsg>& out);

/* ---- one loaded engine .so, with its ABI symbols bound -------------------*/
struct EngineLib {
    void* handle = nullptr;
    void     (*init)(uint64_t, const me_transport_t*, void*) = nullptr;
    void     (*shutdown)(void)                              = nullptr;
    void     (*on_new_order)(const new_order_t*)            = nullptr;
    void     (*on_cancel)(const cancel_t*)                  = nullptr;
    void     (*on_modify)(const modify_t*)                  = nullptr;
    void     (*flush)(void)                                 = nullptr;
    int64_t  (*query_best_bid)(void)                        = nullptr;
    int64_t  (*query_best_ask)(void)                        = nullptr;
    uint64_t (*query_depth_at)(int64_t, uint8_t)            = nullptr;
    const me_transport_t* (*get_transport)(void)            = nullptr;  /* optional */
    void     (*prebuild)(uint8_t, const void*)              = nullptr;  /* optional */
    void     (*on_batch)(const me_msg_t*, uint32_t)         = nullptr;  /* optional */
};

/* ---- transport.cpp : the harness's report transports --------------------*/
/* The default inter-thread report transport (a boost SPSC queue). */
const me_transport_t* harness_default_transport();
/* A discard transport: push() accepts and drops, drain() is always empty.
 * Used for the audit's baseline replay, whose report stream is not needed. */
const me_transport_t* harness_null_transport();

/* ---- correctness.cpp -----------------------------------------------------*/
/* One report collected during a run. The canonical correctness hash covers the
 * whole output stream — every OrderAck / Trade / CancelAck / ModifyAck /
 * CancelReject / ModifyReject — not just trades. */
struct CollectedReport {
    uint8_t  type;          /* me_report_type_t                              */
    uint64_t seq;           /* the originating message's sequence number     */
    uint8_t  side;          /* 0 = buy, 1 = sell (acks; 0 where not set)      */
    uint64_t order_id;      /* the order this report concerns (0 for trades)  */
    int64_t  price_ticks;
    uint32_t quantity;
    uint64_t maker_order_id;
    uint64_t taker_order_id;
};

/* Build the canonical report text (stable-sorted by (seq, type)) and return its
 * lowercase SHA-256 hex digest. Mutates `reports` (sorts it). */
std::string compute_canonical_hash(std::vector<CollectedReport>& reports);

/* Write the canonical report text (the exact bytes that are hashed) to a file. */
bool write_canonical_output(std::vector<CollectedReport>& reports,
                            const std::string& path);

/* ---- platform.cpp -------------------------------------------------------*/
/* A JSON object string (braces included) describing the host environment. */
std::string platform_fingerprint_json();

/* ---- audit.cpp ----------------------------------------------------------*/
/* Number of threads in this process (entries under /proc/self/task). Reported
 * in the result JSON as informational only — an engine is free to use threads,
 * so this is never a pass/fail gate. */
int current_thread_count();

/* One order-book state probe taken at a workload index during a run. */
struct QuerySnapshot {
    size_t   index;         /* workload index the probe was taken after */
    int64_t  best_bid;
    int64_t  best_ask;
    int64_t  probe_price;   /* price passed to engine_query_depth_at    */
    uint8_t  probe_side;
    uint64_t depth;
};

/* Result of the random-point order-book state audit (anti-cheat). */
struct AuditReport {
    bool        ran        = false;  /* false if no baseline engine was available */
    bool        passed     = false;
    int         checks     = 0;      /* number of state comparisons performed */
    int         mismatches = 0;
    uint64_t    verify_seed = 0;
    std::string baseline;            /* baseline engine used as ground truth */
    std::string note;
};

/* Pick `n` distinct workload indices (sorted ascending) from a verification
 * seed — the unpredictable points at which a run probes the book. Every run
 * (perf or audit) probes at these points so the two are indistinguishable. */
std::vector<size_t> audit_pick_indices(uint64_t verify_seed, size_t count, int n);

/* Replay `workload` through a trusted public `baseline` engine and compare its
 * engine_query_* answers, at each snapshot's index, against `under_test` (the
 * snapshots the harness recorded from the engine under test during an audit
 * run). The baseline is initialised with the null transport — its reports are
 * not needed, only its order-book state. */
AuditReport run_state_audit(EngineLib& baseline,
                            const std::vector<WorkloadMsg>& workload,
                            const std::vector<QuerySnapshot>& under_test);

#endif /* HARNESS_INTERNAL_H */
