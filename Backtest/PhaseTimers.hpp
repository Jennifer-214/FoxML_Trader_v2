#pragma once
//======================================================================================================
// [FILE]_[Backtest/PhaseTimers.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[per-phase backtest wall-clock instrumentation — one process-wide PhaseTimer singleton, 8 phases from CSV parse to stamp emit; summary printed at run end]
// [CONTAINS]
//   - [STRUCT]_[PhaseTimer]
//   - [FUNCTION]_[PhaseTimer_Summary]   (PhaseTimer_Global / _NowNs / _Reset ride)
//   - [STRUCT]_[PhaseTimerSnapshot]   (PhaseTimer_PopulateSnapshot rides; UNWIRED at HEAD — TECH_DEBT-240)
//======================================================================================================
// v5.10.0 Item A — per-phase backtest timers.
//
// Instrumentation only; no behavior change. Phases instrumented:
//   parse           — CSV load (BacktestData_Load)
//   fan_out_hot     — tick loop fan_out + per-core hot path
//   feature_collect — on_slow_path hook (Features_PackAll)
//   label_compute   — Backtest_ComputeLabelsFromSamples
//   xgboost_train   — XGBooster fit (in WF + HeldOut)
//   wf_eval         — Backtest_RunWalkForward total
//   held_out_eval   — HeldOutSplit_TrainEval total
//   stamp_emit      — stamp_write_for_model
//
// Backtest is single-threaded; one global PhaseTimer instance is fine.
// Reset at run start via PhaseTimer_Reset (in BacktestSharded_Run); dump
// at run end via PhaseTimer_Summary (Backtest_Run + Backtest_RunFullValidation).
//
// Why a global rather than threading an arg through every site:
// BacktestSharded_Run + Backtest_ComputeLabelsFromSamples + WF + HeldOut
// + stamp emit live in 2-3 files with deep call chains. Plumbing a
// PhaseTimer* through every signature would touch ~15 files for an
// instrumentation-only change. Single-threaded backtest semantics
// make the global safe.
//======================================================================================================

#include <stdint.h>
#include <stdio.h>
#include <time.h>

namespace tt {

//======================================================================
// [STRUCT]_[PhaseTimer]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the 8 phase accumulators in ns + wall-clock total + populated flag — every field has a live writer in BacktestSharded_Run or BacktestEngine]
//======================================================================
// [CODE]
//======================================================================
struct PhaseTimer {
    uint64_t parse_ns;
    uint64_t fan_out_hot_ns;        // tick loop body (fan_out + hot path)
    uint64_t feature_collect_ns;
    uint64_t label_compute_ns;
    uint64_t xgboost_train_ns;
    uint64_t wf_eval_ns;            // includes nested xgboost_train_ns
    uint64_t held_out_eval_ns;      // includes nested xgboost_train_ns
    uint64_t stamp_emit_ns;
    uint64_t total_ns;              // wall-clock total (backtest end - start)
    int      populated;             // 1 = at least one phase recorded; 0 = idle/never-run
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[PhaseTimer]
//======================================================================

//======================================================================
// [FUNCTION]_[PhaseTimer_Summary]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[print the per-phase ms + percent table, skipping empty phases; PhaseTimer_Global / _NowNs / _Reset ride]
//======================================================================
// [CODE]
//======================================================================
// Process-wide instance via function-local static. Single-threaded
// backtest path; suite reuses across runs (Reset at start of each
// Backtest_Run / RunFullValidation). Function-local-static keeps the
// header inline-only (no separate .cpp).
static inline PhaseTimer& PhaseTimer_Global() {
    static PhaseTimer g{};
    return g;
}

static inline uint64_t PhaseTimer_NowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void PhaseTimer_Reset(PhaseTimer* pt) {
    pt->parse_ns           = 0;
    pt->fan_out_hot_ns     = 0;
    pt->feature_collect_ns = 0;
    pt->label_compute_ns   = 0;
    pt->xgboost_train_ns   = 0;
    pt->wf_eval_ns         = 0;
    pt->held_out_eval_ns   = 0;
    pt->stamp_emit_ns      = 0;
    pt->total_ns           = 0;
    pt->populated          = 0;
}

// Print summary to the given stream. Skips empty phases. Each row shows
// absolute ms + percent of total. xgboost_train is shown indented under
// wf_eval / held_out_eval since it's nested inside both.
static inline void PhaseTimer_Summary(const PhaseTimer* pt, FILE* fp) {
    if (!pt->populated || pt->total_ns == 0) {
        fprintf(fp, "[phase timer] (no phases recorded; total=0)\n");
        return;
    }
    double total_ms = pt->total_ns / 1.0e6;
    fprintf(fp, "[phase timer summary]\n");
    #define PHASE_ROW(label, ns) do { \
        if ((ns) > 0) { \
            double ms = (ns) / 1.0e6; \
            double pct = 100.0 * (double)(ns) / (double)pt->total_ns; \
            fprintf(fp, "  %-18s %8.1f ms  (%5.1f%%)\n", (label), ms, pct); \
        } \
    } while (0)
    PHASE_ROW("parse:",           pt->parse_ns);
    PHASE_ROW("fan_out_hot:",     pt->fan_out_hot_ns);
    PHASE_ROW("feature_collect:", pt->feature_collect_ns);
    PHASE_ROW("label_compute:",   pt->label_compute_ns);
    PHASE_ROW("wf_eval:",         pt->wf_eval_ns);
    PHASE_ROW("  xgboost_train:", pt->xgboost_train_ns);  // nested
    PHASE_ROW("held_out_eval:",   pt->held_out_eval_ns);
    PHASE_ROW("stamp_emit:",      pt->stamp_emit_ns);
    #undef PHASE_ROW
    fprintf(fp, "  %-18s %8.1f ms\n", "total:", total_ms);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[PhaseTimer_Summary]
//======================================================================

//======================================================================
// [STRUCT]_[PhaseTimerSnapshot]
//----------------------------------------------------------------------
// [TAG]_[[ENGINE] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[stable copy of the timer for GUI display, PhaseTimer_PopulateSnapshot rides — UNWIRED at HEAD, zero consumers; the Panels phase render reads the live singleton directly]
// [FUTURE_WORK]_[TECH_DEBT]_[TECH_DEBT-240]
//======================================================================
// [CODE]
//======================================================================
struct PhaseTimerSnapshot {
    uint64_t parse_ns;
    uint64_t fan_out_hot_ns;
    uint64_t feature_collect_ns;
    uint64_t label_compute_ns;
    uint64_t xgboost_train_ns;
    uint64_t wf_eval_ns;
    uint64_t held_out_eval_ns;
    uint64_t stamp_emit_ns;
    uint64_t total_ns;
    int      valid;  // 1 if at least one run completed; 0 = no data yet
};

static inline void PhaseTimer_PopulateSnapshot(const PhaseTimer* pt,
                                                PhaseTimerSnapshot* s) {
    s->parse_ns           = pt->parse_ns;
    s->fan_out_hot_ns     = pt->fan_out_hot_ns;
    s->feature_collect_ns = pt->feature_collect_ns;
    s->label_compute_ns   = pt->label_compute_ns;
    s->xgboost_train_ns   = pt->xgboost_train_ns;
    s->wf_eval_ns         = pt->wf_eval_ns;
    s->held_out_eval_ns   = pt->held_out_eval_ns;
    s->stamp_emit_ns      = pt->stamp_emit_ns;
    s->total_ns           = pt->total_ns;
    s->valid              = pt->populated;
}
//======================================================================
// [END_CODE]
//======================================================================
// [COMMENT]
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// [[2026-07-17] [v5.10.0]]
//----------------------------------------------------------------------
// Snapshot of a phase timer for GUI display — design intent: populate at
// run end via PhaseTimer_PopulateSnapshot so a renderer reads a stable
// copy (a mid-run read of the live PhaseTimer_Global() singleton can
// catch a half-updated counter). UNWIRED at HEAD (2026-07-17 rot-check):
// zero consumers for the struct AND the populate fn — the GUI
// phase-breakdown render reads PhaseTimer_Global() directly, the exact
// mid-run read this pair was built to prevent. Wire the render to a
// snapshot or delete the pair: TECH_DEBT-240.
//======================================================================
// [DERIVED]   (tool-refreshed — layout emitter cannot probe this block yet; quartet lands when the emitter covers it, D-327)
//======================================================================
// [END_STRUCT]_[PhaseTimerSnapshot]
//======================================================================

}  // namespace tt
