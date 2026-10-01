#pragma once
//======================================================================================================
// [FILE]_[Backtest/TrainingWorkers.hpp]
//------------------------------------------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ML producer core (E.1.3 MP-1) — the multi-horizon orchestrator, its per-horizon train + validate + record job, and the Run Full Validation job, ImGui-free: a request in, a sink for display, a result out, so the suite's panel and a headless caller run the same code]
// [CONTAINS]
//   - [STRUCT]_[TrainingHorizonSink]
//   - [STRUCT]_[TrainingHorizonDisplay]
//   - [STRUCT]_[TrainingRunSink]
//   - [STRUCT]_[TrainingRunRequest]
//   - [STRUCT]_[TrainingHorizonRequest]
//   - [STRUCT]_[TrainingHorizonOutcome]
//   - [STRUCT]_[TrainingRunResult]
//   - [STRUCT]_[TrainingHorizonJob]
//   - [STRUCT]_[TrainingFvRequest]
//   - [STRUCT]_[TrainingFvSink]
//   - [FUNCTION]_[TrainingSink_Status]   (TrainingSink_IsCancelled / _Set / _Load / _PublishComplete / _Finish / _FinishRun / _ForHorizon ride)
//   - [FUNCTION]_[TrainingWorkers_AllocZeroed]
//   - [FUNCTION]_[TrainingWorkers_ResolveStampSecret]
//   - [FUNCTION]_[TrainingWorkers_WallClockUs]
//   - [FUNCTION]_[TrainingWorkers_HorizonRequest]
//   - [FUNCTION]_[TrainingWorkers_WriteSummary]
//   - [FUNCTION]_[TrainingWorkers_RunHorizon]
//   - [STRUCT]_[TrainingPool]
//   - [FUNCTION]_[TrainingWorkers_RunPool]   (TrainingWorkers_PoolWorker rides)
//   - [FUNCTION]_[TrainingWorkers_RunMultiHorizon]   (TrainingWorkers_HorizonThread / _PoolJob / _Refuse ride)
//   - [FUNCTION]_[TrainingWorkers_FvRequestIdentity]
//   - [FUNCTION]_[TrainingWorkers_RunFullValidation]
//======================================================================================================
// Why this file exists (E.1.3 MP-1; sidecar CS-249, CS-217). The producer — the code that trains a
// model grid, validates every horizon, saves each model and writes its stamp, summary, data-files
// list and expected record — lived in the ImGui panel file and read and wrote TrainingPanelState
// directly. No test could reach the stamp seams, and every identity input crossed THREE hand-kept
// lists (the per-horizon job's 21 parameters, the parallel job struct's 21 fields, the serial path's
// positional call). Here the inputs are ONE request (each horizon's built in one place), the GUI is a
// SINK the adapter wires (display and cancel only — a headless caller supplies its own or none), and
// what the run produced comes back in a RESULT the caller owns, never read back out of the GUI. The
// Run Full Validation job (MP-1b) has the same shape: its request, its sink, its FullValidationResults.
//
// The move is behaviour-neutral for every artifact. These change, each on purpose (MP-1):
//   - the core works on a PRIVATE run-config copy. The serial path used to mutate the GUI-shared
//     run_control->run_config per horizon (a data race with the GUI thread), and BOTH modes copied the
//     pre-run config back over it at the end, erasing any edit the operator made during the run (F3);
//   - the core never writes the caller's dataset: the serial path used to leave the LAST horizon's
//     labels in the shared run_control->results (and fold its NaN counts into results->stats), so a
//     Run Full Validation click afterwards trained on those labels (CS-271). Serial now matches
//     parallel, which always worked on isolated copies. The dataset's by-value fields (config_used,
//     the sample count, the data-file record) are read once, at the start: the serial path re-read them
//     per horizon, so a backtest started mid-run re-stamped later horizons with ITS config;
//   - a horizon's completion is published LAST, with a release store, after every file it writes —
//     it used to go up before the summary, the data-files list and the expected record (F5, F6);
//   - the tally comes from each horizon's returned outcome: a horizon skipped for too few labels, or
//     one that never started (a failed allocation or thread start), no longer counts as trained (F8);
//   - the [WF marker] crash tag is set per horizon in both dispatch modes, not only in parallel (F9);
//   - the parallel no-batch fallback seeds each horizon's label buffer from the dataset — it was left
//     uninitialised, so a label walk that wrote nothing trained on garbage;
//   - a refused run clears the per-horizon display, so the panel never shows the previous run's rows
//     under the new click's horizons;
//   - the 64-aligned objects (the run-config copy, the jobs) are allocated at their alignment: the old
//     job came from malloc, which promises 16 bytes (MP-1a review F1);
//   - the parallel path runs at most max_threads horizons at once, on a bounded pool — it started one
//     thread per horizon whatever the cap (CS-275) — and a horizon a cancel reaches before it starts
//     says so on its row, in both modes.
// Plan: the E.1.3 plan's THE GATE AMENDMENT, MP-1; step 0 =
// plans/v5.15-live-readiness/plan_checks/2026-09-30-E.1.3-MP-1-producer-core-body-content-enumeration.csv.
//======================================================================================================

#include "BacktestEngine.hpp"                  // BacktestResults / BacktestRunConfig / FullValidationResults + the label and validation passes
#include "../ML_Headers/ModelPathSchema.hpp"   // D-431 nested layout — the path-grammar SSoT
#include "../MemHeaders/DirCreate.hpp"         // FoxDir_CreateParents (family + horizon chain)
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>

//======================================================================
// [STRUCT]_[TrainingHorizonSink]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one horizon's display + control channel (TrainingWorkers_RunHorizon's sink) — status text, validation progress, the completion flag (published LAST, release) and the cancel word (read); every pointer may be NULL]
//======================================================================
// [CODE]
//======================================================================
struct TrainingHorizonSink {
    char*         status;                // the horizon's status line (NULL = not displayed)
    size_t        status_cap;
    volatile int* progress;              // 0..100, written by the validation pass
    volatile int* complete;              // published (release) LAST, after every file the horizon writes
    volatile int* cancel;                // READ — under the orchestrator, the run's cancel
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[40B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingHorizonSink]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingHorizonDisplay]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one grid slot's display in the run sink — status text, validation progress and the completion flag; the orchestrator pairs it with the run's cancel to make that horizon's TrainingHorizonSink (no per-horizon cancel to be silently ignored)]
//======================================================================
// [CODE]
//======================================================================
struct TrainingHorizonDisplay {
    char*         status;
    size_t        status_cap;
    volatile int* progress;
    volatile int* complete;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[32B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingHorizonDisplay]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingRunSink]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the run's display + control channel — run status, cancel (read), horizon counters, the completion + running flags (published LAST) and one TrainingHorizonDisplay per grid slot; every pointer may be NULL]
//======================================================================
// [CODE]
//======================================================================
struct TrainingRunSink {
    char*               status;          // the run's status line
    size_t              status_cap;
    volatile int*       cancel;          // READ — the operator's cancel
    volatile int*       total;           // N
    volatile int*       current;         // the horizon in progress (ticks)
    volatile int*       done;            // horizons finished
    volatile int*       complete;        // published (release) LAST
    volatile int*       running;         // cleared (release) LAST
    TrainingHorizonDisplay horizon[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[320B]
// [ALIGN]_[8]
// [CACHE_LINES]_[5]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingRunSink]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingRunRequest]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[every input of a multi-horizon producer run, explicit — value-initialise it, then set EVERY field; the dataset and the run config are read only (the core copies what it mutates)]
//======================================================================
// [CODE]
//======================================================================
struct TrainingRunRequest {
    const BacktestResults*   data;       // the collected samples — read for the whole run, never written (CS-271); keep them unchanged until it returns
    const BacktestRunConfig* run_cfg;    // copied ONCE at the start into the core's private copy (F3); the GUI passes its click-time snapshot
    char  run_name[64];                  // the family name
    char  models_root[256];              // the artifact root — the "models" the paths were built from, made explicit
    int   primary_label_type;            // the run's kind: decides the class tree once per family (D-431)
    int   training_side;                 // 0 entry · 1 exit (flips the role file)
    int   horizon_count;                 // N, 1..HORIZON_LIST_MAX
    int   horizon_ticks[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float tp_pct[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    float sl_pct[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    int   label_type[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];   // per-horizon kind
    tt::XGBHyperparams hp;               // ONE snapshot for the booster, the validation trainers and the stamp
    int   wf_n_splits;
    int   wf_buffer_ticks;
    int   wf_min_train;
    float gap_threshold;
    float held_out_fraction;
    char  stamp_secret[sizeof(FullValidationResults::auto_stamp_secret)];   // the destination's own size (CS-274)
    uint64_t now_us;                     // CS-273 — the run's clock (μs since the Unix epoch): every horizon's stamp records it; the panel takes it at the click, a fixed one makes a run byte-reproducible
    int   max_threads;                   // 1 = serial · >= 2 = parallel · <= 0 = one per horizon (F16)
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[688B]
// [ALIGN]_[8]
// [CACHE_LINES]_[11]
// [STRADDLE]_[run_name@16 · tp_pct@380 · label_type@444 · hp@476]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingRunRequest]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingHorizonRequest]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one horizon's inputs — built ONLY by TrainingWorkers_HorizonRequest, so the identity list exists once (it existed three times)]
//======================================================================
// [CODE]
//======================================================================
struct TrainingHorizonRequest {
    int   h;                             // grid member index 0..horizon_count-1
    int   horizon_count;                 // N — the grid size the stamp records (PARITY-021)
    int   horizon_ticks;
    float tp_pct;
    float sl_pct;
    int   label_type;                    // this horizon's kind
    int   primary_label_type;            // the run's kind (the class tree)
    int   training_side;
    char  run_name[64];
    char  models_root[256];
    tt::XGBHyperparams hp;
    int   wf_n_splits;
    int   wf_buffer_ticks;
    int   wf_min_train;
    float gap_threshold;
    float held_out_fraction;
    char  stamp_secret[sizeof(FullValidationResults::auto_stamp_secret)];
    uint64_t now_us;                     // the run's clock (CS-273)
    int   labels_precomputed;            // 1 = the orchestrator's batch pass already filled the view's labels
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[560B]
// [ALIGN]_[8]
// [CACHE_LINES]_[9]
// [STRADDLE]_[run_name@32 · hp@352]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingHorizonRequest]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingHorizonOutcome]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[what one horizon did — returned by TrainingWorkers_RunHorizon; the run's tally sums these (F8), never the display]
//======================================================================
// [CODE]
//======================================================================
struct TrainingHorizonOutcome {
    int trained;                         // passed the label floor and went to training (a skipped horizon did not)
    int validated;                       // the held-out pass completed
    int stamped;                         // its stamp was written
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[12B]
// [ALIGN]_[4]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingHorizonOutcome]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingRunResult]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[what a run produced — each horizon's validation result and outcome, and the run's tally (summed from the outcomes); the caller owns it, the core zeroes it at the start]
//======================================================================
// [CODE]
//======================================================================
struct TrainingRunResult {
    FullValidationResults  horizon[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];   // FV request + result in one struct until MP-3 splits them
    TrainingHorizonOutcome outcome[ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX];
    int                    trained;
    int                    validated;
    int                    stamped;
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[105904B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1655]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingRunResult]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingHorizonJob]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one parallel horizon's job — its request, its dataset view (own labels), its private run-config copy, its sink, its result slot and its outcome; heap-allocated zeroed, owned and freed by the orchestrator]
//======================================================================
// [CODE]
//======================================================================
struct TrainingHorizonJob {
    TrainingHorizonRequest req;
    BacktestResults        view;         // a shallow copy of the dataset whose label buffer is this horizon's own
    BacktestRunConfig      run_cfg;      // this horizon's private copy (~565 KB — why jobs live on the heap)
    TrainingHorizonSink    sink;         // this horizon's channel, the run's cancel folded in
    FullValidationResults* result;       // this horizon's slot in the run's result (distinct per job)
    volatile int*          current;      // the run's current-horizon channel
    volatile int*          done;         // the run's horizons-finished channel
    TrainingHorizonOutcome outcome;      // written by the worker, read by the orchestrator AFTER the join
};
// BacktestResults + BacktestRunConfig make the job 64-aligned (ControllerConfig's alignas(64) PerNodeCfg),
// so it comes from TrainingWorkers_AllocZeroed, never malloc / calloc. At namespace scope on purpose: it
// also puts the job's layout in every TU's record dump, so the cache-layout gate polices it in a TU built
// without XGBoost too (the only code that allocates a job is XGBoost-only).
static_assert(alignof(TrainingHorizonJob) == 64,
              "TrainingHorizonJob's alignment changed: re-check how it is allocated");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[632512B]
// [ALIGN]_[64]
// [CACHE_LINES]_[9883]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingHorizonJob]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingFvRequest]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[every input of a Run Full Validation job — the model to validate and re-stamp, the role it records, the labels' identity, the WF + gate params, the hyperparameters and the secret; value-initialise it, then set EVERY field]
//======================================================================
// [CODE]
//======================================================================
struct TrainingFvRequest {
    const BacktestResults* data;         // the collected samples — read only; config_used.auto_stamp_on_held_out decides whether a stamp is requested (R-31 retires that knob at MP-7)
    // v5.10.0E — snapshot operator-editable fields at click time, not in
    // the worker. The ImGui input fields write into state->model_path /
    // state->fv_auto_stamp_secret CONCURRENTLY with worker execution; if
    // operator clicks Run Full Validation with empty model_path then
    // types it AFTER, the worker reads empty (skip copy → auto_stamp_path
    // empty), then status build reads the post-typed value, producing
    // "model_path='X' did not propagate (worker race)" diagnostic.
    // Capture-at-click eliminates the race.
    char     model_path[256];            // the model to validate — the stamp target
    char     role[sizeof(FullValidationResults::req_role)];   // F10 — the role the stamp records ("" = none), explicit; Training_RoleFromModelFile derives it from the file name (Class 59) until MP-7a retires the FV stamp
    // v5.11.41 — capture label params from run_control->run_config at click
    // time so RFV can stamp them into the body. Live in BacktestRunConfig,
    // not in ControllerConfig (= data->config_used) so RFV can't reach
    // them otherwise. Closes /parity-check 2026-05-07-stamp CRITICAL-1.
    //
    // E.1.2.C — `label_type` is deliberately sourced from
    // run_control->run_config (the field that actually produced results->labels[]),
    // NOT from state->label_type: the combo can be changed BETWEEN the Collect
    // click and the Run-Full-Validation click, with no race required, and the
    // worker then trained WF/held-out on one objective while stamping another.
    // When the class counts differ the engine REFUSES at load
    // (NodeModelZoo.hpp: "stamp claims model_num_outputs=N but handle=M"); when
    // they match — WIN_LOSS / BARRIER / VOL_BARRIER / WILL_PEAK are all binary —
    // nothing catches it and the stamp simply records a label the model never
    // trained on. Sourcing from run_config makes the stamp describe the labels.
    int      label_type;
    int      label_forward_ticks;
    double   label_tp_pct;
    double   label_sl_pct;
    // D-476 (2026-09-02) — the round-trip fee, so the single-horizon stamp records the
    // SAME effective TP the label walk used (tp+fee for a percent barrier). Without it
    // this seam stamped the fee-blind tp while the multi-horizon seam stamped tp+fee —
    // a served bracket narrower than the trained barrier (PARITY-065 residual (3)).
    double   label_roundtrip_fee_pct;
    uint64_t feature_mask;               // D-477 — the collect-time mask (0 = all-on)
    int      wf_n_splits;
    int      wf_horizon_ticks;           // the purge horizon (s5 leaf-16)
    int      wf_buffer_ticks;
    int      wf_min_train;
    float    gap_threshold;
    float    held_out_fraction;
    // E.1.2.C follow-up (2026-08-22) — the SECOND entry point into
    // Backtest_RunFullValidation. The multi-horizon Train path was threaded with
    // the click-time hyperparameters at f99e102; THIS one was missed, so the
    // standalone "Run Full Validation" button still trained WF folds + held-out at
    // XGBHyperparams_Defaults() AND overwrote the target model's signed stamp with
    // 6/0.1/200 — silently undoing, on disk, the thing f99e102 fixed. The
    // `= nullptr` default that makes hp_override safe for un-updated callers is
    // exactly what made the omission invisible: no compile error, no warning.
    // Caught by the Stage-6.5.4 adversarial handoff review, not by the author.
    // (MP-1b: a request value-initialised and never given `hp` carries
    // XGBHyperparams' default member values — the same silent 6/0.1/200. Set it.)
    tt::XGBHyperparams hp;
    char     stamp_secret[sizeof(FullValidationResults::auto_stamp_secret)];   // CS-274 — the destination's own size, so nothing truncates it (the old snapshot was 64 B)
    uint64_t now_us;                     // CS-273 — the run's clock (μs since the Unix epoch) for the stamp; the panel takes it at the click
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[528B]
// [ALIGN]_[8]
// [CACHE_LINES]_[9]
// [STRADDLE]_[hp@344]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingFvRequest]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingFvSink]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a Run Full Validation job's display + control channel — status text, progress, cancel (read), and the completion + running flags published LAST; every pointer may be NULL]
//======================================================================
// [CODE]
//======================================================================
struct TrainingFvSink {
    char*         status;
    size_t        status_cap;
    volatile int* progress;              // 0..100, written by the validation pass
    volatile int* cancel;                // READ
    volatile int* complete;              // published (release) LAST — the panel shows the result once it reads 1 (acquire)
    volatile int* running;               // cleared (release) LAST
};
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[48B]
// [ALIGN]_[8]
// [CACHE_LINES]_[1]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingFvSink]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingSink_Status]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the sink's access helpers — every channel may be NULL (a headless caller wires only what it reads); completion is published with release, and the GUI reads it with TrainingSink_Load (acquire)]
//======================================================================
// [CODE]
//======================================================================
__attribute__((format(printf, 3, 4)))  // -Werror=format sees every call (cmake/FormatGuard.cmake)
inline void TrainingSink_Status(char* buf, size_t cap, const char* fmt, ...) {
    if (!buf || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}
inline int TrainingSink_IsCancelled(volatile int* cancel) {
    return cancel ? __atomic_load_n(cancel, __ATOMIC_RELAXED) : 0;
}
inline void TrainingSink_Set(volatile int* p, int v) {
    if (p) __atomic_store_n(p, v, __ATOMIC_RELAXED);
}
// A completion flag guards what was written before it (status text, the result, the files): release
// here, acquire in the reader, so seeing the flag means seeing those writes (F6).
inline void TrainingSink_PublishComplete(volatile int* flag) {
    if (flag) __atomic_store_n(flag, 1, __ATOMIC_RELEASE);
}
inline void TrainingSink_Finish(volatile int* complete, volatile int* running) {
    TrainingSink_PublishComplete(complete);
    if (running) __atomic_store_n(running, 0, __ATOMIC_RELEASE);
}
inline void TrainingSink_FinishRun(const TrainingRunSink& sink) {
    TrainingSink_Finish(sink.complete, sink.running);
}
inline int TrainingSink_Load(const volatile int* flag) {
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}
inline TrainingHorizonSink TrainingSink_ForHorizon(const TrainingHorizonDisplay& d, volatile int* cancel) {
    TrainingHorizonSink s{};
    s.status     = d.status;
    s.status_cap = d.status_cap;
    s.progress   = d.progress;
    s.complete   = d.complete;
    s.cancel     = cancel;
    return s;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingSink_Status]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_AllocZeroed]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[a zeroed heap object of T at T's own alignment — malloc / calloc promise 16 bytes, and the run-config copy, the jobs and the panel's worker args are 64-aligned; NULL on out-of-memory, released with free()]
//======================================================================
// MP-1a review F1: a member access through a pointer short of T's alignment is undefined behaviour,
// and the ubsan lane aborts on it the first time a test reaches the code. aligned_alloc wants a size
// that is a multiple of the alignment, so the size is rounded up (the tail past sizeof(T) is unused).
//======================================================================
// [CODE]
//======================================================================
template <typename T>
inline T* TrainingWorkers_AllocZeroed() {
    constexpr size_t a = alignof(T) > alignof(max_align_t) ? alignof(T) : alignof(max_align_t);
    constexpr size_t n = (sizeof(T) + a - 1) / a * a;
    void* p = aligned_alloc(a, n);
    if (p) memset(p, 0, n);
    return (T*)p;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_AllocZeroed]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_ResolveStampSecret]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE rule for the secret a training stamp is signed with — the panel's field, else the collected cfg's auto_stamp_secret, else empty (dev mode); copied whole, NUL-terminated]
//======================================================================
// CS-277 — Train Multi-Horizon fell back to the cfg secret when the panel field was empty (v5.11.47:
// set it once in cfg) and Run Full Validation did not, so with the secret set only in cfg the FV
// re-stamp overwrote a cfg-signed stamp with a dev-mode one. Both request builders call this now.
//======================================================================
// [CODE]
//======================================================================
inline void TrainingWorkers_ResolveStampSecret(const char* panel_secret, size_t panel_cap,
                                               const char* cfg_secret, size_t cfg_cap,
                                               char* out, size_t out_cap) {
    if (!out || out_cap == 0) return;
    const char* src = panel_secret;
    size_t      cap = panel_cap;
    if (!src || cap == 0 || src[0] == '\0') {
        src = cfg_secret;
        cap = cfg_cap;
    }
    size_t n = (src && cap) ? strnlen(src, cap) : 0;
    if (n >= out_cap) n = out_cap - 1;
    if (n) memcpy(out, src, n);
    out[n] = '\0';
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_ResolveStampSecret]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_WallClockUs]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the wall clock a run's request carries (CS-273) — CLOCK_REALTIME, μs since the Unix epoch, the stamp consumer's contract; the GUI adapters take it once, at the click]
//======================================================================
// [CODE]
//======================================================================
inline uint64_t TrainingWorkers_WallClockUs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_WallClockUs]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_HorizonRequest]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the ONE place a horizon's inputs are assigned — both dispatch modes call it; value-initialised, every field set from the run request]
//======================================================================
// [CODE]
//======================================================================
inline TrainingHorizonRequest TrainingWorkers_HorizonRequest(const TrainingRunRequest& run, int h,
                                                             int labels_precomputed) {
    TrainingHorizonRequest r{};
    r.h                  = h;
    r.horizon_count      = run.horizon_count;
    r.horizon_ticks      = run.horizon_ticks[h];
    r.tp_pct             = run.tp_pct[h];
    r.sl_pct             = run.sl_pct[h];
    r.label_type         = run.label_type[h];
    r.primary_label_type = run.primary_label_type;
    r.training_side      = run.training_side;
    memcpy(r.run_name, run.run_name, sizeof(r.run_name));
    memcpy(r.models_root, run.models_root, sizeof(r.models_root));
    r.hp                 = run.hp;
    r.wf_n_splits        = run.wf_n_splits;
    r.wf_buffer_ticks    = run.wf_buffer_ticks;
    r.wf_min_train       = run.wf_min_train;
    r.gap_threshold      = run.gap_threshold;
    r.held_out_fraction  = run.held_out_fraction;
    memcpy(r.stamp_secret, run.stamp_secret, sizeof(r.stamp_secret));
    r.now_us             = run.now_us;
    r.labels_precomputed = labels_precomputed;
    return r;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_HorizonRequest]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_WriteSummary]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one horizon's summary_{entry,exit}.txt body — the identity, the hyperparameters that trained, the label config, the WF + held-out metrics by kind, the stamp outcome and the corpus record; Past Runs reads it]
//======================================================================
// Lifted out of TrainingWorkers_RunHorizon (CS-272) so a cell can check what a summary says for each
// label kind without training a model. The body is the moved block; the request is bound to its names.
//======================================================================
// [CODE]
//======================================================================
inline void TrainingWorkers_WriteSummary(FILE* sf, const TrainingHorizonRequest& req, const char* role,
                                         const char* horizon_dir, int num_classes,
                                         const BacktestRunConfig* local_run_cfg,
                                         const BacktestResults* results,
                                         const FullValidationResults* fv) {
    const char*               run_name      = req.run_name;
    const int                 horizon_ticks = req.horizon_ticks;
    const int                 label_type    = req.label_type;
    const float               tp_pct        = req.tp_pct;
    const float               sl_pct        = req.sl_pct;
    const tt::XGBHyperparams& snap_hp       = req.hp;
    // v5.11.50 — use CANONICAL summary.txt field names that past_runs
    // reads (in PastRuns_LoadOne). Pre-fix v5.11.41.A used made-up wf_*
    // names; past_runs ignored them, leaving Train Acc/Val Acc/Gap
    // columns blank. NOW uses the same names Save Run uses so
    // multi-horizon runs render the same as single-horizon Save Run
    // bundles. Plus expected_num_classes (v5.11.49) for Classes col.
    fprintf(sf, "run: %s/horizon_%d\n", run_name, horizon_ticks);  // D-431 nested (display-only; verified unparsed)
    fprintf(sf, "role: %s\n", role);
    fprintf(sf, "model: %s/%s.json\n", horizon_dir, role);
    fprintf(sf, "label_type: %d\n", label_type);
    fprintf(sf, "expected_num_classes: %d\n", num_classes);
    // E.1.2.C — report what actually TRAINED. These were a SECOND live read of
    // the same widgets, minutes after the first, so an edit in between made
    // summary.txt disagree with the model sitting beside it — and PastRuns
    // displays the summary value as truth.
    fprintf(sf, "max_depth: %d\n", snap_hp.max_depth);
    fprintf(sf, "learning_rate: %.3f\n", (double)snap_hp.learning_rate);
    fprintf(sf, "n_estimators: %d\n", snap_hp.n_estimators);
    fprintf(sf, "label_tp_pct: %.4f\n", (double)tp_pct);
    fprintf(sf, "label_sl_pct: %.4f\n", (double)sl_pct);
    // s5 leaf-15 — lineage: which round trip this run's win threshold cleared.
    // Sourced from local_run_cfg (the config that produced these labels), the
    // same click-time-snapshot discipline as the hyperparameters above.
    fprintf(sf, "label_roundtrip_fee_pct: %.4f\n",
            local_run_cfg ? local_run_cfg->label_roundtrip_fee_pct : 0.0);
    fprintf(sf, "label_lookahead_ticks: %d\n", horizon_ticks);
    fprintf(sf, "n_train_samples: %d\n", results->sample_count);
    fprintf(sf, "label_kind: %d\n", fv->label_kind);
    fprintf(sf, "valid_folds: %d\n", fv->walkforward.valid_folds);
    // val_accuracy / val_correlation: pick whichever fits the kind.
    // past_runs reader sets has_wf_results=1 when EITHER is read.
    if (LabelType_IsRegression(label_type)) {  // regression (E.1.2.D NEW-6 — was the unreachable == 2)
        fprintf(sf, "val_correlation: %.4f\n",
                (double)fv->walkforward.mean_val_correlation);
        fprintf(sf, "val_mse: %.6f\n",
                (double)fv->walkforward.mean_val_mse);
        // CS-272 — the in-sample r, for Past Runs' Train r column (it showed the train "accuracy"
        // line as a proxy, and that line was always 0.00 for regression).
        fprintf(sf, "train_correlation: %.4f\n",
                (double)fv->walkforward.mean_train_correlation);
    } else {  // binary or multiclass
        fprintf(sf, "val_accuracy: %.2f\n",
                100.0 * (double)fv->walkforward.mean_val_accuracy);
        fprintf(sf, "val_stddev: %.2f\n",
                100.0 * (double)fv->walkforward.std_val_accuracy);
        // 2026-09-03 — the gate metric + per-class recall (E.2/E.3). Percent
        // like val_accuracy so Past Runs reads them with the same rule.
        fprintf(sf, "val_balanced_accuracy: %.2f\n",
                100.0 * (double)fv->walkforward.mean_val_balanced_accuracy);
        {
            const int Kp = (fv->walkforward.num_classes >= 2)
                         ? (fv->walkforward.num_classes > 16 ? 16 : fv->walkforward.num_classes) : 2;
            for (int k = 0; k < Kp; ++k)
                fprintf(sf, "val_recall_c%d: %.2f\n", k,
                        100.0 * (double)fv->walkforward.mean_val_class_recall[k]);
            fprintf(sf, "held_out_balanced_accuracy: %.4f\n",
                    (double)fv->held_out_balanced_accuracy);
            for (int k = 0; k < Kp; ++k)
                fprintf(sf, "held_out_recall_c%d: %.4f\n", k,
                        (double)fv->held_out_class_recall[k]);
        }
    }
    fprintf(sf, "train_val_gap: %.4f\n",
            (double)fv->wf_to_held_out_gap);
    fprintf(sf, "overfit_folds: %d\n", fv->walkforward.overfit_count);
    fprintf(sf, "held_out_metric: %.4f\n", (double)fv->held_out_metric);
    fprintf(sf, "held_out_count: %d\n", fv->held_out_count);
    // accuracy = train accuracy (classification); past_runs reads it as `r->train_accuracy`.
    // CS-272 — gated on the kind SSoT: `label_kind != 2` was the unreachable discriminant NEW-6
    // fixed in TrainingWorkers_RunHorizon's metric lines (label_kind IS num_classes and no target
    // has 2), so every regression horizon wrote "accuracy: 0.00" — mean_train_accuracy is never
    // set for regression.
    if (!LabelType_IsRegression(label_type)) {
        fprintf(sf, "accuracy: %.2f\n",
                100.0 * (double)fv->walkforward.mean_train_accuracy);
    }
    // Bookkeeping (operator may grep for these even though past_runs
    // doesn't use them):
    fprintf(sf, "ran_held_out: %d\n", fv->ran_held_out);
    fprintf(sf, "auto_stamp_attempted: %d\n", fv->auto_stamp_attempted);
    fprintf(sf, "auto_stamp_ok: %d\n", fv->auto_stamp_ok);
    if (fv->auto_stamp_ok) {
        fprintf(sf, "auto_stamp_path_written: %s\n",
                fv->auto_stamp_path_written);
    } else if (fv->auto_stamp_attempted) {
        fprintf(sf, "auto_stamp_error: %s\n", fv->auto_stamp_error);
    }
    // 2026-09-03 — the CORPUS SELECTION record (what-to-do-next.md: "today the
    // selection is not recorded anywhere"). Sourced from `results` — the record
    // the corpus walk wrote at collect time (BacktestResults_RecordCorpus), NOT
    // the Data panel's live selection: the operator can reselect between
    // Collect and Train, and the samples this model trained on are the
    // collect-time set. The tick-time span is the DATA's own timestamps (UTC),
    // the same two scalars the purge gap's time reach reads (D-474).
    {
        fprintf(sf, "data_files: %d\n", results->data_file_count);
        if (results->data_file_count > 0) {
            fprintf(sf, "data_first_file: %s\n", results->data_first_file);
            fprintf(sf, "data_last_file: %s\n",  results->data_last_file);
        }
        if (results->data_list_sha256[0])
            fprintf(sf, "data_list_sha256: %s\n", results->data_list_sha256);
        if (results->first_tick_us > 0 && results->last_tick_us >= results->first_tick_us) {
            char d0[16] = "", d1[16] = "";
            struct tm tm0, tm1;
            time_t t0 = (time_t)(results->first_tick_us / 1000000ULL);
            time_t t1 = (time_t)(results->last_tick_us  / 1000000ULL);
            if (gmtime_r(&t0, &tm0)) strftime(d0, sizeof(d0), "%Y-%m-%d", &tm0);
            if (gmtime_r(&t1, &tm1)) strftime(d1, sizeof(d1), "%Y-%m-%d", &tm1);
            fprintf(sf, "data_first_tick_utc: %s\n", d0);
            fprintf(sf, "data_last_tick_utc: %s\n",  d1);
            fprintf(sf, "data_first_tick_us: %llu\n", (unsigned long long)results->first_tick_us);
            fprintf(sf, "data_last_tick_us: %llu\n",  (unsigned long long)results->last_tick_us);
        }
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_WriteSummary]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_RunHorizon]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[one horizon of a multi-horizon grid — labels, the final model trained + saved, WF + held-out validation (which emits its stamp), then the summary, the data-files list and the expected record; completion published LAST]
// [REFERENCE]_[PARITY]_[PARITY-21]
//======================================================================
// v5.11.41 — the per-horizon FV helper, moved here by MP-1 (it was mh_run_one_horizon_fv in the
// panel file). `view` is the dataset with THIS horizon's own label buffer; `run_cfg` is a copy the
// core owns, mutated per horizon to drive the label pass — never the GUI's; `result` is written on
// every path (zeroed first, so a skipped horizon's slot is defined). The body moved verbatim except the
// substitutions in plans/v5.15-live-readiness/plan_checks/2026-09-30-E.1.3-MP-1a-move-review.diff: the
// sink routing, models_root, the outcome marks (F8), the completion moving to the end (F5), the dead
// auto-stamp flag (F2) and two dead locals, and the comments that went stale with the move (F14).
//======================================================================
// [CODE]
//======================================================================
inline TrainingHorizonOutcome TrainingWorkers_RunHorizon(const TrainingHorizonRequest& req,
                                                         BacktestResults* view,
                                                         BacktestRunConfig* run_cfg,
                                                         const TrainingHorizonSink& sink,
                                                         FullValidationResults* result) {
    TrainingHorizonOutcome outcome{};
    if (!view || !run_cfg || !result) {
        fprintf(stderr, "[mh-train] horizon %d: missing dataset view, run config or result slot — refused\n",
                req.horizon_ticks);
        TrainingSink_Status(sink.status, sink.status_cap,
                            "h=%d FAILED: missing dataset view, run config or result slot", req.horizon_ticks);
        TrainingSink_PublishComplete(sink.complete);
        return outcome;
    }
    memset((void *)result, 0, sizeof(*result));   // zero BYTES, as the moved body's own clear does
    // The request, bound once under the names the moved body uses.
    const int                 h                      = req.h;
    const int                 horizon_ticks          = req.horizon_ticks;
    const float               tp_pct                 = req.tp_pct;
    const float               sl_pct                 = req.sl_pct;
    const int                 label_type             = req.label_type;
    const char*               run_name               = req.run_name;
    const int                 snap_n_splits          = req.wf_n_splits;
    const int                 snap_buffer_ticks      = req.wf_buffer_ticks;
    const int                 snap_min_train         = req.wf_min_train;
    const float               snap_gap_threshold     = req.gap_threshold;
    const float               snap_held_out_fraction = req.held_out_fraction;
    const char*               snap_auto_stamp_secret = req.stamp_secret;
    const int                 primary_label_type     = req.primary_label_type;
    const int                 labels_precomputed     = req.labels_precomputed;
    const int                 training_side          = req.training_side;
    const int                 horizon_count          = req.horizon_count;
    const tt::XGBHyperparams& snap_hp                = req.hp;
    BacktestResults*          results                = view;
    BacktestRunConfig*        local_run_cfg          = run_cfg;
    // Backtest_RunFullValidation writes progress and reads cancel through non-null pointers.
    volatile int  progress_unwired = 0;
    volatile int  cancel_never     = 0;
    volatile int* progress = sink.progress ? sink.progress : &progress_unwired;
    volatile int* cancel   = sink.cancel   ? sink.cancel   : &cancel_never;
    // TECH_DEBT-302c — tag this thread's [WF marker] crash-bisection lines with its horizon, in BOTH
    // dispatch modes (only the parallel worker used to set it, so serial markers were untagged — F9).
    const int prev_marker = g_wf_marker_horizon;
    g_wf_marker_horizon = horizon_ticks;

    TrainingSink_Status(sink.status, sink.status_cap,
             "h=%d: computing labels...", horizon_ticks);

    local_run_cfg->label_forward_ticks = horizon_ticks;
    local_run_cfg->label_tp_pct        = (double)tp_pct;
    local_run_cfg->label_sl_pct        = (double)sl_pct;
    // E.1.2.C — the label KIND belongs in this per-horizon mutation set too,
    // and its absence was not cosmetic. Backtest_ComputeLabelsFromSamples
    // picks the label leaf from local_run_cfg->label_type
    // (the label pass reads local_run_cfg->label_type), which nothing here
    // wrote — so every horizon
    // recomputed labels from whatever the last COLLECT click left behind,
    // while `label_type` (the parameter) drove num_classes, the XGB
    // objective, the role file, the run_subdir and the stamp. Same run,
    // two different labels.
    //
    // That silently voided the "Label Kind CSV" feature outright: the orchestrator
    // writes the per-horizon kind into the job's label_type, so it reached the
    // objective and the stamp but NEVER the labels the model actually
    // trained on. Every horizon trained on the collect-time label and was
    // then stamped as something else.
    //
    // Safe to mutate: local_run_cfg is ALWAYS a copy the core owns — serial: the run's private copy, its
    // horizons in turn; parallel: one per horizon — never the GUI-shared run config (MP-1, F3).
    local_run_cfg->label_type          = label_type;
    // The mutation set above stays UNCONDITIONAL even when labels arrive
    // precomputed: Backtest_RunFullValidation + the stamp read label_type /
    // fwd / tp / sl off local_run_cfg downstream. Only the corpus walk is
    // skipped — the batch already produced these exact bytes (memcmp oracle).
    if (!labels_precomputed) {
        Backtest_ComputeLabelsFromSamples(results, local_run_cfg);
    }

    int n_valid = 0;
    for (int s = 0; s < results->sample_count; ++s) {
        if (!isnan(results->labels[s]) && !isinf(results->labels[s]))
            n_valid++;
    }
    if (n_valid < 50) {
        fprintf(stderr, "[mh-train] horizon %d: only %d valid labels; skip\n",
                horizon_ticks, n_valid);
        TrainingSink_Status(sink.status, sink.status_cap,
                 "h=%d FAILED: only %d valid labels (need >= 50)",
                 horizon_ticks, n_valid);
        g_wf_marker_horizon = prev_marker;
        TrainingSink_PublishComplete(sink.complete);   // nothing written on this path
        return outcome;
    }
    outcome.trained = 1;   // past the label floor (F8: a skipped horizon is not counted as trained)

    int num_classes = (label_type >= 0 && label_type < LABEL_COUNT)
                      ? label_table[label_type].num_classes : 0;
    // E.1.2.C 3-role — side selects the ROLE FILE via the extracted helper
    // (side=1 => "exit", saved CO-LOCATED; label kind stays free per (b)).
    const char* role = Training_ResolveRole(label_type, training_side);
    // D-431 nested layout — run_subdir derives from the RUN's PRIMARY kind
    // (S2-F4 close: one family = one class tree; the old per-horizon
    // derivation fragmented a mixed-CSV family across two trees).
    int primary_nc = (primary_label_type >= 0 && primary_label_type < LABEL_COUNT)
                     ? label_table[primary_label_type].num_classes : 0;
    const char* run_subdir = (primary_label_type == LABEL_PEAK_VALLEY_STABLE
                              || primary_label_type == LABEL_REGIME
                              || primary_nc >= 2)
        ? "classification" : (primary_nc == 1 ? "regression" : "classification");
    // E.1.2.C 3-retire (2026-08-20) — the models/exit/ SIDE TREE is RETIRED:
    // no loader ever walked it (PARITY-044); exit models land CO-LOCATED in
    // the same per-horizon dirs (side flips the ROLE FILE, next commit).
    //
    // D-431 — the FAMILY node is the unit: models/<class>/<family>/ holds
    // horizon_<N> children + the bundle-scoped state files. Every future
    // family is born with its bundle node (the treadmill's structural end);
    // FoxDir_CreateParents builds the whole chain.
    char family_dir[340];
    snprintf(family_dir, sizeof(family_dir), "%s/%s/%s",
             req.models_root, run_subdir, run_name);
    char horizon_dir[360];
    ModelPath_HorizonDir(horizon_dir, sizeof(horizon_dir),
                         family_dir, (long)horizon_ticks);
    FoxDir_CreateParents(horizon_dir);

    HeldOutSplit split = HeldOutSplit_Make(results->sample_count,
                                            (double)snap_held_out_fraction);
    char unlock_token[33];
    memcpy(unlock_token, split.lock_token, sizeof(unlock_token));
    HeldOutSplit_Unlock(&split, unlock_token);

    FullValidationResults *fv = result;
    memset((void *)fv, 0, sizeof(*fv));   // zero BYTES (-Wclass-memaccess: deliberate)
    // v5.11.47 — ALWAYS stamp. Was gated on snap_auto_stamp_enabled
    // (= cfg.auto_stamp_on_held_out which defaults to 1 but could be
    // 0 OR uninitialized if Run Control never loaded a cfg). Operator
    // wants stamps unconditionally — they're cheap, and unstamped
    // models lose load-time safety checks (label_registry_hash,
    // feature_registry_hash, model_num_outputs, etc.). Removed the
    // conditional; auto_stamp_path is always set.
    snprintf(fv->auto_stamp_path, sizeof(fv->auto_stamp_path),
             "%s/%s.json", horizon_dir, role);
    size_t n = strnlen(snap_auto_stamp_secret, sizeof(req.stamp_secret));
    if (n >= sizeof(fv->auto_stamp_secret))
        n = sizeof(fv->auto_stamp_secret) - 1;
    memcpy(fv->auto_stamp_secret, snap_auto_stamp_secret, n);
    fv->auto_stamp_secret[n] = '\0';
    fv->auto_stamp_format_version = 0;
    fv->req_label_lookahead_ticks = horizon_ticks;
    // s5-F12 — record the EFFECTIVE barrier, i.e. the one the labels were
    // actually built against, not the raw operator input. Same resolver the
    // label walk uses (Label_ResolveEffectiveTp), so train-time and stamp-time
    // cannot disagree. Before this the stamp carried the raw tp while the
    // labels carried tp+fee, and the served bracket inherited the stamp's
    // value — an M5 train-serve parity break with no observable symptom
    // (the fee is not in the stamp body either, so nothing could catch it).
    // D-476 (2026-09-02) — and record it BY KIND. The sl line here used to copy the
    // raw slot (the s5-F12 fix landed on tp and not its sibling), so a sigma-window
    // row stamped its TICK COUNT into a field the serve side reads as a price
    // bracket and tt::barrier_is_corrupt refused as >100%. Label_Stamp*Pct is the
    // ONE rule for both stamp seams (this one + the single-horizon twin): price-
    // percent kinds → the effective resolver; sigma-quantities (TP_SIGMA_K,
    // SL_VOL_WINDOW_TICKS) → 0, which the serve side defines as "use the cfg
    // bracket" (GateParameters.hpp tp_pct / sl_pct). PARITY-065 carries the future
    // tier, where the kind itself travels in the stamp body.
    fv->req_label_tp_pct          = Label_StampTpPct(
                                        label_type, (double)tp_pct,
                                        local_run_cfg ? local_run_cfg->label_roundtrip_fee_pct : 0.0);
    fv->req_label_sl_pct          = Label_StampSlPct(label_type, (double)sl_pct);
    fv->req_feature_mask          = local_run_cfg ? local_run_cfg->feature_mask : 0;   // D-477 — the mask the rows were collected under
    // v5.15.3.B.2 — PARITY-021 close. Grid identification plumbed from
    // multi-horizon worker through FullValidationResults → StampArgs.
    // grid_member_count = horizon_count (total horizons), member_idx = h
    // (this horizon's index 0..N-1). Single-horizon callers (Train Model
    // button) leave defaults at 1/0/1 via the function-arg default.
    fv->req_grid_member_count = horizon_count;
    fv->req_grid_member_idx   = h;
    fv->req_horizon_count     = horizon_count;
    // v5.15.3.B.2 — also plumb expected_role from label_type so Stamp_
    // AssembleAndEmit emits args.req_role correctly. Pre-v5.15.3 this
    // came via inf.expected_role manual setter at RFV; helper expects
    // it via out->req_role.
    snprintf(fv->req_role, sizeof(fv->req_role), "%s", role);

    // v5.11.52 — train + save the FINAL deployable model BEFORE calling
    // RFV. RFV computes WF + held-out metrics + auto-stamps the file at
    // fv->auto_stamp_path, but it doesn't save a model itself (it trains
    // boosters internally for WF folds + held-out then discards). The
    // pre-v5.11.41.A worker had this train+save inline; my v5.11.41.A
    // refactor dropped it when replacing inline XGB with RFV. Without
    // a saved model file, stamp_write_for_model failed at SHA256 of
    // model_path → "could not sha256 ..." status. Restoring train+save
    // here closes that bug.
    //
    // Model trained on full feature_matrix (NaN-filtered labels). This
    // matches the held-out training's training portion ([0, trainval_end))
    // closely enough for deployment purposes — operator gets a model
    // that learned from the most data possible.
#ifdef USE_XGBOOST
    {
        TrainingSink_Status(sink.status, sink.status_cap,
                 "h=%d: training final model for save+stamp...", horizon_ticks);

        // count valid (non-NaN, non-Inf) labels
        int n_valid = 0;
        for (int s = 0; s < results->sample_count; ++s) {
            if (!isnan(results->labels[s]) && !isinf(results->labels[s]))
                n_valid++;
        }
        if (n_valid >= 50) {
            float *train_features = (float*)malloc((size_t)n_valid * MODEL_NUM_FEATURES * sizeof(float));
            float *train_labels   = (float*)malloc((size_t)n_valid * sizeof(float));
            if (train_features && train_labels) {
                int j = 0;
                for (int s = 0; s < results->sample_count; ++s) {
                    if (isnan(results->labels[s]) || isinf(results->labels[s])) continue;
                    memcpy(&train_features[(size_t)j * MODEL_NUM_FEATURES],
                           &results->feature_matrix[(size_t)s * MODEL_NUM_FEATURES],
                           MODEL_NUM_FEATURES * sizeof(float));
                    train_labels[j] = results->labels[s];
                    j++;
                }
                DMatrixHandle dtrain = nullptr;
                // PARITY-049 — was a bare quiet_NaN() literal while nine other sites passed
                // -1.0f. The VALUE was right (and is why no retrain is owed); the divergence was.
                XGDMatrixCreateFromMat(train_features, n_valid, MODEL_NUM_FEATURES,
                                        XGB_MISSING_VALUE, &dtrain);
                XGDMatrixSetFloatInfo(dtrain, "label", train_labels, n_valid);
                BoosterHandle booster = nullptr;
                XGBoosterCreate(&dtrain, 1, &booster);
                // E.1.2.C — the click-time snapshot, not eight live widget reads.
                tt::XGBHyperparams hp = snap_hp;
                tt::XGBHyperparams_Apply(booster, hp);
                int K_classes = (label_type >= 0 && label_type < LABEL_COUNT)
                              ? label_table[label_type].num_classes : 0;
                int is_multi  = (K_classes >= 2);
                int is_regr   = (K_classes == 1);
                if (is_multi) {
                    XGBoosterSetParam(booster, "objective", "multi:softprob");
                    char nc_s[8]; snprintf(nc_s, 8, "%d", K_classes);
                    XGBoosterSetParam(booster, "num_class", nc_s);
                } else if (is_regr) {
                    XGBoosterSetParam(booster, "objective", "reg:squarederror");
                } else {
                    XGBoosterSetParam(booster, "objective", "binary:logistic");
                }
                // TECH_DEBT-301a (2026-08-25) — THE SHIPPED MODEL WAS TRAINED UNWEIGHTED.
                // The WF folds and the held-out eval both applied class balance; this site — the
                // one whose artifact is actually SAVED and STAMPED — applied none. So the two
                // numbers the stamp certifies described boosters this model is not, and on a
                // 61.5/34.4/4.1 split an unweighted booster leans to the majority and rarely
                // emits the rare class, which is the buy signal. Routed through the SAME producer
                // as the other two so the three cannot drift apart again.
                float *ship_w = XGBoost_ApplyClassBalance(booster, dtrain, train_labels, n_valid,
                                                           K_classes, is_regr, is_multi,
                                                           "mh-train shipped");
                int it_completed = 0;   // E.1.2.D (scan-2 NEW-1) — real rounds only
                for (int it = 0; it < hp.n_estimators; ++it) {
                    if (TrainingSink_IsCancelled(cancel)) break;
                    if (XGBoosterUpdateOneIter(booster, it, dtrain) != 0) break;
                    it_completed++;
                }
                // E.1.2.D (scan-2 NEW-1) — NEVER save a zero-tree husk over a
                // real artifact. A cancel at round 0 / a first-round failure /
                // n_estimators==0 fell through to an unconditional save,
                // writing a valid-but-empty XGBoost JSON ("num_trees":"0")
                // that silently REPLACED the previous model at this path and
                // predicted base_score forever — unstamped, so every load-time
                // check was vacuous on it (S2-F6). The 516-byte
                // twins_horizon_7500/exit.json husk is the live instance.
                if (ship_w) { free(ship_w); ship_w = nullptr; }   // TECH_DEBT-301a — DMatrix copied it
                if (it_completed > 0) {
                    int save_rc = XGBoosterSaveModel(booster, fv->auto_stamp_path);
                    if (save_rc != 0) {
                        fprintf(stderr, "[mh-train] horizon %d: SaveModel(%s) failed: %s\n",
                                horizon_ticks, fv->auto_stamp_path,
                                XGBGetLastError() ? XGBGetLastError() : "(null)");
                    }
                } else {
                    fprintf(stderr, "[mh-train] horizon %d: 0 boosting rounds "
                            "completed (%s) — NOT saving over %s\n",
                            horizon_ticks,
                            TrainingSink_IsCancelled(cancel) ? "cancelled"
                                             : "first round failed or n_estimators==0",
                            fv->auto_stamp_path);
                }
                XGBoosterFree(booster);
                XGDMatrixFree(dtrain);
            }
            free(train_features);
            free(train_labels);
        }
    }
#endif

    TrainingSink_Status(sink.status, sink.status_cap,
             "h=%d: WF + held-out (%d folds)...",
             horizon_ticks, snap_n_splits);

    // E.1.2.C — hand the SAME snapshot to validation. Without this the WF folds
    // trained at 6/0.1/200 + four cfg overrides, the held-out model at pure
    // defaults, and the stamp recorded a third story — while the booster above
    // used the operator's values. Four descriptions of one run.
    Backtest_RunFullValidation(fv, results, &split,
                                snap_n_splits, horizon_ticks,
                                snap_buffer_ticks, snap_min_train,
                                progress,
                                cancel,
                                label_type, snap_gap_threshold,
                                req.now_us,   // CS-273 — the run's clock
                                /*hp_override=*/&snap_hp);


    // E.1.2.D (scan-1 NEW-6) — `label_kind == 2` was the wrong discriminant:
    // FullValidationResults.label_kind IS num_classes (0=binary, 1=regression,
    // >=2=multiclass) and no FOREACH_TARGET row has num_classes==2, so that
    // branch was unreachable and every REGRESSION horizon displayed/recorded
    // accuracy 0.00 instead of its correlation. Route through the kind SSoT.
    double wf_metric = LabelType_IsRegression(label_type)
        ? fv->walkforward.mean_val_correlation
        : fv->walkforward.mean_val_accuracy;
    double ho_metric = LabelType_IsRegression(label_type)
        ? fv->held_out_correlation : fv->held_out_metric;
    // 2026-09-03 — the per-horizon row shows the GATE metric + direction recall
    // next to the plain pair (E.2/E.3): "bal=0.412 rec c0/c1/c2=0.31/0.47/0.45".
    // Built once, appended to every classification verdict below; empty for
    // regression (the buffer is 256 B — the recall list stays compact).
    char bal_buf[96] = "";
    if (!LabelType_IsRegression(label_type)) {
        const int Kp = (fv->walkforward.num_classes >= 2)
                     ? (fv->walkforward.num_classes > 6 ? 6 : fv->walkforward.num_classes) : 2;
        int off = snprintf(bal_buf, sizeof(bal_buf), " bal=%.3f rec",
                           (double)fv->walkforward.mean_val_balanced_accuracy);
        for (int k = 0; k < Kp && off > 0 && off < (int)sizeof(bal_buf); ++k)
            off += snprintf(bal_buf + off, sizeof(bal_buf) - (size_t)off, "%s%.2f",
                            k == 0 ? " " : "/", (double)fv->walkforward.mean_val_class_recall[k]);
    }

    if (fv->auto_stamp_attempted && fv->auto_stamp_ok) {
        TrainingSink_Status(sink.status, sink.status_cap,
                 "h=%d OK: WF=%.3f HO=%.3f gap=%.3f stamped%s",
                 horizon_ticks, wf_metric, ho_metric,
                 fv->wf_to_held_out_gap, bal_buf);
    } else if (fv->ran_held_out && fv->auto_stamp_attempted) {
        // Class-62 close — a run the gate REFUSED (or whose stamp write failed)
        // is NOT "OK". The old format labeled the refuse branch
        // "h=%d OK: … (stamp skipped: %s)" — a refused run read as OK, and the
        // reason clipped at 128B. auto_stamp_error carries the gate's verdict
        // (skill floor / gap gate) or the writer's error.
        TrainingSink_Status(sink.status, sink.status_cap,
                 "h=%d REFUSED: WF=%.3f HO=%.3f gap=%.3f%s — %s",
                 horizon_ticks, wf_metric, ho_metric, fv->wf_to_held_out_gap, bal_buf,
                 fv->auto_stamp_error[0] ? fv->auto_stamp_error
                                         : "unknown write error");
    } else if (fv->ran_held_out) {
        // stamp not requested (auto_stamp_attempted=0: auto_stamp_on_held_out=0
        // in cfg, OR snap was 0 at click time, OR Run Control hasn't loaded a
        // cfg) — validation itself completed; only the stamp was never asked for.
        TrainingSink_Status(sink.status, sink.status_cap,
                 "h=%d OK: WF=%.3f HO=%.3f gap=%.3f%s (no stamp requested: auto_stamp_on_held_out=0)",
                 horizon_ticks, wf_metric, ho_metric,
                 fv->wf_to_held_out_gap, bal_buf);
    } else if (TrainingSink_IsCancelled(cancel)) {
        TrainingSink_Status(sink.status, sink.status_cap,
                 "h=%d CANCELLED mid-validation", horizon_ticks);
    } else {
        TrainingSink_Status(sink.status, sink.status_cap,
                 "h=%d FAILED: held-out did not complete",
                 horizon_ticks);
    }

    char dst_summary[400];
    // E.1.2.D D-e (operator-decided) — SIDE-SUFFIXED summaries end the
    // buy/exit collision: the exit run used to OVERWRITE summary.txt,
    // destroying the buy record (measured twice — twins and run_1 both
    // lost their entry metrics to it; D4's accept+document disposition
    // failed in practice). Entry runs write summary_entry.txt, exit runs
    // summary_exit.txt; legacy summary.txt on old dirs stays readable via
    // the PastRuns_LoadOne preference chain.
    snprintf(dst_summary, sizeof(dst_summary), "%s/%s", horizon_dir,
             training_side == 1 ? "summary_exit.txt" : "summary_entry.txt");
    FILE *sf = fopen(dst_summary, "w");
    if (sf) {
        TrainingWorkers_WriteSummary(sf, req, role, horizon_dir, num_classes, local_run_cfg, results, fv);
        fclose(sf);
    }

    // 2026-09-03 — the full data-file list, side-addressed like its summary
    // (model-artifact-path-schema-discipline #6). Written from the run cfg the
    // request carries and stamped with BOTH hashes. In the GUI that run cfg is the
    // selection the last Collect / Run Backtest built — the same click that reset the
    // samples and recorded their corpus — so the two always MATCH there; MISMATCH means
    // a caller paired the dataset with another file list, whose labels came from the
    // wrong ticks (CS-280 — MP-3's request contract refuses it).
    if (local_run_cfg) {
        char dst_list[400];
        snprintf(dst_list, sizeof(dst_list), "%s/%s", horizon_dir,
                 training_side == 1 ? MODEL_SIDECAR_DATA_FILES_EXIT
                                    : MODEL_SIDECAR_DATA_FILES_ENTRY);
        FILE *lf = fopen(dst_list, "w");
        if (lf) {
            char sel_sha[65] = "";
            BacktestRunConfig_DataListSha256(local_run_cfg, sel_sha, sizeof(sel_sha));
            const int same = (results->data_list_sha256[0] && sel_sha[0] &&
                              strcmp(results->data_list_sha256, sel_sha) == 0);
            fprintf(lf, "# data files selected at Train click (%d) — the %s record\n",
                    local_run_cfg->num_data_files, training_side == 1 ? "EXIT" : "ENTRY");
            fprintf(lf, "# selection_list_sha256 = %s\n", sel_sha[0] ? sel_sha : "(unknown)");
            fprintf(lf, "# corpus_list_sha256 = %s   (the Collect-time list the samples came from)\n",
                    results->data_list_sha256[0] ? results->data_list_sha256 : "(unknown)");
            fprintf(lf, "# %s\n", same ? "MATCH — this list IS the training corpus"
                                       : "MISMATCH — the selection changed since Collect; the corpus is the "
                                         "Collect-time list (see summary data_first_file / data_last_file)");
            for (int i = 0; i < local_run_cfg->num_data_files && i < MAX_DATA_FILES; ++i)
                fprintf(lf, "%s\n", local_run_cfg->data_paths[i]);
            fclose(lf);
        } else {
            fprintf(stderr, "[mh] h=%d: could not write %s (errno %d)\n",
                    horizon_ticks, dst_list, errno);
        }
    }

    // D-d (2026-08-22, operator-decided) — expected.cfg gains its FIRST live
    // producer, ported from the deleted Save Run block: the mh path emits it
    // per horizon dir, so the load-side VerifyExpected + the cd9c2c7
    // label-direction check stop being vacuous (register #22 / Class 51).
    // NEW-8 dies in the port: num_classes comes from label_table (computed
    // above), never a hand-switch; hyperparams record the CLICK-TIME SNAPSHOT
    // (the dead writer recorded live panel state).
    // 2026-09-03 — SIDE-ADDRESSED (model-artifact-path-schema-discipline #6, the
    // summary_{entry,exit}.txt shape): one shared expected.cfg per horizon dir
    // meant the exit run OVERWROTE the entry record (role=exit, label 5,
    // 0 classes) on every horizon of the operator's best family — the barrier
    // record was gone and nothing said so. entry runs write expected_entry.cfg,
    // exit runs expected_exit.cfg; every reader resolves through
    // ModelPath_ExpectedCfgResolve (side file first, legacy shared name once).
    {
        char dst_expected[400];
        snprintf(dst_expected, sizeof(dst_expected), "%s/%s", horizon_dir,
                 ModelPath_ExpectedCfgName(training_side));
        FILE *ef = fopen(dst_expected, "w");
        if (ef) {
            fprintf(ef, "# auto-generated by foxml_suite multi-horizon train — DO NOT EDIT\n");
            fprintf(ef, "# the engine compares these against engine.cfg at load time.\n");
            fprintf(ef, "# mismatch → warning (default) or failure (model_verify_strict=1).\n");
            fprintf(ef, "# side-addressed: this is the %s record; the other side's record is its sibling file.\n\n",
                    training_side == 1 ? "EXIT" : "ENTRY");
            fprintf(ef, "expected_role = %s\n", role);
            fprintf(ef, "expected_label_type = %d\n", label_type);
            fprintf(ef, "expected_num_classes = %d\n", num_classes);
            fprintf(ef, "\n# ML config the model was trained against. live engine should match.\n");
            fprintf(ef, "ml_buy_threshold = %.3f\n",
                    FPN_ToDouble(results->config_used.ml_buy_threshold));
            fprintf(ef, "ml_tp_pct = %.6f\n", Money_ToDouble(results->config_used.ml_tp_pct));
            fprintf(ef, "ml_sl_pct = %.6f\n", Money_ToDouble(results->config_used.ml_sl_pct));
            fprintf(ef, "ml_backend = %d\n", results->config_used.ml_backend);
            fprintf(ef, "expected_poll_interval = %u\n",
                    results->config_used.poll_interval);
            fprintf(ef, "expected_feature_format_version = %u\n",
                    (unsigned)MODEL_FORMAT_VERSION);
            fprintf(ef, "expected_num_features = %u\n", (unsigned)MODEL_NUM_FEATURES);
            fprintf(ef, "held_out_fraction = %.4f\n",
                    FPN_ToDouble(results->config_used.held_out_fraction));
            fprintf(ef, "gap_acceptable_threshold = %.4f\n",
                    FPN_ToDouble(results->config_used.gap_acceptable_threshold));
            fprintf(ef, "\n# click-time training hyperparameters (informational)\n");
            fprintf(ef, "# max_depth = %d\n", snap_hp.max_depth);
            fprintf(ef, "# learning_rate = %.3f\n", (double)snap_hp.learning_rate);
            fprintf(ef, "# n_estimators = %d\n", snap_hp.n_estimators);
            fclose(ef);
        }
    }

    outcome.validated = fv->ran_held_out;
    outcome.stamped   = fv->auto_stamp_ok;
    g_wf_marker_horizon = prev_marker;
    // F5 — completion goes up LAST, after the model, the stamp, the summary, the data-files list and
    // the expected record are all written (it used to precede the last three).
    TrainingSink_PublishComplete(sink.complete);
    return outcome;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_RunHorizon]
//======================================================================

//======================================================================
// [STRUCT]_[TrainingPool]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[the bounded pool's shared queue — the job function, its context and the job count (fixed before any worker starts) and the next index to claim (the one word the workers write)]
//======================================================================
// [CODE]
//======================================================================
typedef void (*TrainingPoolFn)(void* ctx, int i);
struct TrainingPool {
    TrainingPoolFn  fn;                  // fixed before the workers start
    void*           ctx;
    int             count;
    alignas(64) int next;                // H6 — the one cross-thread-written word (an atomic fetch-add), on its own line
};
static_assert(alignof(TrainingPool) == 64, "TrainingPool's claim counter must sit on its own cache line (H6)");
//======================================================================
// [END_CODE]
//======================================================================
// [DERIVED]
// [UPDATED]_[2026-09-30]
// [SIZE]_[128B]
// [ALIGN]_[64]
// [CACHE_LINES]_[2]
// [STRADDLE]_[none]
// [ORIGIN]_[AUTO]
//======================================================================
// [END_STRUCT]_[TrainingPool]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_RunPool]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[run fn(ctx, i) for every i in [0, count) on at most max_workers threads, the caller one of them — each index exactly once; returns how many threads worked (the caller included)]
//======================================================================
// CS-275 — the cap `multi_horizon_max_threads` promises. The parallel path used to start one thread per
// horizon however low the cap was, and every running horizon copies its training set and builds its own
// DMatrix, so a cap set to bound memory bounded nothing. Workers claim the next index from one counter;
// the CALLER is the last worker, so a failed thread start leaves fewer workers, never an unrun index.
//======================================================================
// [CODE]
//======================================================================
inline void* TrainingWorkers_PoolWorker(void* arg) {
    TrainingPool* p = (TrainingPool*)arg;
    for (int i = __atomic_fetch_add(&p->next, 1, __ATOMIC_RELAXED); i < p->count;
         i = __atomic_fetch_add(&p->next, 1, __ATOMIC_RELAXED))
        p->fn(p->ctx, i);
    return NULL;
}
inline int TrainingWorkers_RunPool(TrainingPoolFn fn, void* ctx, int count, int max_workers) {
    constexpr int TMAX = ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX;   // the grid never needs more
    TrainingPool pool{};
    pool.fn    = fn;
    pool.ctx   = ctx;
    pool.count = count;
    int extra = (max_workers < count ? max_workers : count) - 1;             // the caller is one worker
    if (extra > TMAX - 1) extra = TMAX - 1;
    pthread_t tids[TMAX];
    int spawned = 0;
    for (int k = 0; k < extra; ++k)
        if (pthread_create(&tids[spawned], NULL, TrainingWorkers_PoolWorker, &pool) == 0) ++spawned;
    TrainingWorkers_PoolWorker(&pool);
    // The join orders every worker's writes before the caller reads them.
    for (int k = 0; k < spawned; ++k) pthread_join(tids[k], NULL);
    return spawned + 1;
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_RunPool]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_RunMultiHorizon]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[train a multi-horizon model grid, serial or parallel — one batched label pass, then one TrainingWorkers_RunHorizon per horizon over a private dataset view + run-config copy; fills the caller's result, publishes completion LAST]
// [REFERENCE]_[PARITY]_[PARITY-21]
//======================================================================
// [CODE]
//======================================================================
// One parallel horizon. No OpenMP pinning: since OMP-B (D-494) XGBoost is built without OpenMP, so
// boosters on concurrent workers share no thread pool (the omp_set_* pair that stood in the old
// worker was one of Landmine 1's failed mitigations).
inline void* TrainingWorkers_HorizonThread(void* arg) {
    TrainingHorizonJob* job = (TrainingHorizonJob*)arg;
    // PROGRESS PUBLISH (2026-08-25, operator: the bar read "horizon 0/3 (current: 0 ticks)" for a
    // whole run) — publish the horizon on entry; the completion count is bumped atomically below
    // because N workers finish out of order and a plain increment from several threads loses counts.
    TrainingSink_Set(job->current, job->req.horizon_ticks);
    job->outcome = TrainingWorkers_RunHorizon(job->req, &job->view, &job->run_cfg, job->sink,
                                              job->result);
    if (job->done) __atomic_add_fetch(job->done, 1, __ATOMIC_RELAXED);   // display-only: RELAXED
    return NULL;
}

// One pooled horizon (ctx = the jobs array). A job whose setup failed is NULL — its row already says why.
// A job the run's cancel reaches before it starts never runs (serial's rule) and says so on its row; it
// publishes no completion, because it did not complete.
inline void TrainingWorkers_PoolJob(void* ctx, int h) {
    TrainingHorizonJob* job = ((TrainingHorizonJob**)ctx)[h];
    if (!job) return;
    if (TrainingSink_IsCancelled(job->sink.cancel)) {
        TrainingSink_Status(job->sink.status, job->sink.status_cap,
                            "h=%d not started: the run was cancelled", job->req.horizon_ticks);
        return;
    }
    TrainingWorkers_HorizonThread(job);
}

// A run that stops before it starts: say why, and publish the run's completion.
__attribute__((format(printf, 2, 3)))  // -Werror=format sees every call (cmake/FormatGuard.cmake)
inline void TrainingWorkers_Refuse(const TrainingRunSink& sink, const char* fmt, ...) {
    if (sink.status && sink.status_cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(sink.status, sink.status_cap, fmt, ap);
        va_end(ap);
    }
    TrainingSink_FinishRun(sink);
}

inline void TrainingWorkers_RunMultiHorizon(const TrainingRunRequest& req,
                                            const TrainingRunSink& sink,
                                            TrainingRunResult* out) {
    constexpr int HMAX = ControllerConfig<BACKTEST_FP>::HORIZON_LIST_MAX;
    // v5.11.41 — a new run clears the per-horizon display FIRST, and the run's total stays 0 until the
    // run is accepted: a refused run then shows its reason and no table, never the previous run's rows
    // under this click's horizons (MP-1a review F7).
    for (int h = 0; h < HMAX; ++h) {
        const TrainingHorizonDisplay& hd = sink.horizon[h];
        TrainingSink_Set(hd.complete, 0);
        TrainingSink_Set(hd.progress, 0);
        if (hd.status && hd.status_cap) hd.status[0] = '\0';
    }
    TrainingSink_Set(sink.total, 0);
    if (!out) {
        TrainingWorkers_Refuse(sink, "Multi-horizon: no result storage — refused.");
        return;
    }
    memset((void *)out, 0, sizeof(*out));   // zero BYTES — what the panel's per-horizon clear did
#ifdef USE_XGBOOST
    const int   horizon_count = req.horizon_count;
    const char* run_name      = req.run_name;
    if (horizon_count <= 0) {
        TrainingWorkers_Refuse(sink, "Multi-horizon: cfg.horizon_list empty; set horizons first.");
        return;
    }
    if (horizon_count > HMAX) {
        TrainingWorkers_Refuse(sink, "Multi-horizon: %d horizons exceeds the grid cap %d — refused.",
                               horizon_count, HMAX);
        return;
    }
    // An empty root would put the model tree at the filesystem root ("/<class>/...").
    if (!req.models_root[0] || !req.run_cfg) {
        TrainingWorkers_Refuse(sink, "Multi-horizon: no models root or run config in the request — refused.");
        return;
    }
    if (!req.data || req.data->sample_count <= 0) {
        TrainingWorkers_Refuse(sink, "Multi-horizon: Collect Features first.");
        return;
    }

    fprintf(stderr, "[mh-train] starting multi-horizon train: %d horizons, "
                    "%d samples, run_name='%s'\n",
            horizon_count, req.data->sample_count, run_name);

    // The core's PRIVATE run-config copy (F3) — the label pass and the horizons mutate THIS, never
    // the caller's. ~565 KB and 64-aligned, so the heap, at its alignment (review F1).
    BacktestRunConfig *run_cfg = TrainingWorkers_AllocZeroed<BacktestRunConfig>();
    if (!run_cfg) {
        TrainingWorkers_Refuse(sink, "Multi-horizon: out of memory (run config copy).");
        return;
    }
    *run_cfg = *req.run_cfg;

    // The dataset VIEW (CS-271): a shallow copy of the caller's results whose label pointer is
    // NULLed here and only ever pointed at a buffer the core owns, so nothing below can write the
    // caller's labels (the batch pass reads the sample count, tick indices, regimes and prices).
    BacktestResults view = *req.data;
    view.labels = NULL;

    TrainingSink_Set(sink.total, horizon_count);   // accepted: the panel's per-horizon table appears

    // Parallel horizons run concurrent XGBoost trainings on pthreads — Landmine 1's workload (a SIGSEGV
    // in libgomp's parallel-region setup) until OMP-B (D-494) built XGBoost without OpenMP: there is no
    // runtime pool left for the workers to race on. max_threads picks the mode and caps it: 1 = serial;
    // N >= 2 = at most N horizons at once, on a bounded pool (CS-275 — it used to start one thread per
    // horizon whatever N was). (The registry clamps the cfg knob to [1, 256] with a WARN, so a cfg `0`
    // runs SERIAL; the `<= 0` floor below — "auto" = horizon_count — is reachable only by a direct
    // request write.)
    int mh_max_threads = req.max_threads;
    if (mh_max_threads <= 0) {
        mh_max_threads = horizon_count;  // 0 = auto = fully parallel
    }
    int n_parallel = horizon_count < mh_max_threads ? horizon_count : mh_max_threads;
    int parallel_mode = (n_parallel >= 2 && horizon_count >= 2);

    // E.1.2.D leaf 5 — ONE batched corpus walk labels every horizon up front (was: one full walk per
    // horizon, and in parallel mode N of them running SIMULTANEOUSLY against the same disk). Targets
    // carry the per-horizon (kind, fwd, tp, sl); both modes consume the vectors below and skip the
    // in-place walk. Buffer-alloc failure degrades to the legacy per-horizon walks (mh_batch_ok=0),
    // never to wrong labels. A corpus abort keeps precomputed=1: the NAN prefill makes every horizon
    // refuse on 0 valid labels. (The serial path used to fold the batch's NaN counters into the SHARED
    // results->stats; the core writes no caller buffer, so serial now matches parallel — CS-271.)
    float *mh_label_bufs[HMAX] = {0};
    int mh_batch_ok = 1;
    {
        LabelBatchTarget bt[HMAX];
        for (int h = 0; h < horizon_count; ++h) {
            mh_label_bufs[h] = (float *)malloc(
                (size_t)view.sample_count * sizeof(float));
            if (!mh_label_bufs[h]) { mh_batch_ok = 0; break; }
            bt[h] = LabelBatchTarget{};
            bt[h].label_type    = req.label_type[h];
            bt[h].tp_pct        = (double)req.tp_pct[h];
            bt[h].sl_pct        = (double)req.sl_pct[h];
            bt[h].forward_ticks = req.horizon_ticks[h];
            bt[h].out_labels    = mh_label_bufs[h];
        }
        if (mh_batch_ok) {
            int labeled = Backtest_ComputeLabelsBatch(&view, run_cfg, bt, horizon_count);
            if (labeled < 0) {
                fprintf(stderr, "[mh-train] batched label pass aborted; "
                                "horizons will refuse on 0 valid labels\n");
            }
        } else {
            fprintf(stderr, "[mh-train] label batch buffer alloc failed; "
                            "falling back to per-horizon label walks\n");
        }
    }

    if (parallel_mode) {
        fprintf(stderr, "[mh-train] parallel mode: %d horizons on %d workers "
                        "(one single-threaded booster each)\n",
                horizon_count, n_parallel);

        // v5.11.41.C — every horizon gets a job: zeroed at its alignment, its request from the ONE
        // builder, a dataset view with its own labels (the batch vector, ownership transferred; or the
        // fallback walk's buffer), its own copy of the run config, its own slot in the result, and
        // xgb_train_nthread=1 — the stamp's parallel-mode marker (the MODEL bytes match serial's: every
        // booster is single-threaded since D-494; the marker retires with the key at MP-7b). CS-275: the
        // jobs then run on the bounded pool, at most n_parallel at once; the orchestrator frees every job
        // after the pool returns.
        TrainingHorizonJob *jobs[HMAX] = {0};
        for (int h = 0; h < horizon_count; ++h) {
            if (TrainingSink_IsCancelled(sink.cancel)) {
                for (int r = h; r < horizon_count; ++r)
                    TrainingSink_Status(sink.horizon[r].status, sink.horizon[r].status_cap,
                                        "h=%d not started: the run was cancelled", req.horizon_ticks[r]);
                break;
            }
            const TrainingHorizonDisplay& hd = sink.horizon[h];
            TrainingHorizonJob *job = TrainingWorkers_AllocZeroed<TrainingHorizonJob>();
            if (!job) {
                TrainingSink_Status(hd.status, hd.status_cap,
                                    "h=%d FAILED: malloc job arg", req.horizon_ticks[h]);
                TrainingSink_PublishComplete(hd.complete);
                continue;
            }
            job->req  = TrainingWorkers_HorizonRequest(req, h, mh_batch_ok);
            job->view = view;
            if (mh_batch_ok) {
                job->view.labels = mh_label_bufs[h];
                mh_label_bufs[h] = NULL;   // consumed — the post-join free skips it
            } else {
                // The fallback walk rewrites every label; seeding from the dataset keeps the no-walk
                // edge (collect_features=0) the same as the serial path's.
                job->view.labels = (float *)malloc((size_t)view.sample_count * sizeof(float));
                if (job->view.labels && req.data->labels)
                    memcpy(job->view.labels, req.data->labels,
                           (size_t)view.sample_count * sizeof(float));
            }
            if (!job->view.labels) {
                TrainingSink_Status(hd.status, hd.status_cap,
                                    "h=%d FAILED: malloc labels[]", req.horizon_ticks[h]);
                TrainingSink_PublishComplete(hd.complete);
                free(job);
                continue;
            }
            job->view.config_used.xgb_train_nthread = 1;
            job->run_cfg     = *run_cfg;         // this horizon's own copy
            job->sink        = TrainingSink_ForHorizon(hd, sink.cancel);
            job->result      = &out->horizon[h];
            job->current     = sink.current;
            job->done        = sink.done;
            jobs[h] = job;
        }
        const int workers = TrainingWorkers_RunPool(TrainingWorkers_PoolJob, jobs, horizon_count, n_parallel);
        if (workers < n_parallel)
            fprintf(stderr, "[mh-train] only %d of %d workers started (pthread_create failed); "
                            "every horizon still ran\n", workers, n_parallel);
        for (int h = 0; h < horizon_count; ++h) {
            if (!jobs[h]) continue;
            out->outcome[h] = jobs[h]->outcome;
            free(jobs[h]->view.labels);
            free(jobs[h]);
        }
        // Workers counted themselves up; this is the terminal pin (a cancelled or failed horizon may
        // not have ticked, and the bar must still read complete at the end).
        TrainingSink_Set(sink.done, horizon_count);
    } else {
        fprintf(stderr, "[mh-train] serial mode: %d horizons sequential "
                        "(stamps record xgb_train_nthread=%d from cfg)\n",
                horizon_count, view.config_used.xgb_train_nthread);
        // The no-batch fallback's buffer: one copy of the dataset's labels that the per-horizon walks
        // rewrite in turn — the in-place semantics serial always had, on the core's own buffer.
        float *own_labels = NULL;
        for (int h = 0; h < horizon_count; ++h) {
            if (TrainingSink_IsCancelled(sink.cancel)) {
                fprintf(stderr, "[mh-train] cancelled at horizon %d/%d\n",
                        h, horizon_count);
                for (int r = h; r < horizon_count; ++r)
                    TrainingSink_Status(sink.horizon[r].status, sink.horizon[r].status_cap,
                                        "h=%d not started: the run was cancelled", req.horizon_ticks[r]);
                break;
            }
            TrainingSink_Set(sink.current, req.horizon_ticks[h]);
            TrainingSink_Set(sink.done, h + 1);
            const TrainingHorizonSink hs = TrainingSink_ForHorizon(sink.horizon[h], sink.cancel);
            if (mh_batch_ok) {
                view.labels = mh_label_bufs[h];   // the batch vector itself — nothing copied into a shared buffer
            } else {
                if (!own_labels) {
                    own_labels = (float *)malloc((size_t)view.sample_count * sizeof(float));
                    if (own_labels && req.data->labels)
                        memcpy(own_labels, req.data->labels,
                               (size_t)view.sample_count * sizeof(float));
                }
                view.labels = own_labels;
            }
            if (!view.labels) {
                TrainingSink_Status(hs.status, hs.status_cap,
                                    "h=%d FAILED: malloc labels[]", req.horizon_ticks[h]);
                TrainingSink_PublishComplete(hs.complete);
                continue;
            }
            out->outcome[h] = TrainingWorkers_RunHorizon(
                TrainingWorkers_HorizonRequest(req, h, mh_batch_ok), &view, run_cfg, hs,
                &out->horizon[h]);
        }
        view.labels = NULL;
        free(own_labels);
    }

    // Release the batch vectors (parallel transferred the consumed ones to jobs and NULLed them;
    // free(NULL) is a no-op for every consumed or never-allocated slot).
    for (int h = 0; h < horizon_count; ++h) free(mh_label_bufs[h]);
    free(run_cfg);

    // F8 — the tally comes from the outcomes the jobs returned, never from the display.
    for (int h = 0; h < horizon_count; ++h) {
        out->trained   += out->outcome[h].trained;
        out->validated += out->outcome[h].validated;
        out->stamped   += out->outcome[h].stamped;
    }
    TrainingSink_Status(sink.status, sink.status_cap,
                        "Multi-horizon: %d/%d horizons trained, %d validated (held-out), "
                        "%d stamped. Models in %s/<class>/%s/%s*/.",
                        out->trained, horizon_count, out->validated, out->stamped,
                        req.models_root, run_name, MODEL_HORIZON_PREFIX);
    TrainingSink_FinishRun(sink);
#else
    (void)req;
    TrainingWorkers_Refuse(sink, "Multi-horizon: XGBoost not compiled in (build with -DUSE_XGBOOST=ON)");
#endif
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_RunMultiHorizon]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_FvRequestIdentity]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[fill a Run Full Validation request's identity from its click-time sources — the dataset, the model path and the role its file name names, the stamp secret by the one rule, the labels' identity from the run config that produced them; GUI-free, so the cells drive it]
//======================================================================
// MP-1b review F2 (Class 64): CS-274 (a 64-byte secret snapshot) and CS-277 (no cfg fallback) both lived
// in the panel's click handler, where no test reaches. The panel's request builder calls this for every
// field those bugs touched, and tests call the same function.
//======================================================================
// [CODE]
//======================================================================
inline void TrainingWorkers_FvRequestIdentity(TrainingFvRequest* r,
                                              const char* model_path, size_t model_path_cap,
                                              const char* panel_secret, size_t panel_secret_cap,
                                              const BacktestResults* data,
                                              const BacktestRunConfig* run_cfg) {
    if (!r) return;
    r->data = data;
    {
        size_t n = model_path ? strnlen(model_path, model_path_cap) : 0;
        if (n >= sizeof(r->model_path)) n = sizeof(r->model_path) - 1;
        if (n) memcpy(r->model_path, model_path, n);
        r->model_path[n] = '\0';
    }
    Training_RoleFromModelFile(r->model_path, r->role, sizeof(r->role));   // F10
    // CS-277 — the one secret rule (the panel's field, else the collected cfg's); CS-274 — whole.
    TrainingWorkers_ResolveStampSecret(panel_secret, panel_secret_cap,
                                       data ? data->config_used.auto_stamp_secret : NULL,
                                       data ? sizeof(data->config_used.auto_stamp_secret) : 0,
                                       r->stamp_secret, sizeof(r->stamp_secret));
    if (run_cfg) {
        r->label_type              = run_cfg->label_type;
        r->label_forward_ticks     = run_cfg->label_forward_ticks;
        r->label_tp_pct            = run_cfg->label_tp_pct;
        r->label_sl_pct            = run_cfg->label_sl_pct;
        r->label_roundtrip_fee_pct = run_cfg->label_roundtrip_fee_pct;   // D-476
        r->feature_mask            = run_cfg->feature_mask;              // D-477
    }
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_FvRequestIdentity]
//======================================================================

//======================================================================
// [FUNCTION]_[TrainingWorkers_RunFullValidation]
//----------------------------------------------------------------------
// [TAG]_[[ML] [BACKTEST]]
// [SCHEMA]_[v1.0]
// [OVERVIEW]_[Run Full Validation — WF on the train+val slice, the held-out evaluation and its gap gate, and the re-stamp of the model at the request's path when the gate passes; fills the caller's result, publishes completion LAST]
//======================================================================
// v5.8.7 — the full-validation job, moved here by MP-1b (it was fullvalidation_worker_fn in the panel
// file). Behaviour-neutral but for: the role the stamp records comes from the request (F10 — the panel
// derives it from the file name, as the worker did); the secret travels at its full size (CS-274) and
// follows the one secret rule (CS-277, applied by the panel's request builder); completion is published
// with release (F6). The role still does not reach the stamp: Backtest_RunFullValidation's entry reset
// wipes req_role (CS-216 — MP-3's).
//======================================================================
// [CODE]
//======================================================================
inline void TrainingWorkers_RunFullValidation(const TrainingFvRequest& req, const TrainingFvSink& sink,
                                              FullValidationResults* out) {
    if (!out || !req.data) {
        if (out) memset((void *)out, 0, sizeof(*out));   // completion is published: no stale result behind it
        TrainingSink_Status(sink.status, sink.status_cap,
                            "Full validation: no dataset or result storage in the request — refused.");
        TrainingSink_Finish(sink.complete, sink.running);
        return;
    }
    const BacktestResults* data = req.data;
    // Backtest_RunFullValidation writes progress and reads cancel through non-null pointers.
    volatile int  progress_unwired = 0;
    volatile int  cancel_never     = 0;
    volatile int* progress = sink.progress ? sink.progress : &progress_unwired;
    volatile int* cancel   = sink.cancel   ? sink.cancel   : &cancel_never;

    // Build held-out split and unlock immediately. The friction-grade lock
    // exists to make held-out access a deliberate operator action; the
    // suite UI's Run Full Validation button is exactly that deliberate
    // action, so unlocking here is correct.
    // E.1.2.C — the click-time fraction. This determined the held-out split size,
    // hence held_out_metric, hence the generalization gap that gates auto-stamp —
    // off a live slider read from a worker thread.
    HeldOutSplit split = HeldOutSplit_Make(data->sample_count,
                                            (double)req.held_out_fraction);
    char unlock_token[33];
    memcpy(unlock_token, split.lock_token, sizeof(unlock_token));
    HeldOutSplit_Unlock(&split, unlock_token);

    // Pre-populate auto-stamp request fields. Backtest_RunFullValidation
    // gates the stamp_write_for_model call on auto_stamp_path being non-empty
    // AND ran_held_out=1; both are met here when training succeeds.
    //
    // v5.8.10 — gate path-setting on the cfg's auto_stamp_on_held_out flag.
    // When the operator runs the suite with auto_stamp_on_held_out=0 (originally:
    // for manual stamping via tools/stamp_model.sh; bash CLI DELETED at .B.3 Path C
    // 2026-05-24; =0 now only meaningful for v5.16+ cmdline-invocable training per
    // decoupling-endgoal-roadmap), the FV button still runs held-out validation
    // but skips the stamp write. Honors operator intent.
    memset((void *)out, 0, sizeof(*out));   // zero BYTES, as the panel worker did
    const int auto_stamp_enabled = data->config_used.auto_stamp_on_held_out;
    if (auto_stamp_enabled) {
        // v5.10.0E — the click-time snapshot of the model path (the request's).
        size_t n = strnlen(req.model_path, sizeof(req.model_path));
        if (n >= sizeof(out->auto_stamp_path))
            n = sizeof(out->auto_stamp_path) - 1;
        memcpy(out->auto_stamp_path, req.model_path, n);
        out->auto_stamp_path[n] = '\0';
        // E.1.2.C 3-role (F1, per the D2 verdict) — the FV re-stamp was the ONE
        // production emit path that omitted expected_role. The role is the request's
        // own field now (F10); "" leaves it empty (legacy).
        snprintf(out->req_role, sizeof(out->req_role), "%.*s",
                 (int)strnlen(req.role, sizeof(req.role)), req.role);
    }
    {
        // The secret, whole: the request field is the destination's own size (CS-274).
        size_t n = strnlen(req.stamp_secret, sizeof(req.stamp_secret));
        if (n >= sizeof(out->auto_stamp_secret))
            n = sizeof(out->auto_stamp_secret) - 1;
        memcpy(out->auto_stamp_secret, req.stamp_secret, n);
        out->auto_stamp_secret[n] = '\0';
    }
    out->auto_stamp_format_version = 0;  // 0 = use MODEL_FORMAT_VERSION

    // v5.11.41 — the per-horizon label params from the click-time BacktestRunConfig
    // (single-horizon path; the multi-horizon job populates these per horizon).
    out->req_label_lookahead_ticks = req.label_forward_ticks;
    // D-476 — the single-horizon twin of the multi-horizon stamp seam: the same
    // kind-aware rule (Label_Stamp*Pct), with the click-time fee so a percent TP
    // stamps tp+fee here exactly as the multi-horizon seam does (PARITY-065 (3)).
    out->req_label_tp_pct = Label_StampTpPct(req.label_type, req.label_tp_pct,
                                             req.label_roundtrip_fee_pct);
    out->req_label_sl_pct = Label_StampSlPct(req.label_type, req.label_sl_pct);
    out->req_feature_mask = req.feature_mask;   // D-477

    // E.1.2.C — every argument is the CLICK-TIME snapshot (they were live `state->` reads from a
    // worker thread; the label_type one needed no race at all — change the combo between Collect
    // and this click).
    Backtest_RunFullValidation(out, data, &split,
                               req.wf_n_splits, req.wf_horizon_ticks,
                               req.wf_buffer_ticks, req.wf_min_train,
                               progress, cancel,
                               req.label_type, req.gap_threshold,
                               req.now_us,   // CS-273
                               /*hp_override=*/&req.hp);

    // A one-line status summary for the panel.
    if (out->auto_stamp_attempted) {
        if (out->auto_stamp_ok) {
            TrainingSink_Status(sink.status, sink.status_cap,
                                "Stamp written: %s", out->auto_stamp_path_written);
        } else {
            TrainingSink_Status(sink.status, sink.status_cap,
                                "Stamp REFUSED: %s", out->auto_stamp_error);
        }
    } else if (out->ran_held_out) {
        if (!auto_stamp_enabled) {
            TrainingSink_Status(sink.status, sink.status_cap,
                                "Held-out OK; auto-stamp disabled (cfg auto_stamp_on_held_out=0)");
        } else if (req.model_path[0] == '\0') {
            // v5.9.4a / v5.10.0E — the model path as it was AT CLICK TIME (the panel's button needs a
            // non-empty path, so this is a headless caller's empty request).
            TrainingSink_Status(sink.status, sink.status_cap,
                                "Held-out OK; auto-stamp skipped — model_path was empty in the request "
                                "(set Model Path BEFORE clicking Run Full Validation)");
        } else if (out->auto_stamp_path[0] == '\0') {
            // Truly unexpected — the request's path was non-empty but the copy didn't populate.
            TrainingSink_Status(sink.status, sink.status_cap,
                                "Held-out OK; auto-stamp skipped — model_path='%.*s' "
                                "non-empty but auto_stamp_path empty "
                                "(internal copy failure; report bug)",
                                (int)strnlen(req.model_path, sizeof(req.model_path)), req.model_path);
        } else {
            TrainingSink_Status(sink.status, sink.status_cap,
                                "Held-out OK; auto-stamp skipped — Backtest_RunFullValidation "
                                "did not fire stamp_write (auto_stamp_path='%s'; check "
                                "ran_held_out flag + path validity)",
                                out->auto_stamp_path);
        }
    } else {
        TrainingSink_Status(sink.status, sink.status_cap,
                            "Held-out did not complete (cancel or shape error?)");
    }
    // F6 — the result is complete: publish it LAST (release); the panel reads with acquire.
    TrainingSink_Finish(sink.complete, sink.running);
}
//======================================================================
// [END_CODE]
//======================================================================
// [END_FUNCTION]_[TrainingWorkers_RunFullValidation]
//======================================================================
