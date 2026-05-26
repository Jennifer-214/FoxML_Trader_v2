// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [CONTROLLER CONFIG]
//======================================================================================================
// configuration for the portfolio controller - all tunable parameters in one
// place parsed from a simple key=value text file, no JSON, no external libs
//======================================================================================================
#ifndef CONTROLLER_CONFIG_HPP
#define CONTROLLER_CONFIG_HPP

#include "../Limits.hpp"                       // MAX_EXECUTION_CORES (per-core sharding cap; PerCoreCfg<F> cores[] sizing)
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ConfidenceScore.hpp"  // v5.14.9.A — DegradationCurve enum + ToString/FromString helpers
#include "LifecycleCfgFlagRegistry.hpp"       // v5.14.9.F — FOREACH_LIFECYCLE_CFG_FLAG + MASK_LIFECYCLE_CFG_*
#include "GateCfgFlagRegistry.hpp"            // v5.14.9.F.1 — FOREACH_GATE_CFG_FLAG + MASK_GATE_CFG_*
#include "../ML_Headers/MlCfgFlagRegistry.hpp" // v5.14.9.F.2 — FOREACH_ML_CFG_FLAG + MASK_ML_CFG_*
#include "../ML_Headers/BanditAlgorithmRegistry.hpp" // v5.14.10.B — BanditAlgorithm_FromString for cfg.bandit_algorithm parser
#include "../ML_Headers/BarrierBlendModeRegistry.hpp" // v5.15.5.A.5 — BarrierBlendMode_FromString + MODE_BARRIER_BLEND_COUNT for cfg.barrier_blend_mode parser
#include "RiskCfgFlagRegistry.hpp"             // v5.14.9.F.3 — FOREACH_RISK_CFG_FLAG + MASK_RISK_CFG_*
#include "OpsCfgFlagRegistry.hpp"              // v5.14.9.F.3 — FOREACH_OPS_CFG_FLAG + MASK_OPS_CFG_*
#include "SessionPhaseRegistry.hpp"            // v5.15.5.B.5 — FOREACH_SESSION_PHASE + SESSION_BY_HOUR[24] (closes TECH_DEBT-040)
#include "CfgFieldRegistry.hpp"                // v5.15.5.F.4b — universal cfg field registry (FOREACH_CFG_FIELD + CfgFieldDescriptor)
#include "CfgFieldDispatch.hpp"                // v5.15.5.F.4b — tt:: type-trait dispatch (3-barrier Class 23 fix)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>      // v5.11.18a: ERANGE detection in feature_mask hex parser

//======================================================================================================
// [ENGINE MODE]
//======================================================================================================
// Phase 13 of the per-core sharding migration. Selects which hot-path
// architecture the engine runs:
//   ENGINE_MODE_SINGLE_CORE (default): legacy single-threaded engine. all
//     existing behavior unchanged. PortfolioController_Tick walks the
//     portfolio bitmap on every tick.
//   ENGINE_MODE_SHARDED: experimental per-core risk-sharded engine. one
//     position per pinned cpu core, controller core drains events on its
//     own thread. branchless ~60ns hot path. requires num_execution_cores
//     to be set; defaults to 4.
//
// engine_mode is STARTUP-ONLY — changes via hot reload are ignored. switching
// modes requires a restart so the thread layout can be torn down and rebuilt.
//======================================================================================================
constexpr uint8_t ENGINE_MODE_SINGLE_CORE = 0;
constexpr uint8_t ENGINE_MODE_SHARDED = 1;

// v5.15.2 — TradingMode constants (paper / live / shadow). File-scope
// uint8_t constants matching ENGINE_MODE_* / RECONCILE_MODE_* style.
// Paper = default safe-mode (gates default-off; no live exchange writes).
// Live = pre-flight REFUSE on missing safety items (held_out_stamp_secret,
// mlockall, per-core strategy, ...). Shadow = future (live data + simulated
// fills). Stamp-bound via FOREACH_STAMP_BOUND_CFG (every model carries
// training-time mode for audit trail).
constexpr uint8_t TRADING_MODE_PAPER  = 0;
constexpr uint8_t TRADING_MODE_LIVE   = 1;
constexpr uint8_t TRADING_MODE_SHADOW = 2;

// v5.15.4 — ControllerConfig_NormalizeForMode key-explicit tracking.
// Parser sets the corresponding bit when the operator explicitly sets a
// key in the cfg file; normalize honors the explicit value. Bits unset
// → default; normalize applies mode-specific override (e.g.,
// trading_mode=LIVE flips model_verify_strict 0→1 unless operator
// explicitly set it).
//
// Cohort discipline per CLAUDE.md item 20 (bitmap-flag-api): bit-pack
// from start; adding next tracked key = 1 bit + 1 parser-side OR. 16 bits
// of headroom; expected to grow with future mode-specific flip rules.
constexpr uint16_t MASK_CFG_KEY_MODEL_VERIFY_STRICT = 1u << 0;
constexpr uint16_t MASK_CFG_KEY_RECONCILE_MODE      = 1u << 1;
// Reserved bits 2..15 for future tracked keys.

//======================================================================================================
// [PER-CORE OVERRIDES — v4.0]
//======================================================================================================
// One slot per execution core (16 max). Each field shadows a same-named
// `ControllerConfig` field; non-zero overrides the global; zero (default)
// means "inherit from global". Resolved per-core via
// `ControllerConfig_ResolveForCore(global, core_id)` on every slow-path
// rebuild.
//
// Why this exists: pre-4.0, each strategy *type* had its own override
// (`mr_tp_pct`, `momentum_tp_mult`, etc.) but those overrides were
// account-wide — Core 0 running MR @ 4% TP and Core 1 running MR @ 6% TP
// was impossible. Per-core overrides let you A/B identical strategies
// with different tunings on different cores, without touching the global
// defaults.
//
// Cfg syntax: `core_N_<field>=<value>`
//   core_0_take_profit_pct=4.0
//   core_1_take_profit_pct=6.0
//   core_2_mr_tp_pct=3.5
//   core_0_ml_buy_threshold=0.6
//
// Adding a field: extend this struct + add the override line in
// `ControllerConfig_ResolveForCore` + add a parser case in
// `ControllerConfig_Load`'s per-core block + surface in the Settings
// panel's per-core tab. Four sites, all in this file + SettingsPanel.hpp.
//======================================================================================================
// v4.7.24: per-core override fields driven by ONE X-macro list. Adding a
// new override = ONE line in this macro. Struct members, init zeroing,
// resolver overwrite logic, and cfg parser ALL auto-derive from this list.
//
// PCT(name)  — percentage field; cfg writes 4.0, stored as 0.04. Parser
//              divides by 100.0 at load.
// RAW(name)  — raw FPN field; cfg writes 3.0, stored as 3.0. Parser uses
//              atof directly.
//
// Field NAMES MUST MATCH same-named members of ControllerConfig exactly —
// the resolver does a direct field-by-field overwrite by name.
//
// Pre-v4.7.24 this required 4 separate edits across this file (struct,
// init, resolver, parser) — easy to forget one site and silently lose
// the override on a new field. The X-macro collapses that to one line.
#define PER_CORE_OVERRIDE_FIELDS(PCT, RAW) \
    /* Trading */ \
    PCT(take_profit_pct) \
    PCT(stop_loss_pct) \
    RAW(fee_floor_mult) \
    /* Entry filters */ \
    PCT(entry_offset_pct) \
    RAW(volume_multiplier) \
    RAW(spacing_multiplier) \
    RAW(offset_stddev_mult) \
    /* Strategy-specific */ \
    PCT(simpledip_tp_pct) \
    PCT(simpledip_sl_pct) \
    PCT(mr_tp_pct) \
    PCT(mr_sl_pct) \
    RAW(momentum_tp_mult) \
    RAW(momentum_sl_mult) \
    RAW(momentum_r2_min) \
    PCT(emacross_tp_pct) \
    PCT(emacross_sl_pct) \
    RAW(emacross_dip_mult) \
    RAW(emacross_crossover_min) \
    RAW(emacross_trail_mult) \
    PCT(ml_tp_pct) \
    PCT(ml_sl_pct) \
    RAW(ml_buy_threshold) \
    /* v4.7.29: Adaptation overrides — adaptive feedback per core */ \
    RAW(filter_scale) \
    RAW(r2_threshold) \
    RAW(slope_scale_buy) \
    RAW(max_shift) \
    PCT(offset_min) \
    PCT(offset_max) \
    RAW(vol_mult_min) \
    RAW(vol_mult_max) \
    /* v4.7.29: Trailing TP/SL overrides — exit ratchet per core */ \
    RAW(tp_hold_score) \
    RAW(tp_trail_mult) \
    RAW(sl_trail_mult) \
    /* v4.7.29: Time exit override — holding period per core */ \
    PCT(min_hold_gain_pct) \
    /* v4.7.29: Vol sizing overrides — position scale curve per core */ \
    RAW(vol_scale_min) \
    RAW(vol_scale_max) \
    /* v4.7.29: No-trade band override — entry gate strictness per core */ \
    RAW(no_trade_band_mult) \
    /* v4.7.29: Partial exit geometry overrides — TP1 split + TP2 mult per core */ \
    RAW(partial_exit_pct) \
    RAW(tp2_mult) \
    /* v4.7.31: ML/FoxML overrides — different ML cores can have different */ \
    /* confidence behavior, vol-z scaling, bandit blend ratios, etc. */ \
    RAW(foxml_vol_scaling_z_max) \
    RAW(bandit_blend_ratio) \
    /* v5.14.9.D — DELETED RAW(confidence_freshness_tau) per-core override. */ \
    /* Cfg field deleted (TECH_DEBT-004); legacy tau hardcoded internally. */ \
    RAW(confidence_threshold_scale) \
    /* v5.14.1.D: per-core winsor override — supports heterogeneous winsor */ \
    /* models (e.g. 3 buy_signal models with different winsor settings each */ \
    /* on its own core). 0 = inherit global cfg.winsor_pct_*; non-zero overrides. */ \
    /* Field name matches global ControllerConfig field per resolver contract. */ \
    RAW(winsor_pct_low) \
    RAW(winsor_pct_high) \
    /* v5.14.9.B.1: per-core soft risk degradation ladder thresholds — */ \
    /* operator can tune ladder shape per core (e.g. aggressive core 0 with */ \
    /* full=0.20 + min_pct=0.05, conservative core 1 with full=0.10 + min_pct=0.20). */ \
    /* RUNTIME-ONLY overrides (NOT stamp-bound; matches existing per-core risk_pct */ \
    /* precedent — operator policy, not training-derived). 0 = inherit global. */ \
    /* curve enum (risk_degradation_curve) is INT-typed; see PER_CORE_OVERRIDE_INT_FIELDS below. */ \
    RAW(risk_full_size_threshold) \
    RAW(risk_min_size_threshold) \
    RAW(risk_min_size_pct)

// v4.7.40: INT-typed per-core overrides. Separate macro because INT fields
// are uint32_t (not FPN<F>) — different declaration + parser. 0 = inherit
// (caller checks `if (override == 0) use cfg.field`). All sentinel-friendly
// (cfg INT defaults are non-zero meaningful values).
#define PER_CORE_OVERRIDE_INT_FIELDS(INT) \
    /* v4.7.40: per-core slow-path cadence. Each engine can poll faster or */ \
    /* slower than the global poll_interval — fast strategies (momentum) */ \
    /* may want tighter rebuilds; slow strategies (MR) tolerate longer. */ \
    INT(poll_interval) \
    /* v5.14.9.B.1: per-core ladder curve override (CURVE_OFF/LINEAR/EXP/STEP). */ \
    /* 0 = inherit global cfg.risk_degradation_curve; non-zero overrides. Sentinel: */ \
    /* operator who wants to FORCE OFF on a specific core when global=LINEAR sets */ \
    /* core_N_risk_degradation_curve=0 (which is also the inherit signal — but */ \
    /* the global being non-zero means inherit yields LINEAR; cannot per-core- */ \
    /* disable). Trade-off accepted; matches existing INT-override sentinel pattern. */ \
    INT(risk_degradation_curve) \
    /* v5.15.5.A.5: per-core barrier blend mode override (LEGACY/BLEND/DOMINANT/ */ \
    /* BOTH_BLEND_DRIVES/BOTH_DOMINANT_DRIVES). 0 = inherit global */ \
    /* cfg.barrier_blend_mode; non-zero overrides per-core. Same sentinel */ \
    /* trade-off as risk_degradation_curve (cannot per-core-disable when */ \
    /* global is non-zero). Mirror of v5.14.9.C precedent. */ \
    INT(barrier_blend_mode)

// v5.14.9.F.6: BITMAP-typed per-core overrides. Each domain bitmap on
// ControllerConfig (lifecycle/gate/ml/risk/ops_cfg_flags) gets a per-core
// override PAIR: <domain>_cfg_flags_override (the override values) +
// <domain>_cfg_flags_override_set (mask of which bits are overridden).
//
// Per-bit override semantics: `core_3_partial_exit_enabled = 1` sets the
// PARTIAL_EXIT_ENABLED bit in core 3's lifecycle override + sets the
// corresponding bit in lifecycle_override_set so the resolver knows to use
// the override value (not global) for that specific bit. Other bits in the
// domain stay inherited from global.
//
// Resolution (in ControllerConfig_ResolveForCore): branchless bit-select.
//   resolved = (override_set & override_values) | (~override_set & global_values)
//
// Adding a new domain to FOREACH_<DOMAIN>_CFG_FLAG family = 1 row in
// PER_CORE_OVERRIDE_BITMAP_DOMAINS below → declare + zero + resolve + parse
// all auto-flow.
//
// Tuple: X(domain_lower, DOMAIN_UPPER, storage_type, FOREACH_macro)

#define PER_CORE_OVERRIDE_BITMAP_DOMAINS(X)                                              \
    X(lifecycle, LIFECYCLE, uint8_t,  FOREACH_LIFECYCLE_CFG_FLAG)                        \
    X(gate,      GATE,      uint8_t,  FOREACH_GATE_CFG_FLAG)                             \
    X(ml,        ML,        uint16_t, FOREACH_ML_CFG_FLAG)                               \
    X(risk,      RISK,      uint8_t,  FOREACH_RISK_CFG_FLAG)                             \
    X(ops,       OPS,       uint8_t,  FOREACH_OPS_CFG_FLAG)

template <unsigned F> struct PerCoreOverrides {
#define _DECL_OV_FIELD(name) FPN<F> name;
    PER_CORE_OVERRIDE_FIELDS(_DECL_OV_FIELD, _DECL_OV_FIELD)
#undef _DECL_OV_FIELD
// v4.7.40: INT-typed overrides. uint32_t storage; 0 = inherit.
#define _DECL_OV_INT_FIELD(name) uint32_t name;
    PER_CORE_OVERRIDE_INT_FIELDS(_DECL_OV_INT_FIELD)
#undef _DECL_OV_INT_FIELD
// v5.14.9.F.6: BITMAP-typed overrides. <domain>_cfg_flags_override holds the
// override VALUES; <domain>_cfg_flags_override_set is the MASK of which bits
// are overridden (others inherit global). 0 = no overrides for this domain.
#define _DECL_OV_BITMAP_FIELDS(d_lower, D_UPPER, stype, FOREACH_macro) \
    stype d_lower##_cfg_flags_override;     \
    stype d_lower##_cfg_flags_override_set;
    PER_CORE_OVERRIDE_BITMAP_DOMAINS(_DECL_OV_BITMAP_FIELDS)
#undef _DECL_OV_BITMAP_FIELDS
};

// v5.9.2c — CSV tick-sort validation modes (csv_sort_check_mode field).
// Backtest path checks tick timestamp ordering post-load; live engine
// doesn't load CSV (Binance WS is in-order by construction).
#define CSV_SORT_WARN   0   // default — log violations, proceed
#define CSV_SORT_STRICT 1   // refuse load on any violation
#define CSV_SORT_AUTO   2   // sort in-place + INFO log of violation count

//======================================================================================================
// [PER-CORE CONFIG — v5.15.5.F.4c.3 first canonical application of per-instance registry pattern]
//======================================================================================================
// 79 per-core scalar fields. Types preserved exactly from the existing flat
// ControllerConfig<F> declarations. .F.4c.3 Step 2 shadow window: BOTH the
// flat ControllerConfig<F>::<field> AND the per-core cfg.cores[c].<field>
// exist. Parser populates cores[0] from flat after parse (Step 3 will add
// [core N] section parser). Consumers migrate to cfg.cores[c].<field> one at
// a time; flat fields delete after the last consumer migrates.
//
// DESIGN_SPECS/per-instance-registry-pattern.md — this struct is the first
// canonical application; future axes (per-symbol, per-strategy, per-horizon,
// per-regime) instantiate sister PerXxxCfg<F> structs from sister registries.
//
// Layout: alignas(64) — fits AVX-512 boundary; per-core instances at
// cores[c] address cleanly to cache lines without cross-core false sharing.
// Compiler auto-pads sizeof to a multiple of 64 for cores[] array alignment.
//======================================================================================================
template <unsigned F>
struct alignas(64) PerCoreCfg {
    // === Cfg surface (92 fields) — auto-generated from FOREACH_PER_CORE_FIELD_TYPE ===
    // WIP2d-0 structural fix per CLAUDE.md item 31 + DESIGN_PHILOSOPHY § 1.5
    // (framework discipline) + H17 STRONG codification:
    //
    // Single-path field declaration via X-macro; manual cfg-surface field
    // declarations FORBIDDEN here. CI cross-check (tools/check_per_core_registry_integrity.py
    // invoked from build.sh) enforces:
    //   - Every FOREACH_PER_CORE_FIELD_TYPE entry has a FOREACH_PER_CORE_CFG_FIELD row
    //   - Every FOREACH_PER_CORE_CFG_FIELD row has a FOREACH_PER_CORE_FIELD_TYPE entry
    //   - PerCoreCfg<F> body has NO manual cfg-surface fields outside the X-macro
    //     (only the 5-field runtime bitmap cluster below is documented exempt)
    //
    // Field types follow DOD discipline per DESIGN_PHILOSOPHY § 3:
    //   - FPN<F>: accounting math (H4 invariant — 69 fields)
    //   - uint32_t / uint64_t: counters, ticks, seeds (12 + 1 fields)
    //   - int: signed enums, signed counters (10 fields)
    //   - double: ML voting threshold exemption (ensemble_min_agreement_pct only)
    //
    // Field ORDER matches FOREACH_PER_CORE_FIELD_TYPE row order — sections grouped
    // (Trading → Entry → TimeExit → Risk → Gate → Strategy → Regime → ML → ...).
    // Order preserved bytewise-identical to pre-WIP2d-0 manual struct for layout
    // determinism + alignment static_asserts continuity.
    //
    // SEE DESIGN_SPECS/manual-fields-inventory-pattern.md for the full pattern doc.
    FOREACH_PER_CORE_CFG_FIELD(EMIT_PER_CORE_CFG_STRUCT_FIELD)

    // === Runtime bitmap cluster (5 fields — auto-generated via FOREACH_PER_CORE_DOMAIN_BITMAP meta-registry) ===
    // WIP2d-0.B (.F.4c.3) — meta-registry pattern applied per CLAUDE.md item 31 framework
    // discipline. The 5 bitmap fields are RUNTIME representations of FOREACH_*_CFG_FLAG bits,
    // rebuilt from flat KIND_BOOL rows at slow-path rebuild (WIP2e adds the rebuild walker).
    //
    // SINGLE SOURCE OF TRUTH: FOREACH_PER_CORE_DOMAIN_BITMAP (in CfgFieldRegistry.hpp). Adding
    // a new domain registry = 1 row in the meta-registry; struct field + bitmap-overflow
    // static_assert + future WIP2e rebuild walker entry all auto-flow.
    //
    // alignas(8) on the FIRST field (lifecycle_cfg_flags) preserves cluster boundary alignment
    // for atomic multi-byte bitmap reads (driven by the meta-registry's `align_n` column).
    //
    // SEE DESIGN_SPECS/meta-registry-pattern-for-codebase-registry-discipline.md for the
    // pattern doc + DOCS/MANUAL_FIELDS_INVENTORY.md § Section B for the documented exemption.
    FOREACH_PER_CORE_DOMAIN_BITMAP(EMIT_DOMAIN_BITMAP_FIELD)
};

// alignment / size discipline per DESIGN_SPECS/per-snapshot-cluster-layout-pattern.md.
// alignof must be 64 (alignas(64) decorator above); sizeof must be a multiple of 64
// (compiler auto-tail-pads for array alignment).
static_assert(alignof(PerCoreCfg<64>) == 64,
              "PerCoreCfg<F> must be 64-byte aligned for cache-line discipline + per-core "
              "cores[] array alignment");
static_assert(sizeof(PerCoreCfg<64>) % 64 == 0,
              "sizeof(PerCoreCfg<F>) must be a multiple of 64 — compiler should auto-pad "
              "via alignas(64); if this fires, the struct definition broke the alignment "
              "invariant.");

// WIP2d-1.B.0 — compile-time size-bound discipline (closes Shortsighted #3 to ~99.9%).
// The X-macro knows expected payload bytes (sum of STORAGE_T sizeof from FOREACH_PER_CORE_CFG_FIELD
// + FOREACH_PER_CORE_DOMAIN_BITMAP). Adding a manual field to PerCoreCfg<F> body OUTSIDE the X-macros
// pushes sizeof past the upper bound → BUILD ERROR. See CfgFieldRegistry.hpp § "PerCoreCfg<F>
// expected-payload computation" for rationale + leeway math. Defense-in-depth via the CI script's
// gcc -E parser (tools/check_per_core_registry_integrity.py) catches any remaining ~0.1% adversarial gap.
static_assert(sizeof(PerCoreCfg<64>) >= kPerCoreCfgExpectedPayloadBytes64,
              "PerCoreCfg<64> sizeof less than X-macro payload sum — field removed from registry "
              "without updating struct, OR struct manual content shrunk. Check FOREACH_PER_CORE_CFG_FIELD "
              "+ FOREACH_PER_CORE_DOMAIN_BITMAP row count vs struct body.");
static_assert(sizeof(PerCoreCfg<64>) <= kPerCoreCfgExpectedPayloadBytes64 + kPerCoreCfgMaxPaddingBytes,
              "PerCoreCfg<64> sizeof EXCEEDS X-macro payload + max alignment padding leeway — "
              "MANUAL FIELD ADDED to struct body outside FOREACH_PER_CORE_CFG_FIELD / "
              "FOREACH_PER_CORE_DOMAIN_BITMAP. Per H17 discipline: add field via X-macro row, "
              "or document exemption in MANUAL_FIELDS_INVENTORY.md.");

//======================================================================================================
// [CONFIG]
//======================================================================================================
template <unsigned F> struct ControllerConfig {
  FPN<F> r2_threshold;     // min R^2 to trust regression
  FPN<F> slope_scale_buy;  // how much slope shifts buy price threshold
  FPN<F> max_shift;        // max drift from initial buy conditions
  FPN<F> take_profit_pct;  // per-position take profit (e.g. 0.03 = 3%)
  FPN<F> stop_loss_pct;    // per-position stop loss (e.g. 0.015 = 1.5%)
  FPN<F> starting_balance; // paper trading starting balance (e.g. 10000.0)
  FPN<F> fee_rate;         // per-trade fee rate (e.g. 0.001 = 0.1% for Binance)
                           // Phase 8: legacy field. Pre-Phase-8 behavior preserved
                           // when fee_rate_maker == fee_rate_taker == fee_rate.
                           // Backtest fingerprint hashes this field (NOT the new
                           // maker/taker fields) — preserves bundle compatibility.
  // Phase 8 — bifurcated maker/taker fee rates. Live engine uses these per fill
  // based on order->is_maker (set from Binance executionReport "m" field).
  // Backtest simulates as all-taker (is_maker=0 always). Documented divergence.
  FPN<F> fee_rate_maker;   // maker fill fee rate (e.g. 0.00075 = 0.075% Binance tier 0)
  FPN<F> fee_rate_taker;   // taker fill fee rate (e.g. 0.00100 = 0.100% Binance tier 0)
  // v4.3.2 (Track C.1) — pay fees in BNB on Binance gives a 25% discount
  // on both maker and taker. When set, fee_rate_maker and fee_rate_taker
  // are scaled by 0.75 at engine boot. User must also enable BNB fee
  // payment in Binance UI (one-time account setting). Logged at boot so
  // the config is visible. 0 = disabled (default).
  uint32_t pay_fees_in_bnb;
  // Fee_Compute helper — defined after the struct so all fee math sites
  // share one implementation. See note in main file just below struct.
  FPN<F> risk_pct; // fraction of balance to risk per position (e.g. 0.02 = 2%)
  // market microstructure filters (initial values - adapted at runtime by P&L
  // regression)
  FPN<F> volume_multiplier; // buy only when tick volume >= this * rolling_avg
                            // (e.g. 3.0)
  FPN<F> entry_offset_pct;  // buy gate offset below rolling mean (e.g. 0.0015 =
                            // 0.15%)
  FPN<F> spacing_multiplier; // min entry spacing = stddev * this (e.g. 2.0)
  // adaptation clamps - how far the filters can drift from their initial values
  FPN<F>
      offset_min; // min entry_offset_pct (most aggressive, e.g. 0.0005 = 0.05%)
  FPN<F> offset_max; // max entry_offset_pct (most defensive, e.g. 0.005 = 0.5%)
  FPN<F> vol_mult_min; // min volume_multiplier (most aggressive, e.g. 1.5)
  FPN<F> vol_mult_max; // max volume_multiplier (most defensive, e.g. 6.0)
  FPN<F> filter_scale; // how much P&L slope shifts the filters (e.g. 0.50)
  // risk management
  FPN<F> max_drawdown_pct; // halt trading if total P&L drops below this % of
                           // starting balance (e.g. 0.10 = 10%)
  FPN<F> max_exposure_pct; // max fraction of balance deployed in positions
                           // (e.g. 0.50 = 50%)
  // enhanced buy signal (disabled by default = backward compatible)
  FPN<F> offset_stddev_mult; // stddev-scaled offset multiplier (0 = use
                             // percentage mode)
  FPN<F> offset_stddev_min; // adaptation lower bound for stddev mode (e.g. 0.5)
  FPN<F> offset_stddev_max; // adaptation upper bound for stddev mode (e.g. 4.0)
  FPN<F> min_long_slope;    // min long-window price slope to allow buys (0 =
                            // disabled)
  FPN<F> min_buy_delta;     // min volume delta for MR buys (-0.3 = allow mild
                            // selling, block heavy)
  FPN<F> vwap_offset; // buy below VWAP - (VWAP * this) (0 = disabled, 0.001 =
                      // 0.1% below)
  FPN<F> min_stddev_pct;  // skip trades when stddev/price < this (0 = disabled,
                          // 0.0003 = 0.03%)
  FPN<F> momentum_r2_min; // min R² to enter momentum trades (0 = disabled, 0.4
                          // recommended)
  // trailing take-profit (disabled by default)
  FPN<F> tp_hold_score;  // min SNR*R² to hold past TP (0 = disabled, fixed TP)
  FPN<F> tp_trail_mult;  // trailing distance: stddev * this (e.g. 1.0)
  FPN<F> sl_trail_mult;  // trailing SL distance: stddev * this (e.g. 2.0)
  FPN<F> fee_floor_mult; // TP floor = entry × fee_rate × this (default 3.0,
                         // try 5.0 for wider)
  // risk ratios
  FPN<F>
      min_sl_tp_ratio; // min SL/TP distance ratio (0.5 = 2:1 reward/risk floor)
  FPN<F> ror_tp_bonus; // TP multiplier when ROR positive (1.2 = 20% wider)
  FPN<F> momentum_tp_r2_min; // TP scale at R²=0 (0.5 = half base TP,
                             // conservative on uncertainty)
  FPN<F>
      momentum_sl_r2_max; // SL scale at R²=0 (1.5 = wider SL in choppy markets)
  // adaptation speed
  FPN<F> squeeze_decay;      // idle squeeze rate per cycle (0.10 = 10% of gap)
  FPN<F> offset_adapt_scale; // P&L regression → offset shift (0.001)
  FPN<F> stddev_adapt_scale; // P&L regression → stddev/breakout shift (0.1)
  FPN<F> vol_adapt_scale;    // P&L regression → volume shift (0.1)
  FPN<F> breakout_min;       // momentum breakout floor in stddevs (0.5)
  // time-based exit (disabled by default)
  uint32_t
      max_hold_ticks; // close position if held longer than this (0 = disabled)
  // v5.12.3.C — per-core override of max_hold_ticks. 0 = use global
  // cfg.max_hold_ticks (preserves pre-v5.12.3.C behavior). >0 = this
  // core uses its own time-exit threshold. Useful for mixed paper-test
  // experiments where AUTO-mode core wants longer holds than DIP core,
  // or where strategy comparison wants different time horizons per core.
  // Cfg parser pattern: core_<N>_time_exit_ticks=<int>
  // core_time_exit_ticks: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  FPN<F>
      min_hold_gain_pct; // only time-exit if gain < this % (e.g. 0.001 = 0.1%)
  // regime detection
  FPN<F> regime_slope_threshold;     // relative slope magnitude for TRENDING
                                     // (legacy, kept for compat)
  FPN<F> regime_crossover_threshold; // EMA/SMA spread for mild trend (e.g.
                                     // 0.0005 = EMA Cross)
  FPN<F> regime_strong_crossover;    // EMA/SMA spread for strong trend (e.g.
                                     // 0.0015 = Momentum)
  FPN<F> regime_r2_threshold;        // min R² for TRENDING (e.g. 0.70)
  FPN<F> regime_volatile_stddev;     // stddev/price ratio for VOLATILE (legacy,
                                     // kept for compat)
  FPN<F> regime_vol_spike_ratio;     // variance ratio threshold: short/long
                                     // variance > this = volatile spike
  uint32_t regime_hysteresis;  // slow-path cycles before regime switch (e.g. 5)
                               // use warmup_ticks only). CAPS AT W=128: this
                               // gates on rolling.count which is bounded by
                               // the rolling window size. Values > 128 are
                               // CLAMPED at config load with a warning. If you
                               // want a longer total-tick warmup, use
                               // warmup_ticks instead — it counts raw ticks
                               // and has no upper bound.
  // v5.9.2c — CSV tick-sort validation mode. Backtest path checks that
  // ticks are timestamp-monotonic post-load; live engine doesn't load
  // CSV (Binance WS is in-order by construction). Mode values:
  //   0 = WARN   (default — log violations, proceed with potentially-
  //               garbage features). Backward-compat with pre-v5.9.2c.
  //   1 = STRICT (refuse load on any violation; abort backtest run)
  //   2 = AUTO   (sort the array in-place, log INFO with violation count)
  // Without this guard, unsorted CSVs (concatenated daily exports,
  // mistyped tick replays) silently produce garbage rolling stats /
  // ROR / tick-rate features at training time.
  // post-SL cooldown
  uint32_t sl_cooldown_cycles; // slow-path cycles to pause buying after SL (0 =
                               // disabled)
  int sl_cooldown_adaptive;  // 0 = fixed cycles, 1 = scale by trend confidence
                             // at SL time
  uint32_t sl_cooldown_base; // minimum cooldown cycles (even on spikes)
  uint32_t
      sl_cooldown_extra; // max additional cycles (scaled by trend confidence)
  // gate death spiral recovery
  uint32_t idle_reset_cycles; // slow-path cycles with no fill before gate decay
                              // (0 = disabled)
  // momentum strategy
  FPN<F>
      momentum_breakout_mult; // buy when price > avg + stddev * this (e.g. 1.5)
  // v5.7.5 — MOM quality filters. All default to 0 (filter off,
  // current behavior preserved). Operator opts in after observing
  // v5.7.6 quality dashboard data. Each filter sets BUY_BLOCKED +
  // a SHALT_MOM_* code when triggered, surfaced in the v5.6 GUI.
  // See DOCS/changelogs/2026-04-30-regime-classifier-audit.md for
  // why these are defensive depth even after Phase 2's hardcoded
  // strategy boot guard.
  FPN<F> momentum_min_tp_margin_pct;     // SHALT_MOM_TP_TOO_TIGHT — require tp_pct >= this (recommended: 0.0040 = 0.40%)
  FPN<F> momentum_min_buy_delta_recent;  // SHALT_MOM_NO_FLOW — require recent volume_delta >= this (rec: 0.05)
  FPN<F> momentum_min_r2;                // SHALT_MOM_LOW_R2 — require short_r2 >= this (rec: 0.30)
  int momentum_require_last_win;         // SHALT_MOM_LAST_LOST — 1 = block re-entry until previous trade was TP win (rec: 0=off)
  FPN<F> momentum_tp_mult;    // TP multiplier for momentum (e.g. 3.0 stddevs)
  FPN<F> momentum_sl_mult;    // SL multiplier for momentum (e.g. 1.0 stddevs)
  // EMA cross strategy
  FPN<F> emacross_dip_mult;      // buy this many stddevs below EMA (e.g. 0.5)
  FPN<F> emacross_crossover_min; // min EMA-SMA spread for uptrend confirmation
  FPN<F> emacross_trail_mult;    // trailing TP factor when EMA rising
  // volume spike detection
  FPN<F> spike_threshold;         // volume spike ratio (current/max) to trigger
                                  // (e.g. 5.0 = 5x)
  FPN<F> spike_spacing_reduction; // spacing multiplier during spike (e.g. 0.5 =
                                  // half normal)
  // v5.14.9.F* — DOMAIN CFG FLAG BITMAPS (HOT-CLUSTER per heterogeneous-registry-pattern.md
  // cache-layout discipline). Each bitmap is its own domain registry; adding a new flag
  // in any domain = 1 row in FOREACH_<DOMAIN>_CFG_FLAG. See DESIGN_SPECS docs for pattern.
  //
  // LIFECYCLE (v5.14.9.F): position-exit mechanics — see LifecycleCfgFlagRegistry.hpp
  //   partial_exit_enabled  → MASK_LIFECYCLE_CFG_PARTIAL_EXIT_ENABLED
  //   breakeven_on_partial  → MASK_LIFECYCLE_CFG_BREAKEVEN_ON_PARTIAL
  //   breakeven_on_profit   → MASK_LIFECYCLE_CFG_BREAKEVEN_ON_PROFIT
  //
  // GATE (v5.14.9.F.1): entry/exit gate mechanics — see GateCfgFlagRegistry.hpp
  //   depth_enabled                → MASK_GATE_CFG_DEPTH_ENABLED
  //   gate_ema_enabled             → MASK_GATE_CFG_GATE_EMA_ENABLED
  //   no_trade_band_enabled        → MASK_GATE_CFG_NO_TRADE_BAND_ENABLED
  //   cost_gate_enabled            → MASK_GATE_CFG_COST_GATE_ENABLED
  //   barrier_gate_enabled         → MASK_GATE_CFG_BARRIER_GATE_ENABLED (stamp-bound via FOREACH_STAMP_BOUND_MODEL_CONST)
  //   param_staleness_gate_enabled → MASK_GATE_CFG_PARAM_STALENESS_GATE_ENABLED
  //
  // ML (v5.14.9.F.2): ML/confidence mechanics — see MlCfgFlagRegistry.hpp
  //   confidence_enabled              → MASK_ML_CFG_CONFIDENCE_ENABLED
  //   confidence_composite_enabled    → MASK_ML_CFG_CONFIDENCE_COMPOSITE_ENABLED
  //                                      (stamp-bound via FOREACH_STAMP_BOUND_CFG with emit_source=BITMAP_BIT;
  //                                       Y3 dispatch per heterogeneous-registry-pattern.md Form 3)
  //   bandit_enabled                  → MASK_ML_CFG_BANDIT_ENABLED
  //   exit_bandit_enabled             → MASK_ML_CFG_EXIT_BANDIT_ENABLED
  //   use_exit_model                  → MASK_ML_CFG_USE_EXIT_MODEL
  //   foxml_vol_scaling_enabled       → MASK_ML_CFG_FOXML_VOL_SCALING_ENABLED
  //   lazy_rebuild_enabled            → MASK_ML_CFG_LAZY_REBUILD_ENABLED
  //
  // RISK (v5.14.9.F.3): risk/sizing mechanics — see RiskCfgFlagRegistry.hpp
  //   kill_switch_enabled              → MASK_RISK_CFG_KILL_SWITCH_ENABLED
  //   vol_sizing_enabled               → MASK_RISK_CFG_VOL_SIZING_ENABLED
  //   ws_dead_time_flatten_enabled     → MASK_RISK_CFG_WS_DEAD_TIME_FLATTEN_ENABLED
  //
  // OPS (v5.14.9.F.3): operational mechanics — see OpsCfgFlagRegistry.hpp
  //   session_filter_enabled           → MASK_OPS_CFG_SESSION_FILTER_ENABLED
  //   notify_enabled                   → MASK_OPS_CFG_NOTIFY_ENABLED
  alignas(8) uint8_t  lifecycle_cfg_flags;
              uint8_t  gate_cfg_flags;
              uint16_t ml_cfg_flags;
              uint8_t  risk_cfg_flags;
              uint8_t  ops_cfg_flags;
  FPN<F>
      partial_exit_pct; // fraction to exit at TP1 (0.5 = 50%, rest rides TP2)
  FPN<F> tp2_mult; // TP2 = TP1_distance * this (2.0 = double the TP distance)
  FPN<F> breakeven_buffer_pct; // SL offset from entry once breakeven ratchet
                               // fires (0.001 = +0.1% above entry, -0.001 =
                               // allow 0.1% loss)
  // slippage simulation
  FPN<F> slippage_pct; // simulated slippage on entry/exit (e.g. 0.0005 = 0.05%)
  // session awareness — session_filter_enabled migrated to ops_cfg_flags
  // (v5.14.9.F.3). The 4 per-session gate multipliers are registry-driven
  // as of v5.15.5.B.5; see FOREACH_SESSION_PHASE in SessionPhaseRegistry.hpp.
  // Adding a 5th session = ONE row in the registry; field decl + parser +
  // default + consumer lookup auto-flow via X-macro expansion.
#define X(NAME_U, name_l, START, END, MULT, DOC) FPN<F> session_##name_l##_mult;
  FOREACH_SESSION_PHASE(X)
#undef X
  // order book (L2 depth) — depth_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
  FPN<F> min_book_imbalance; // require bid bias to buy (0 = disabled, 0.10 =
                             // 10% bid excess)
  // EMA gate (proactive entry — reacts in 1-2s instead of 5s)
  // gate_ema_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
  FPN<F> gate_ema_alpha; // EMA smoothing factor (0.997 = ~333 tick window)
  FPN<F> gate_ema_one_minus_alpha; // precomputed 1.0 - alpha (avoid subtraction
                                   // on hot path)
  // strategy selection
  int default_strategy; // -1=regime auto, 0=MR, 1=Momentum, 2=SimpleDip
  // live trading
  int use_real_money; // 0=paper (default), 1=real orders via REST API
  // v5.7.2: explicit acknowledgment that the operator wants to run a
  // hardcoded (non-AUTO) strategy in live mode. Default 0 — the boot
  // path refuses to start with use_real_money=1 AND any
  // core_N_strategy != auto unless this flag is set. AUTO (regime-
  // gated) is preferred for live capital because hardcoded strategies
  // fire regardless of regime. Setting this to 1 is the operator
  // saying "I know what I'm doing" and is logged.
  // v5.11.3 — mlockall failure handling. 1 (default) = HFT-correct: fatal
  // exit if pages can't be locked into RAM (deployment must have enough
  // RLIMIT_MEMLOCK + CAP_IPC_LOCK). 0 = laptop / dev: warn and continue
  // when mlockall fails. Setting this to 0 in production trades determinism
  // for portability — operator should explicitly opt out.
  // v5.11.22 — InitArena MAP_HUGETLB opt-in. Default 0 = use 4 KB pages
  // (no OS dependency); set to 1 to request 2 MB hugepages for the 8 MB
  // boot arena, reducing TLB pressure on slow-path CoreSlowState +
  // strategy state access.
  //
  // Requires OS-level hugepage reservation:
  //   sudo sysctl -w vm.nr_hugepages=4   # 4 × 2 MB = 8 MB to fit arena
  // OR persistent in /etc/sysctl.d/99-foxml.conf:
  //   vm.nr_hugepages=4
  //
  // On boot, if the cfg flag is 1 but hugepages are not available,
  // InitArena_Create retries WITHOUT MAP_HUGETLB and emits a stderr
  // WARN — engine continues with normal pages, no fatal. See
  // DOCS/OPERATOR_DEPLOYMENT.md for the production-machine recipe.
  // kill switch (sticky — stays active until session reset or manual TUI 'k')
  // kill_switch_enabled migrated to risk_cfg_flags (v5.14.9.F.3)
  FPN<F>
      kill_switch_daily_loss_pct; // max daily loss before kill (e.g. 0.03 = 3%)
  FPN<F> kill_switch_drawdown_pct; // max drawdown from session peak before kill
                                   // (e.g. 0.05 = 5%)
  uint32_t kill_recovery_warmup; // slow-path cycles to observe after kill reset
                                 // before trading
  // v5.12.1.A — WS dead-time emergency-flatten policy (live-only).
  // Slow-path reads producer's last_ws_tick_us (set in EngineSharded fan_out),
  // computes (local_now_us - last_ws_tick_us). When the gap exceeds
  // ws_dead_time_flatten_threshold_secs AND ws_dead_time_flatten_enabled=1,
  // CAS-wins one slow-path thread invokes EventLoop_FlattenAll to emergency-
  // close all open positions via the standard drainer queue.
  // Disabled by default (0); flip to 1 BEFORE live-capital deployment.
  // Pre-warmup (last_ws_tick_us == 0) is always treated as "no flatten".
  // Backtest: cfg.ws_dead_time_flatten_enabled MUST stay 0; backtest's
  // tick-driven last_ws_tick_us would otherwise produce a huge gap vs
  // local clock and fire a phantom flatten on every cycle.
  // ws_dead_time_flatten_enabled migrated to risk_cfg_flags (v5.14.9.F.3)
  // v5.12.1.A.3 — recovery refusal window in seconds after a flatten
  // fires. Strategy_BuildParameters' caller (RebuildOneCore) sets
  // BUY_BLOCKED + SHALT_RECOVERY while now_us < oms.recovery_until_us.
  // Auto-clears when now_us crosses the deadline, on the next slow-path
  // cycle to detect the expiry.
  // v5.12.1.B.3 — hot-path parameter freshness gate. Slow-path stalls
  // (GC pause, OS scheduler hiccup, blocking I/O on health log) leave the
  // hot path executing on stale GateParameters. ConfidenceScorer freshness
  // damping is a soft signal; this is the HARD gate. Hot-path checks
  // (tick.sequence - publish_tick) > param_max_age_ticks → BUY_BLOCKED
  // via branchless mask. SHALT_PARAM_STALE on strategy_halt_reason.
  // Disabled by default; flip to 1 BEFORE live deployment after
  // measuring slow-path p99 latency on operator hardware.
  // param_staleness_gate_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
  // v5.14.8.E — stale-MODEL age check (load-time gate). Operator policy:
  // if a loaded model's stamp body claims training_timestamp_us older
  // than max_age_hours, engine WARNs (held_out_gate_strict=0) or
  // REFUSEs load (held_out_gate_strict=1). Default 0 = disabled.
  // Distinct from param_staleness_gate (slow-path-tier; this is a
  // boot-time gate). Per-model evaluation in CoreModelZoo_CheckStaleModel
  // (uses ModelHandle.training_timestamp_us — stamp-bound field added
  // in v5.14.8.D via FOREACH_STAMP_BOUND_MODEL_CONST registry).
  uint32_t model_max_age_hours;
  // v5.12.1.D — confidence-conditional sizing INFRASTRUCTURE (DEPRECATED v5.14.9.A).
  //   0 = disabled (default; flat risk_pct regardless of prediction P)
  //   1 = linear scale (factor = clamp((P - threshold) / (1 - threshold), 0, 1))
  //   2 = quadratic scale (factor squared — steeper rolloff)
  //
  // **DEPRECATED:** the math here was broken-for-composite-scale (compares
  // conf_now ∈ [0.001, 0.3] against ml_buy_threshold ∈ [0.5, 0.7] → factor=0
  // always when composite enabled). Replaced by v5.14.9.A FOREACH_DEGRADATION_CURVE
  // registry + curve dispatch. Field kept for back-compat until v5.14.9.B
  // replaces the caller at StrategyParameters.hpp:1291-1322. After .B ships,
  // this field is unused; back-compat parser shim translates legacy cfg
  // `risk_scale_by_confidence=N` → `risk_degradation_curve=N` (boot WARN).
  // Will be deleted in a future cleanup ship.
  int risk_scale_by_confidence;
  // v5.14.9.A — soft risk degradation ladder (replaces broken v5.12.1.D math).
  // Enum values come from FOREACH_DEGRADATION_CURVE in ML_Headers/ConfidenceScore.hpp:
  //   0 = OFF    (default; factor=1.0; preserves pre-v5.14.9 behavior bytewise)
  //   1 = LINEAR (linear interp between thresholds)
  //   2 = EXP    (quadratic falloff; preserves more size in middle)
  //   3 = STEP   (binary above/below midpoint; debug shape)
  //
  // REQUIRES cfg.confidence_composite_enabled=1 (boot REFUSE otherwise per
  // v5.14.9.B — ladder thresholds tuned for composite scale, not legacy
  // 3-factor IC scale). Stamp-bound via FOREACH_STAMP_BOUND_CFG (v5.14.9.C);
  // drift detection fires at boot if cfg differs from training-time stamp.
  //
  // Per-core override: core_N_risk_degradation_curve (RUNTIME-ONLY; not
  // stamp-bound — operator policy, matches existing per-core risk_pct
  // precedent). See v5.14.9.B.1.
  int    risk_degradation_curve;             // default 0 (OFF)
  // v5.15.5.A.5 — per-horizon TP/SL barrier blend mode (5-mode enum).
  // 0=LEGACY (cfg.ml_tp_pct direct; pre-v5.15.5 behavior; DEFAULT for
  //   backward-compat — operator must opt in to non-legacy mode), 1=BLEND
  //   (Σ wᵢ · barrierᵢ weighted blend), 2=DOMINANT (argmax(weights) picks
  //   one arm's barriers; exact train-serve match per trade), 3=BOTH_BLEND_DRIVES
  //   (blend drives + dominant logged for shadow-mode A/B compare),
  //   4=BOTH_DOMINANT_DRIVES (dominant drives + blend logged for shadow-mode).
  // Per CLAUDE.md item 13 X-macro: enum + ToString/FromString + branchless
  // dispatch via MODE_FLAGS[] table auto-generated from FOREACH_BARRIER_BLEND_MODE
  // (see ML_Headers/BarrierBlendModeRegistry.hpp).
  // Per-core override: core_N_barrier_blend_mode (INT-enum, mirrors
  // risk_degradation_curve v5.14.9.C precedent).
  int    barrier_blend_mode;                  // default 0 (LEGACY)
  // Threshold above which factor=1.0 (full size at high confidence).
  // Default 0.15 matches composite confidence's practical upper bound.
  FPN<F> risk_full_size_threshold;           // default 0.15
  // Threshold below which factor=min_pct (or 0 if min_pct=0 = ladder bottom).
  // Default 0.05 matches composite confidence's "low edge" boundary.
  FPN<F> risk_min_size_threshold;            // default 0.05
  // Factor at min threshold. ∈ [0, 1]. Below this size, ladder bottom fires
  // (factor=0 → trade_size=0 → BUY_BLOCKED + SHALT_LOW_CONFIDENCE).
  // Default 0.10 = 10% of base size at low edge; operator sets 0.0 for hard
  // ladder bottom even at min_threshold (matches confidence_hard_block
  // behavior but with a smooth ramp above).
  FPN<F> risk_min_size_pct;                  // default 0.10
  // v5.14.1 — composite confidence (IC × Freshness × Capacity × Stability)
  // Default 0 = legacy 3-factor ConfidenceScorer_Compute (bytewise-unchanged
  // pre-v5.14.1 behavior). Flip to 1 to swap in the 4-factor formula at the
  // risk_scale_by_confidence sizing site (StrategyParameters.hpp:1099).
  // Pairs with: confidence_freshness_tau_secs (decay constant),
  // confidence_capacity_target_dollars (0=unbounded), confidence_rmse_baseline
  // (training-time RMSE for stability normalization).
  // confidence_composite_enabled migrated to ml_cfg_flags (v5.14.9.F.2; stamp-bound via FOREACH_STAMP_BOUND_CFG emit_source=BITMAP_BIT)
  FPN<F>   confidence_freshness_tau_secs;       // default 3600.0 (1 hour decay)
  FPN<F>   confidence_capacity_target_dollars;  // default 0.0 (unbounded)
  FPN<F>   confidence_capacity_kappa;           // default 0.1 (ADV proportionality)
  FPN<F>   confidence_rmse_baseline;            // default 1.0 (rebound at training time)
  // v5.14.1.D — feature winsorization (per-feature percentile clipping
  // applied in FeatureStandardizer_Apply BEFORE mean-center + unit-var).
  // Reduces noise from 5σ outliers (flash crashes, exchange glitches) +
  // improves model generalization vs fat-tailed crypto returns. Stamp-
  // bound via FOREACH_STAMP_BOUND_CFG → drift between training cfg and
  // inference cfg detected at model load.
  // Setting low=0 + high=1 disables (no clip; identity pass-through).
  FPN<F>   winsor_pct_low;                      // default 0.005 (lower clip pct)
  FPN<F>   winsor_pct_high;                     // default 0.995 (upper clip pct)
  // v5.12.2.B — lazy slow-path rebuild. Skip RebuildOneCore body when
  // slow_state hasn't changed materially since last rebuild. Estimated
  // 30-50% of cycles become no-ops on stable regimes; per-cycle savings
  // ~30-50us (bypassed regime-classify + strategy-build work).
  // Default 0 = always rebuild (preserves bytewise replay-determinism
  // baseline). Flip to 1 after parity-check confirms regime histogram
  // unchanged within tolerance.
  // force_period_us bounds worst-case missed regime shift (1s = 100
  // poll_interval cycles at default poll_interval=100). price_threshold_pct
  // is the per-tick price-delta threshold below which the slow-path cycle
  // is considered "no material change."
  // lazy_rebuild_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
  FPN<F>   lazy_rebuild_price_threshold_pct; // default 0.0005 (0.05%)
  // v5.12.2.D — Treelite AOT inference backend (INFRASTRUCTURE ONLY in
  // this ship; Treelite vendoring + Predict_AOT impl deferred to follow-
  // up). When 1 + stamp body has has_aot_compiled_sha256=1 + the .so
  // file at aot_compiled_path verifies SHA-256 → engine loads compiled
  // .so via Model_LoadAOT, calls Predict_AOT (~<100ns vs ~1-5us C API).
  // When 0 (default) OR AOT load fails OR sha mismatch → engine falls
  // back to XGBoost C API path silently. Per-stamp opt-in via the
  // stamp body fields, not just the cfg flag.
  // v5.13.0 — sell-side ML predictions (Path 3: path-based role
  // discrimination using existing PEAK_VALLEY_STABLE 3-class labels).
  // When enabled + ezoo->exit_predictor_count > 0, MLStrategy runs
  // exit prediction at slow-path; if blended exit_prob > exit_threshold
  // AND position open for this core's slot, fires early market-exit
  // via OMS_PushSubmit. Default disabled; opt-in for paper-test.
  // Hot path UNTOUCHED.
  // use_exit_model migrated to ml_cfg_flags (v5.14.9.F.2)
  FPN<F> exit_threshold;                     // default 0.6 (60% blended exit prob)
  char exit_signal_model_dir[256];           // optional explicit dir; empty = auto-detect
  // v5.13.0.B — calibration log: every exit fill records the predicted
  // exit prob + realized PnL bps + flag indicating whether v5.13.0 exit-
  // model fired. Operator post-processes the CSV to assess prediction
  // calibration (Brier score, AUC, etc.). Default empty = disabled.
  char calibration_log_path[256];

  // v5.14.0 — Ridge risk-parity blending. Markowitz-style cost-aware
  // weight combination of N model predictions via Cholesky solve of
  // (Σ + λI) w = μ. Complements (does NOT replace) Exp3-IX bandit:
  // bandit selects ONE arm per regime; Ridge blends correlated models
  // intelligently (penalizes double-counting of correlated alpha).
  // Default off; opt-in for paper-test session. When enabled, slow-
  // path adds ~3µs/cycle (BuildCorr ~1µs + Cholesky ~2µs at N=8).
  // Hot path UNTOUCHED.
  // ridge_within_horizon migrated to ml_cfg_flags (v5.14.11.C; bit MASK_ML_CFG_RIDGE_WITHIN_HORIZON)
  // ridge_across_horizons migrated to ml_cfg_flags (v5.14.11.C; bit MASK_ML_CFG_RIDGE_ACROSS_HORIZONS)
  FPN<F> ridge_lambda;               // ridge regularization; default 0.15
  FPN<F> ridge_cost_penalty;         // cost penalty in net IC = IC - penalty*cost; default 0.5
  FPN<F> ridge_min_ic_floor;         // min net IC floor (prevents zero-weight starvation); default 0.001
  // vol-scaled position sizing
  // vol_sizing_enabled migrated to risk_cfg_flags (v5.14.9.F.3)
  FPN<F> vol_scale_min; // min scale factor (e.g. 0.25 = never less than 25% of
                        // base qty)
  FPN<F> vol_scale_max; // max scale factor (e.g. 2.0 = never more than 200% of
                        // base qty)
  // no-trade band (cost-aware signal strength gate)
  // no_trade_band_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
  FPN<F> no_trade_band_mult; // signal must exceed fee_rate * this to trade
                             // (e.g. 3.0)
  // ML inference
  char ml_model_path[256];     // path to buy-signal model file
  FPN<F> ml_buy_threshold;     // prediction > this = buy signal (e.g. 0.6)
  FPN<F> ml_tp_pct;            // TP % for ML positions (e.g. 0.015 = 1.5%)
  FPN<F> ml_sl_pct;            // SL % for ML positions (e.g. 0.008 = 0.8%)
  char regime_model_path[256]; // path to regime enrichment model
  FPN<F> regime_model_weight;  // score weight in Regime_Classify (e.g. 2)
  // danger gradient (hot-path crash protection)
  FPN<F> danger_warn_stddevs;  // gradient starts at this many stddevs below avg
                               // (e.g. 3.0)
  FPN<F> danger_crash_stddevs; // full gate kill at this many stddevs below avg
                               // (e.g. 6.0)
  // tick recording (writes raw ticks to CSV for backtesting/ML training)
                    // data/{symbol}/YYYY-MM-DD.csv
  // depth recording (Phase 8a c5): writes @depth5@100ms snapshots to
  // data/{symbol}/depth/YYYY-MM-DD.csv. Requires depth_enabled=1 (recorder
  // is fed by depth_thread_fn). Off by default — opt-in for replay/audit.

  // operational alerts (Phase 8b): route kill switch, orphan, disconnect
  // events through a configurable backend. All off by default.
  // notify_enabled migrated to ops_cfg_flags (v5.14.9.F.3)
  char notify_command[512];       // shell template with up to 2 %s (subject, body)
                                  // examples in engine.cfg / SettingsPanel tooltip
  // FoxML integration — Phase 6C (all default OFF, zero behavior change when
  // disabled)
  // cost_gate_enabled migrated to gate_cfg_flags (v5.14.9.F.1) — original comment: 0=disabled, 1=estimate trade cost via CostModel,
                         // suppress if unprofitable
  // foxml_vol_scaling_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
                                  // inverse-vol on slow path
  FPN<F> foxml_vol_scaling_z_max; // z-score clipping threshold for VolScaler
                                  // (default 3.0)
  // bandit_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
                      // weights
  FPN<F> bandit_blend_ratio; // bandit influence fraction at full ramp (default
                             // 0.30)
  // confidence_enabled migrated to ml_cfg_flags (v5.14.9.F.2) — original comment: 0=disabled, 1=dynamic ml_buy_threshold from
                             // confidence scoring
  // Phase 6 prep — tunable confidence loop parameters. Defaults preserve the
  // pre-Phase-6prep hardcoded values. Only consulted when confidence_enabled=1.
  uint32_t confidence_window;       // RollingIC + RollingRMSE window (default 32)
  // v5.14.9.D — DELETED legacy `confidence_freshness_tau` field. Was
  // mathematically inert in production (data_age=0 → freshness=1.0
  // regardless of tau). Composite confidence (v5.14.1) uses
  // `confidence_freshness_tau_secs` for its own freshness math; legacy
  // 3-factor formula now uses CONFIDENCE_FRESHNESS_TAU_DEFAULT (300.0)
  // hardcoded constant. Closes TECH_DEBT-004.
  FPN<F>   confidence_threshold_scale; // gate formula: effective_thr = base * (this - conf)
                                       // (default 2.0 — clamps at 1.0 in code)
  // v5.9.1 (V5_9_AUDIT-#21) — hard-block entries when raw confidence is below
  // this threshold, INDEPENDENT of damping. With low IC (noisy predictions),
  // damped threshold can become pathological; trades fire on essentially-
  // random predictions. Hard floor is the safety net.
  // Default 0.0 = disabled (preserves pre-v5.9.1 behavior). Operator opts in
  // (audit-recommended value 0.05). Only consulted when confidence_enabled=1.
  // Surfaced via SHALT_LOW_CONFIDENCE on the strategy_halt_reason channel.
  FPN<F>   confidence_hard_block_threshold;
  // Phase 7 prep — held-out validation infrastructure. Used by foxml_suite
  // when training/evaluating a model. Live engine reads via expected.cfg
  // mismatch checks (CoreModelZoo).
  // v5.15.5.F.4d.1.B.3 Phase F HIGH-1 (b) — manual `FPN<F> held_out_fraction;` decl REMOVED;
  // auto-gen via FOREACH_GLOBAL_CFG_FIELD(EMIT_GLOBAL_CFG_STRUCT_FIELD) at line 1338 (Path α)
  // covers the field declaration from the new registry row at CfgFieldRegistry.hpp Validation.
  // v5.2.0 (held-out gate Phase 1) — model attestation infrastructure.
  // Each .bin model can have a paired .stamp file with hash+signature
  // attesting that held-out validation passed. When held_out_gate_strict=1,
  // CoreModelZoo_LoadFromDir refuses to load .bin files without a valid
  // stamp. Defaults: strict=0 (gate disabled — existing models still load),
  // secret="" (which makes verify_model_stamp accept any stamp signature
  // — useful for dev). Set both for production live deploy.
  // v5.9.2b — allow loading a model whose stamp's engine_version differs
  // by major version from current build (e.g. stamp says 5.x but engine
  // is 6.x). Cross-major can introduce hot-path predicate changes that
  // make a v5-trained model misbehave at v6 inference. Default 0 (refuse
  // cross-major in both strict + non-strict modes); operator opts in
  // explicitly with allow_cross_major_engine=1 + held_out_gate_strict=0.
  // Within-major loads (5.7 → 5.9) are always permitted.
  char   held_out_stamp_secret[128]; // HMAC-SHA256 secret for stamp signing/verify
  // v5.3.2 — auto-stamp on held-out completion. When 1, Backtest_RunFullValidation
  // calls stamp_write_for_model after a successful held-out training pass, using
  // held_out_stamp_secret + gap_acceptable_threshold above. Default 1 since v5.8.10
  // (suite Run Full Validation auto-stamps). v5.15.5.F.4d.1.B.3 Path C 2026-05-24:
  // manual tools/stamp_model.sh DELETED; the =0 alternative is now only meaningful
  // for v5.16+ cmdline-invocable training per decoupling-endgoal-roadmap.
  // v5.11.47 — auto-stamp HMAC secret. When non-empty AND
  // auto_stamp_on_held_out=1, the suite signs each generated stamp
  // with this secret. Empty = devmode (signature accepted as-is at
  // load time). Operator can also type a secret in the GUI Validation
  // panel; the GUI value takes priority over this cfg fallback.
  // Setting here means operator doesn't have to re-type per-session.
  char   auto_stamp_secret[128];
  // v5.4.0 (Phase 0.1) — operational health log. Always-available
  // structured JSONL diagnostic log. Replaces ad-hoc env-gated stderr
  // traces. When enabled, engine init configures MemHeaders/HealthLog.hpp
  // with these values; subsequent Health_Log() calls append to the path.
  //   health_log_path: "" (default) = disabled; non-empty = JSONL output path
  //   health_log_level: 0 = info (always), 1 = debug, 2 = trace
  char   health_log_path[256];
  // v5.9.4 — health log rotation (in-process, atomic rename pattern).
  // Without rotation, long soaks (30+ days) accumulate >5GB of JSONL,
  // operator-unfriendly. Rotates via rename to <path>.<unix_ts>,
  // keeps last `keep_count` rotated files, deletes oldest.
  //   health_log_max_bytes: 0 = no rotation (default for back-compat);
  //                         >0 = rotate when current file exceeds size
  //   health_log_keep_count: 0 = keep nothing (default; only the
  //                              active file lives); N = keep last N
  //                              rotated files (typical: 7)
  // v5.9.4 — acknowledge_cross_binary_version_drift MIGRATED to ops_cfg_flags
  // bitmap (MASK_OPS_CFG_ACKNOWLEDGE_CROSS_BINARY_DRIFT) at v5.15.5.A.7. Read
  // sites use BITMAP_IS_SET; legacy cfg key `acknowledge_cross_binary_version_drift=1`
  // still parses via FOREACH_OPS_CFG_FLAG walker (legacy_field column).
  // v5.9.5i — acknowledge_inference_cfg_drift MIGRATED to ops_cfg_flags bitmap
  // (MASK_OPS_CFG_ACKNOWLEDGE_INFERENCE_CFG_DRIFT) at v5.15.5.A.7. Read sites
  // use BITMAP_IS_SET; legacy cfg key still parses via FOREACH_OPS_CFG_FLAG walker.
  // v5.10.0c — hot model swap behavior when a position is open.
  //   0 (default) = swap is deferred until the position closes naturally
  //                 (next slow-path retries; safer — entry & exit use
  //                 the same model);
  //   1           = swap proceeds immediately, open positions exit on
  //                 whatever model fires next (operator opt-in for
  //                 emergency model retraction).
  // Setting this flag = 1 acknowledges that a single position may have
  // its entry and exit driven by different models, which can produce
  // unintuitive realized P&L. Default 0 keeps the entry-exit symmetry.
  // v5.10.0e — runtime IC drift detection. Sampled post-fill on ML cores;
  // sustained breach (avg IC over `confidence_ic_floor_window` seconds
  // below `confidence_ic_floor`) emits CRITICAL log. When `auto_kill_on_drift=1`
  // also trips the per-core kill_switch — engine stops opening new
  // positions on that core; existing positions exit naturally.
  //
  // confidence_ic_floor       — min acceptable rolling IC. Default 0.02
  //                              (Spearman correlation; > random chance).
  // confidence_ic_floor_window — sustained-breach window in SECONDS.
  //                              Default 86400 (24 hours).
  // auto_kill_on_drift        — 0 (default) = log only; 1 = also trip
  //                              per-core kill_switch on first breach.
  double   confidence_ic_floor;
  uint32_t confidence_ic_floor_window;
  int      auto_kill_on_drift;
  // v5.2.1 (live reconciliation Phase 1) — exchange-truth sync at boot
  // (and optionally on heartbeat). LIVE-mode-only — paper mode skips
  // reconcile entirely.
  //   reconcile_interval_sec: 0 = boot-only (default, sufficient for
  //                              most cases), >0 = poll cadence in
  //                              seconds for heartbeat reconciliation
  //                              (defends against silent WS-missed fills).
  //   reconcile_dry_run:      DEPRECATED v5.14.4. Use reconcile_mode.
  //                              Back-compat parser still accepts this
  //                              field (dry_run=1 → mode=WARN; dry_run=0
  //                              → mode=STRICT). Will be removed in
  //                              v5.X+ once operator cfgs migrated.
  //   reconcile_mode:         v5.14.4 — 3-mode enum (STRICT/WARN/AUTO_SYNC).
  //                              See FOREACH_RECONCILE_MODE in Reconcile.hpp
  //                              for canonical values + cfg_string mappings.
  //                              Cfg parser accepts both string ("strict",
  //                              "warn", "auto_sync") and numeric (0/1/2)
  //                              for operator-friendly + back-compat reading.
  // Default reconcile_mode=WARN is intentional friction — flip to STRICT
  // for production refusal-on-mismatch, or AUTO_SYNC for replay+cancel.
  int    reconcile_dry_run;          // legacy; back-compat shim translates to reconcile_mode
                                      // (avoid pulling Reconcile.hpp into universal-include
                                      //  ControllerConfig.hpp; cast at point of use)
  // v5.15.2 — TradingMode discriminates paper vs live vs shadow operation.
  // Distinct from engine_mode (sharded vs single_core architectural).
  // Default PAPER preserves pre-v5.15 behavior; legacy cfgs unset →
  // PAPER → no behavior change.
  // Stamp-bound via FOREACH_STAMP_BOUND_CFG so every model carries its
  // training-time mode for audit trail. Read at boot for
  // LiveReadiness_Verify dispatch — REFUSE on live + missing pre-flight
  // items; WARN-only on paper/shadow. Values defined at file scope below
  // (TRADING_MODE_PAPER / LIVE / SHADOW; matches engine_mode constant
  // style for consistency).
  // v5.15.4 — bitmap of "operator explicitly set this key" flags. Used
  // by ControllerConfig_NormalizeForMode<F> to honor operator overrides
  // when applying mode-specific defaults. Adding a new tracked key = 1
  // bit position + 1 parser-side `cfg.cfg_keys_explicit |= MASK_X` set.
  //
  // Cohort verdict (CLAUDE.local.md cohort-audit rule 2026-05-11):
  // 2 bits today (MODEL_VERIFY_STRICT + RECONCILE_MODE); expected to grow
  // as more mode-specific flip rules land. Per CLAUDE.md item 20
  // (bitmap-flag-api) + cfg-flag-eligibility-criteria.md cohort
  // discipline, bit-pack from the start rather than retrofit later.
  uint16_t cfg_keys_explicit;
  // Prediction normalization — Phase 7F (default OFF)
  int prediction_normalize; // 0=disabled, 1=z-score normalize predictions
                            // (activates after 100)
  // Barrier gate — Phase 7E (default OFF)
  // barrier_gate_enabled migrated to gate_cfg_flags (v5.14.9.F.1; stamp-bound via FOREACH_STAMP_BOUND_MODEL_CONST) — original comment: 0=disabled, 1=block entries before predicted
                               // price peaks
  char peak_model_path[256];   // path to P(will_peak) model
  char valley_model_path[256]; // path to P(will_valley) model
  // Phase 5c stupid-proofing: when a model is loaded from a run bundle
  // (core_N_model_dir), the engine reads models/{dir}/expected.cfg and
  // compares ML-relevant fields against the live config. mismatches are
  // a) warnings (default), b) load failures (strict=1), or c) ignored (=-1).
  // strict mode is recommended for production deployment; default mode
  // for development so a single missing expected.cfg doesn't break startup.
  // Per-core sharding (Phase 13) — STARTUP-ONLY, ignored by hot reload
  // v5.0.2: slow-path CPU pin policy. STARTUP-ONLY.
  //   < 0  → do not pin slow-paths (OS-scheduled — original v5.0 behavior)
  //   == 0 → auto: pin slow-path c to (drainer_cpu + 1 + c) mod nproc.
  //          Drainer pins to (num_cores + 1) so default base is num_cores + 2.
  //          Wraps via modulo if base + num_cores > nproc.
  //   > 0  → explicit base: pin slow-path c to (offset + c) mod nproc.
  // Default 0 (auto). Slow-paths are jitter-tolerant so HT-sharing with
  // spare cores is acceptable. Set to -1 to disable (e.g. for benchmarks
  // comparing pinned vs unpinned).
                                // mode (default 4, cap 16)
  // Phase 14: when 1, sharded mode forces the synthetic tick generator
  // (sawtooth around $60k) instead of connecting to Binance. Useful for
  // latency demos that need reliable trade firing without depending on
  // current market volatility. Default 0 = use real Binance feed.
  // Per-core strategy assignment for sharded mode. core_strategies[i] is the
  // STRATEGY_* constant for execution core i. Default: all STRATEGY_SIMPLE_DIP.
  // Config syntax: core_0_strategy=simple_dip, core_1_strategy=ema_cross, etc.
  // Accepted names: mr, momentum, simple_dip, ml, ema_cross, none.
  // core_strategies: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // v5.9.0c — explicit-set bitmap for core_strategies. Bit i set = `core_i_strategy=`
  // appeared in the cfg file (operator deliberate choice). Bit i clear = field
  // absent in cfg, default applied. Surfaces "default vs deliberate" tri-state
  // in TUI per V5_9_AUDIT-#5. The today-bug class: backtest.cfg lacked
  // per-core strategy lines → all cores defaulted to SIMPLE_DIP → operator
  // saw "0!" hardcoded warnings, couldn't tell defaulted from deliberate.
  uint16_t core_strategies_explicit_set;
  // v5.9.0c — captured cfg file path. ControllerConfig_Load stores the path
  // it parsed; the engine header panel displays this so operators see at
  // boot which cfg drove the configuration. Distinct binaries (engine_gui
  // reads engine.cfg, foxml_suite reads backtest.cfg) → "loaded cfg path"
  // makes the difference visible.
  char source_cfg_path[256];
  // Per-core risk allocation. core_risk_pct[i] is the fraction of total
  // balance this core can risk on a single trade. Default 0 = use the
  // shared risk_pct / num_cores. Non-zero = use this specific percentage.
  // Config syntax: core_0_risk_pct=20.0 (stored as 0.20).
  // core_risk_pct: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // Phase 3: per-core kill switch overrides + global tunables.
  // core_max_drawdown_pct[i] overrides the shared max_drawdown_pct for
  // this specific core. Default 0 = use shared. Config syntax:
  // core_0_max_drawdown_pct=15.0 (stored as 0.15).
  // core_max_drawdown_pct: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // min_kill_loss: absolute USDT floor for the per-core kill switch. The
  // trip fires only when BOTH dd_pct exceeds threshold AND drop exceeds
  // this floor. Without it, a tiny allocation ($10) loses $0.50, dd=5%,
  // and the kill trips on rounding noise. Default $5. Config syntax:
  // min_kill_loss=5.0
  FPN<F> min_kill_loss;
  // enable_mtm_kill_switch: 1 = include unrealized P&L in kill eval (mark
  // to market every slow path); 0 = realized-only (legacy behavior). MTM
  // catches "position riding down with no SL hit yet" scenarios. Default 1.
  uint32_t enable_mtm_kill_switch;
  // Per-core ML model path. Each core running STRATEGY_ML can load its
  // own model. Default empty = use shared ml_model_path. Config syntax:
  // core_0_model_path=models/aggressive.xgb
  // core_model_path: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // Per-core ML model directory. When set, the engine auto-discovers
  // role-specific models in this directory (barrier.json/.xgb,
  // buy_signal.json/.xgb, regime.json/.xgb, exit.json/.xgb) and loads
  // them into a CoreModelZoo. Missing files = role disabled.
  // When BOTH model_dir and model_path are set, model_dir wins (zoo
  // supersedes legacy single-model). Config syntax:
  // core_0_model_dir=models/aggressive/
  // core_model_dir: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // v5.10.0a.G.6 — per-core multi-horizon ensemble cfg (string-typed, can't
  // fit X-macro pattern). Default empty = inherit from global cfg.horizon_list /
  // cfg.ensemble_blend_mode. Auto-detect (G.5) takes priority over both;
  // these are overrides for operators who want explicit per-core control
  // (e.g., core 0 deploys 5-horizon ensemble while core 1 stays single-model).
  //
  //   core_0_horizon_list=100,500,1000        # CSV; per-core horizon set
  //   core_0_ensemble_blend_mode=weighted     # selection | weighted (default)
  //   core_0_disabled_horizons=100            # CSV; kill-switch per horizon
  //                                           # (skips predict; bandit weight frozen)
  // core_horizon_list: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // core_ensemble_blend_mode: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // core_disabled_horizons: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // v5.11.18a — per-core feature mask. Bit i set = feature index i is
  // computed + packed for this core; bit i clear = packed as 0.0f
  // (NOT skipped; sparse-zero array contract per parity-check finding).
  // Default 0xFFFFFFFFFFFFFFFF (all features enabled) — preserves
  // pre-v5.11.18a behavior bytewise.
  //
  // Config syntax: `core_0_feature_mask=0xFFFFFFFFFFFFFFFF` (matches the
  // existing per-core field convention: core_N_strategy, core_N_risk_pct,
  // etc.). Parser accepts 0x-prefixed hex (any case) or plain decimal.
  // Stored uint64_t.
  // Parity binding (Surface G stamp body extension at v5.11.18a, ML
  // wiring at v5.11.18) ensures runtime-mask vs training-mask drift is
  // caught at model load.
  //
  // Safety: v5.11.18a ships this cfg field + stamp infra ONLY. Behavior
  // change (Features_PackAll respecting the mask) lands in v5.11.18.
  // Default-on bitmap means any cfg without `feature_mask_<N>=` lines
  // produces identical features to pre-v5.11.18a builds.
  // core_feature_mask: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // Per-core full-tunable overrides (v4.0). One slot per execution core
  // (16 max). Each PerCoreOverrides field shadows a same-named field on
  // ControllerConfig — non-zero overrides global; zero inherits.
  // Resolved on every slow-path rebuild via
  // ControllerConfig_ResolveForCore. See PerCoreOverrides comment block.
  // core_overrides: declared via FOREACH_MANUAL_PER_CORE_FIELD X-macro (see ControllerConfig<F> struct end + DOCS/MANUAL_FIELDS_INVENTORY.md Section A)
  // Per-strategy TP/SL overrides. Default 0 = fall back to the shared
  // take_profit_pct / stop_loss_pct. Non-zero = use this instead.
  // Momentum already has momentum_tp_mult / momentum_sl_mult (stddev mults).
  FPN<F> simpledip_tp_pct;    // SimpleDip TP override (%, stored as decimal)
  FPN<F> simpledip_sl_pct;    // SimpleDip SL override
  FPN<F> mr_tp_pct;           // MeanReversion TP override
  FPN<F> mr_sl_pct;           // MeanReversion SL override
  FPN<F> emacross_tp_pct;     // EMA Cross TP override
  FPN<F> emacross_sl_pct;     // EMA Cross SL override
  // OMS phase 03: which path EventLoop_OnEvent takes when a TradeEvent
  // arrives. mode 0 (legacy): OnEvent mutates the portfolio + balance
  // directly, same as phase 02. mode 1 (event log): OnEvent just bumps
  // total_events_processed and routes to OMS, the OMS callback does the
  // portfolio mutation + balance update + kill switch peak + trade log
  // write. mode 1 is what the head-to-head test exercises and what
  // production runs after the soak. default 0 so existing tests stay
  // green during the migration window.
  uint32_t oms_event_log_mode; // 0 = legacy (default), 1 = event log

  // v5.15.5.C.3 Phase 7.A — Runtime bench gate substrate flag.
  //
  // 0 = production (default; ZERO bench instrumentation cost — compile-time
  //     elision via template <bool BENCH=false> in Phase 7.B follow-up).
  // 1 = bench mode (instrumented slow-path sites emit per-cycle rdtsc
  //     measurements into LatencyHistogram clusters; surface via TUI
  //     p50/p99/max readout per snapshot publish).
  //
  // Phase 7.A scope: cfg flag substrate + LatencyHistogram primitive +
  // tests. Phase 7.B integration (template-dispatch wrappers + N
  // instrumented sites + TUI surface) deferred to focused follow-up ship.
  // See MemHeaders/LatencyHistogram.hpp + DESIGN_SPECS/runtime-toggleable-
  // bench-gate-pattern.md for full design.
  //
  // Today (Phase 7.A only): flipping this flag has NO observable effect —
  // bench gate dispatch is not yet wired. Reserved for the follow-up ship.
  uint32_t oms_bench_enabled;  // 0 = production (default); 1 = bench mode (Phase 7.B integration pending)

  // v5.9.5h — XGBoost training hyperparams (cfg-tunable subset).
  // max_depth/learning_rate/n_estimators are operator-tunable via Train
  // Model GUI panel only (NOT cfg-bound — they're per-experiment, not
  // per-deploy). The 5 fields below were hardcoded pre-v5.9.5h; now
  // operator-tunable via cfg. Defaults match pre-v5.9.5h hardcoded
  // values bytewise. Stamp body records what trained the model
  // (Surface G `has_xgb_hyperparams` flag); engine load-WARN fires
  // when stamp's value differs from cfg's at boot.
  FPN<F>   xgb_subsample;          // row subsample per tree (0.5-1.0); default 0.8
  FPN<F>   xgb_colsample_bytree;   // column subsample per tree (0.5-1.0); default 0.8
  char     xgb_tree_method[16];    // hist | exact | approx | auto; default "hist"

  // v5.10.0 Item D — hardware-aware cfg. Operator-tunable thread counts
  // and RAM budgets; defaults match v5.9.5j-final behavior bytewise so
  // upgrades don't silently flip defaults. Operator opts in to multi-thread
  // training / parallel CSV / larger budgets.
  //
  // Thread counts (default=1 matches current hardcoded behavior at
  // BacktestEngine.hpp:1352, 1638). Setting >1 breaks bytewise reproducibility;
  // boot-time WARN fires when operator sets >1 to make the tradeoff explicit.
  // v5.11.41 — Multi-Horizon parallelism cap. Worker spawns
  //   min(N_horizons, multi_horizon_max_threads) pthreads, each running a
  //   full per-horizon Backtest_RunFullValidation pipeline. 0 = auto
  //   (defaults to min(8, ncores/2) computed at runtime to leave room
  //   for GUI/other threads). 1 = forced serial (legacy behavior). >1
  //   pins xgb_train_nthread=1 inside parallel worker for bytewise
  //   determinism vs serial-mode-with-nthread=1. Recorded in stamp body
  //   via xgb_train_nthread field for forensic mode-divergence detection.

  // RAM budgets (advisory soft caps — emit WARN at boot if dataset projects
  // to exceed; no hard refuse since the streaming label compute closes
  // OOM regardless). Operator hint when sizing the box.

  // v5.10.0a Item #4 — multi-horizon training. Comma-separated list of
  // forward-tick horizons; Train Model worker iterates and trains one
  // model per horizon. Empty (default) = single-horizon (uses
  // label_forward_ticks from per-run state). Cfg field rather than
  // RunConfig because operator typically standardizes horizons across
  // training experiments.
  // Example: horizon_list=100,500,1000,5000 → 4 trainings, saved as
  // <model_dir>/horizon_100/<role>.json etc.
  // LITE in v5.10.0a: trains + saves N models; operator manually picks
  // one to deploy. Ensemble inference (load all N at runtime, blend
  // predictions) deferred to v5.10.0a.x — needs stamp body extension +
  // multi-model load in CoreModelZoo, both genuinely complex.
  static constexpr int HORIZON_LIST_MAX = 8;
  int      horizon_list[HORIZON_LIST_MAX];  // 0 = unused slot
  int      horizon_count;                    // number of populated slots

  // v5.10.0a.G.6 — global ensemble cfg (per-core overrides via
  // core_N_ensemble_blend_mode / core_N_horizon_list / core_N_disabled_horizons).
  // Used when ensemble auto-detect (G.5) finds horizon siblings on disk
  // OR operator explicitly populates horizon_list.
  //
  //   ensemble_blend_mode    "weighted" (default) | "selection"
  //                          weighted = G.7 Bandit-Exp3 per-regime weighted blend
  //                          selection = G.4 argmax-confidence (single horizon per tick)
  //   ensemble_bandit_eta    Bandit learning rate (0.01 .. 1.0); higher = faster
  //                          adaptation but more variance. Default 0.1 conservative.
  //   ensemble_min_warmup_predictions  predictions per regime before weights trusted
  //                                    (uniform during warmup). Default 100.
  //   ensemble_min_agreement_pct       safety: ≥X fraction of non-disabled horizons
  //                                    must predict same direction OR skip entry.
  //                                    Default 0.6 (60% agreement). Set 0 to disable.
  char     ensemble_blend_mode[16];
  double   ensemble_bandit_eta;
  int      ensemble_min_warmup_predictions;
  double   ensemble_min_agreement_pct;
  // v5.13.4 — sell-side bandit. Disabled by default; opt-in for paper-
  // test. When enabled, exit-fired trades' counterfactual reward feeds
  // exit_bandit; non-exit fills bypass exit_bandit (existing buy_bandit
  // path unchanged). Reward formula: actual_pnl_bps - hypothetical_held-
  // to-TP_pnl_bps (optimistic; biases against exits — operator scales
  // via exit_bandit_lr until paper-test calibration suggests refinement).
  // exit_bandit_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
  double   exit_bandit_lr;             // bandit learning rate; default 0.1
  // v5.14.1.E — exit-side blender selector. Mirrors v5.14.0 buy-side
  // ridge_within_horizon. Enables Ridge blending across exit_predictor
  // handles (correlation-aware; downweights correlated alpha sources).
  // 0 (default) = bandit-only (pre-v5.14.1.E behavior; preserved
  // bytewise). 1 = Ridge across exit_predictor handles using existing
  // ridge_lambda + ridge_cost_penalty + ridge_min_ic_floor cfg.
  // Stamp-bound via FOREACH_STAMP_BOUND_CFG → drift detected at load.
  // exit_blender_mode migrated to ml_cfg_flags (v5.14.11.C; bit MASK_ML_CFG_EXIT_BLENDER_MODE)
  // v5.14.1.G — Portfolio turnover (operator diagnostic only; not
  // stamp-bound — tunable post-train without retraining).
  // window: rolling buffer size (≥ 2, ≤ 256)
  // topk: top-K arm members tracked per cycle (≥ 1, ≤ 8 = ENSEMBLE_HORIZON_MAX)
  // Surfaced in PerCoreSnap.ml_portfolio_turnover for operator visibility.
  int      confidence_turnover_window;  // default 100
  int      confidence_turnover_topk;    // default 3 (top-3 arms)
  // v5.14.1.F — IC variant selector (drift detection + TUI display).
  // 0 = Spearman (default; existing RollingIC implementation despite
  //     generic name — see ICVariantRegistry.hpp doc note).
  // 1+ = future variants (Pearson, Kendall, etc.; slot in via
  //      FOREACH_IC_VARIANT(X) in ICVariantRegistry.hpp).
  // Routes through ConfidenceScorer_ComputeICVariant dispatcher at:
  //   - ControllerEventLoop.hpp drift detection
  //   - ShardedSnapshot.hpp ml_confidence_ic display
  // Composite formula internals always use direct Spearman (no cfg in scope).
  int      confidence_ic_variant;      // 0=spearman (default), 1+=future
  // v5.10.0a.G.8 — trade-close reward multiplier. Real money signal
  // (TP/SL hit) gets weighted ×N over the slow-path lookback rewards
  // (which are hypothetical "would have been correct" signals).
  // Default 4.0 — operator-tunable. Higher = trust trade outcomes more
  // (faster convergence to deployable weights); lower = faster cold-
  // start learning from dense lookback signals.
  double   ensemble_trade_reward_mult;
  // v5.10.0a.G.9 — bandit state persistence cadence. Every N total
  // bandit updates (across all regimes), flush bandit_state.json to
  // <model_dir>/bandit_state.json. Default 5000 — with ~1 update per
  // poll_interval ticks, that's roughly 1 save per 500K ticks (≈ 1
  // save/hour at typical tick rates). Set to 0 to disable periodic
  // saves (state still saved on engine clean shutdown).
  int      ensemble_bandit_save_interval;
  // v5.14.10.B — Bayesian Thompson sampling bandit (alternative bandit weight
  // provider; cfg.bandit_algorithm enum picks Exp3-IX vs Thompson vs Both).
  // Cfg-flag eligibility analysis (per cfg-flag-eligibility-criteria.md):
  // bandit_algorithm is INT enum (3 values), thompson_*_prior/obs are FPN
  // scalars, thompson_rng_seed is uint64 — none are booleans → all REJECT
  // for FOREACH_ML_CFG_FLAG bitmap migration. Stay as direct cfg fields.
  // Stamp-bound via FOREACH_STAMP_BOUND_CFG (drift detection); rng_seed
  // excluded (RNG state is runtime-only; doesn't affect inference reproducibility
  // across stamps — only within-run determinism via seeded splitmix64).
  // See ML_Headers/ThompsonBandit.hpp for math kernel + replay-determinism
  // PARITY-014 contract; see ML_Headers/BanditAlgorithmRegistry.hpp for
  // FOREACH_BANDIT_ALGORITHM dispatch (3 algos: EXP3=0, THOMPSON=1, BOTH=2).
  int      bandit_algorithm;          // 0=EXP3 (default), 1=THOMPSON, 2=BOTH (parallel A/B)
  FPN<F>   thompson_mu_prior;         // posterior mean prior; default 0.0
  FPN<F>   thompson_precision_prior;  // posterior precision prior (= 1/variance); default 1.0
  FPN<F>   thompson_precision_obs;    // observation precision; default 1.0
  uint64_t thompson_rng_seed;         // splitmix64 seed; default 42
  FPN<F>   thompson_exp3_blend_alpha; // v5.15.5.F.4d — BLENDED state-4 blend ratio; default 0.5 (only consumed when bandit_algorithm=4)

  //==================================================================================================
  // [v5.15.5.F.4c.3 — PER-CORE AUTHORITATIVE CONFIG]
  //==================================================================================================
  // One PerCoreCfg<F> instance per execution core. Step 2 shadow window: each
  // per-core field exists in BOTH the flat fields above (legacy path) AND in
  // cores[c].<field> (per-core authoritative path). Parser populates cores[c]
  // from flat after parse via ControllerConfig_PopulateCoresFromFlat (Step 3
  // adds [core N] section parser that writes cores[c] directly).
  //
  // Consumer migration: _BuildParameters takes const PerCoreCfg<F>* per the
  // HIGH-1 Option A boundary-stable sig change (4 fn sigs + 11 call sites).
  // Production read sites (~32) migrate cfg.X → cfg.cores[c].X one cohort at
  // a time. After last consumer migrates, flat fields delete.
  //
  // DESIGN_SPECS/per-instance-registry-pattern.md — first canonical
  // application; sister axes (per-symbol, per-strategy, per-horizon,
  // per-regime) instantiate analogous PerXxxCfg<F> arrays.
  //
  // alignas(64) on PerCoreCfg<F> ensures cores[0..N-1] addresses are all
  // cache-line aligned — per-core slow-path reads land in distinct cache
  // lines without cross-core false sharing (H6 discipline).
  //==================================================================================================
  PerCoreCfg<F> cores[MAX_EXECUTION_CORES];

  // === Per-core legacy parallel arrays (documented exemptions; auto-generated via X-macro) ===
  // WIP2d-0.B (.F.4c.3) — 11 parallel arrays consolidated into FOREACH_MANUAL_PER_CORE_FIELD
  // X-macro expansion per CLAUDE.md item 31 framework discipline + H17 STRONG codification.
  // ONE source of truth; CI cross-checks bidirectional sync with DOCS/MANUAL_FIELDS_INVENTORY.md.
  //
  // SECTIONS:
  //   • 5 string arrays (core_model_path/_dir/_horizon_list/_ensemble_blend_mode/_disabled_horizons)
  //     → migrate to KIND_FILE_PATH/KIND_STRING cohort at .F.4e
  //   • 1 hex64 bitmap (core_feature_mask) → migrate to KIND_HEX64 cohort at .F.4e
  //   • 4 TRANSITIONAL override arrays (core_risk_pct / core_strategies / core_time_exit_ticks /
  //     core_max_drawdown_pct) → delete at WIP2g when cores[c] becomes authoritative
  //   • 1 TRANSITIONAL legacy override struct (core_overrides) → delete at WIP2f with
  //     PerCoreOverrides<F> retirement (cfg-scope-discipline.md § Anti-pattern 1)
  //
  // CI script: tools/check_per_core_registry_integrity.py verifies every entry has a
  // FOREACH_MANUAL_PER_CORE_FIELD row + a MANUAL_FIELDS_INVENTORY.md row + correct type.
  FOREACH_MANUAL_PER_CORE_FIELD(EMIT_MANUAL_PER_CORE_DECL)

  //==================================================================================================
  // [Global cfg field auto-generation — v5.15.5.F.4d.1.B.3 Step 0.5b.B Path α landing]
  //==================================================================================================
  // 48 global cfg fields auto-generated from FOREACH_GLOBAL_CFG_FIELD (one source of truth at
  // CoreFrameworks/CfgFieldRegistry.hpp). Sister to FOREACH_PER_CORE_CFG_FIELD(EMIT_PER_CORE_CFG_STRUCT_FIELD)
  // at PerCoreCfg<F>:324 — closes the global↔per-core column asymmetry per Decision A (a) Path α.
  //
  // Per H17 invariant (STRONG at per-core; HARD-promoted at global surface at .B.3 Step 0.5b.B
  // codification): adding a new global cfg field = 1 row in FOREACH_GLOBAL_CFG_FIELD; the row's
  // STORAGE_T column drives the struct field decl + parser auto-flow + GUI render + drift check.
  // NO manual cfg-surface field declarations allowed in ControllerConfig<F> body outside of this
  // X-macro invocation (legacy fields at struct top are pre-registry-era; future cfg fields go
  // through the registry).
  //
  // Step 0.5b.B closure: 48 manual cfg field decls atomically replaced by this single X-macro
  // invocation. CI Check 9 (LANDED v5.15.5.F.4d.1.B.3 WIP-9 at CfgFieldRegistry.hpp:1156-1180) static_asserts STAMP_BOUND_CFG_DERIVED cohort
  // coverage. Closes TECH_DEBT-093 (gap_acceptable_threshold manual storage cleanup);
  // ALSO closes 47 sibling cohort manual decls in same atomic edit (struct-gen + bulk delete).
  //
  // SEE DESIGN_SPECS/universal-cfg-field-registry-pattern.md for the full pattern doc.
  FOREACH_GLOBAL_CFG_FIELD(EMIT_GLOBAL_CFG_STRUCT_FIELD)
};

//======================================================================================================
// [FEE_COMPUTE — Phase 8 maker/taker helper]
//======================================================================================================
// Apply the correct fee rate based on whether the fill was a maker or taker.
// Single source of truth for fee math on a per-fill basis.
//
// Caller-side discipline (CLAUDE.md "Maker/Taker Accuracy" invariant in c7):
//   - ENTRY fees: pass order->is_maker from the matching fill
//   - EXIT fees from market sells (TP/SL hits): pass is_maker=0 (always taker)
//   - EXIT fees from limit sells (Phase 9 hybrid execution, deferred): pass
//     order->is_maker from the matching exit fill
//
// Backtest path: pass is_maker=0 always — backtest simulates as all-taker
// (documented divergence; backtest maker simulation is Phase 9 work).
//
// In legacy cfg mode (only fee_rate set, mirrored to maker+taker), both
// branches return identical values → behavior matches pre-Phase-8.
template <unsigned F>
inline FPN<F> Fee_Compute(const ControllerConfig<F>* cfg, FPN<F> notional, int is_maker) {
    FPN<F> rate = is_maker ? cfg->fee_rate_maker : cfg->fee_rate_taker;
    return FPN_Mul(notional, rate);
}

//======================================================================================================
// [PER-CORE CFG RESOLVE — v4.0]
//======================================================================================================
// Build a stack-local copy of the global cfg with per-core overrides applied.
// Strategies receive the resolved config and don't need to know about the
// override mechanism. Cost: one ~12.5KB struct copy + 18 conditionals per
// rebuild. With 4 cores at 5Hz slow path = 20 copies/sec, ~250KB/sec — well
// inside any reasonable budget.
//
// Zero in any override field means "inherit global". This matches the
// existing strategy-type-override convention (mr_tp_pct=0 → use take_profit_pct).
//======================================================================================================
template <unsigned F>
inline ControllerConfig<F> ControllerConfig_ResolveForCore(
    const ControllerConfig<F>& global, int core_id) {
    ControllerConfig<F> resolved = global;
    if (core_id < 0 || core_id >= 16) return resolved;
    const PerCoreOverrides<F>& ov = global.core_overrides[core_id];
    // v4.7.24: resolver auto-derives from PER_CORE_OVERRIDE_FIELDS. Adding
    // a new field = 1 line in the macro list, not a line here.
#define _RESOLVE_OV_FIELD(name) if (!FPN_IsZero(ov.name)) resolved.name = ov.name;
    PER_CORE_OVERRIDE_FIELDS(_RESOLVE_OV_FIELD, _RESOLVE_OV_FIELD)
#undef _RESOLVE_OV_FIELD
// v4.7.40: INT overrides — 0 = inherit (caller's config field already
// has the global default; non-zero overrides it).
#define _RESOLVE_OV_INT_FIELD(name) if (ov.name != 0) resolved.name = ov.name;
    PER_CORE_OVERRIDE_INT_FIELDS(_RESOLVE_OV_INT_FIELD)
#undef _RESOLVE_OV_INT_FIELD
// v5.14.9.F.6: BITMAP overrides — branchless bit-select per domain.
//   resolved = (override_set & override_values) | (~override_set & global_values)
// Bits set in override_set use the override value; bits clear inherit global.
// Per-bit override (operator can override SOME bits in a domain, leave others
// inherited). 0 override_set = full inherit (no overrides this domain).
#define _RESOLVE_OV_BITMAP_FIELDS(d_lower, D_UPPER, stype, FOREACH_macro) \
    {                                                                          \
        stype _ov_set = ov.d_lower##_cfg_flags_override_set;                   \
        stype _ov_val = ov.d_lower##_cfg_flags_override;                       \
        stype _global = global.d_lower##_cfg_flags;                            \
        resolved.d_lower##_cfg_flags = (stype)((_ov_set & _ov_val) | ((stype)~_ov_set & _global)); \
    }
    PER_CORE_OVERRIDE_BITMAP_DOMAINS(_RESOLVE_OV_BITMAP_FIELDS)
#undef _RESOLVE_OV_BITMAP_FIELDS
    return resolved;
}

//======================================================================================================
// [PER-CORE SHADOW POPULATE — v5.15.5.F.4c.3 Step 2]
//======================================================================================================
// Copies the resolved per-core view (flat fields + legacy PerCoreOverrides
// merged via ControllerConfig_ResolveForCore) into cfg->cores[c] for each
// execution core. This is the Step 2 SHADOW transition: cores[c] is populated
// from the flat path so consumers migrating to read cfg.cores[c].<field> see
// identical values to consumers still reading cfg.<field> + ResolveForCore.
//
// The X-macro walker iterates FOREACH_PER_CORE_CFG_FIELD; each row emits one
// field assignment `cfg->cores[c].name = resolved.name`. Adding a new per-core
// row to the registry automatically adds a populate line — no changes needed
// here.
//
// Called at end of ControllerConfig_Default + ControllerConfig_Load. After
// Step 3 lands the `[core N]` section parser, the parser will write cores[c]
// directly + this shadow function becomes a no-op / gets deleted.
//
// Runtime note: GUI cfg edits mutate the flat fields directly via
// tt::cfg_render_field<T>; cores[c] becomes stale until next reload. Step 2
// consumers tolerate this (no migrated reader runs on the GUI-mutated path
// without a reload signal yet). Step 6 wires GUI edits to also update
// cores[c] via reload-from-file or direct-sync hook.
//======================================================================================================
template <unsigned F>
inline void ControllerConfig_PopulateCoresFromFlat(ControllerConfig<F>* cfg) {
    for (int c = 0; c < MAX_EXECUTION_CORES; ++c) {
        ControllerConfig<F> resolved = ControllerConfig_ResolveForCore(*cfg, c);
        // Per-core registry rows — X-macro auto-walker over FOREACH_PER_CORE_CFG_FIELD.
        // Future per-core row additions auto-flow here; no edits needed.
        // WIP2d-1.B.0 — copy walker filters by NO_FLAT_FIELD (was HAS_SIDE_EFFECT, which was
        // overloaded). Rows with NO_FLAT_FIELD have no scalar on ControllerConfig (resolved.name
        // doesn't exist; e.g., `strategy` lives only on cores[c] + legacy core_strategies[16]).
        // The if-constexpr inside templated PopulateCoresFromFlat<F> discards the cfg-access branch
        // at instantiation, making row body syntactically valid for ALL rows. Rows with MANUAL_PARSER
        // (but NOT NO_FLAT_FIELD) DO have flat scalars — copy walker should populate cores[c].X from
        // resolved.X. Pre-WIP2d-1.B.0 the walker SKIPPED these rows (latent regression from Phase 1
        // HAS_SIDE_EFFECT overload); the bit split restores the correct copy semantic.
        #define EMIT_PER_CORE_COPY(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload, tooltip, \
                                    applies_to_strategy, applies_to_op_mode, \
                                    applies_to_regime, applies_to_risk, lives_in_struct) \
            if constexpr (!((meta) & CfgFieldDescriptor::NO_FLAT_FIELD)) { \
                cfg->cores[c].name = resolved.name; \
            }
        FOREACH_PER_CORE_CFG_FIELD(EMIT_PER_CORE_COPY)
        #undef EMIT_PER_CORE_COPY

        // WIP2d-1.B.0 — AUTOPOPULATE NO_FLAT_FIELD sync (replaces WIP2d-1 Phase 1 ad-hoc manual line).
        // Per autopopulate-pattern-for-production-caller-class.md: registry-driven sync of NO_FLAT_FIELD
        // rows from their legacy parallel-array sources. Future per-core-only fields = 1 row in
        // FOREACH_PER_CORE_NO_FLAT_FIELD_SYNC (target ← source) + 1 row in FOREACH_PER_CORE_CFG_FIELD
        // with NO_FLAT_FIELD bit. The auto-flow then generates sync line + skips copy walker
        // mechanically. Closes Shortsighted #4 (manual sync line ad-hoc).
        FOREACH_PER_CORE_NO_FLAT_FIELD_SYNC(EMIT_NO_FLAT_FIELD_SYNC)

        // 5 cfg-domain bitmap STORAGE fields — manual copies (not in registry; A2 flat KIND_BOOL
        // rows ship at WIP2e and rebuild these bitmaps from rows at slow-path rebuild). The
        // resolved view already merges per-core bitmap overrides via the legacy
        // PerCoreOverrides<F> bitmap path in ControllerConfig_ResolveForCore.
        cfg->cores[c].lifecycle_cfg_flags = resolved.lifecycle_cfg_flags;
        cfg->cores[c].gate_cfg_flags      = resolved.gate_cfg_flags;
        cfg->cores[c].ml_cfg_flags        = resolved.ml_cfg_flags;
        cfg->cores[c].risk_cfg_flags      = resolved.risk_cfg_flags;
        cfg->cores[c].ops_cfg_flags       = resolved.ops_cfg_flags;
    }
}

//======================================================================================================
template <unsigned F> inline ControllerConfig<F> ControllerConfig_Default() {
  ControllerConfig<F> cfg;

  // v5.15.5.F.4d.1.B.3 Step 1.6.1 — auto-defaults for all 48 global cfg fields via
  // FOREACH_GLOBAL_CFG_FIELD(EMIT_GLOBAL_CFG_DEFAULT). Sister to struct-gen invocation
  // at line 1338+ (Step 0.5b.B). Closes TECH_DEBT-093 (gap_acceptable_threshold full closure)
  // + future-headache reducer for all 48 globals. Manual default lines DELETED below
  // (Python script /tmp/delete_manual_defaults.py).
  //
  // tt::cfg_assign_field reads descriptor.payload per KIND dispatch (FPN<F> from as_double;
  // int/uint{8,16,32,64}_t from as_int or as_bool; KIND_INT_ENUM from as_int_enum).
  // Defaults baked in registry rows at CfgFieldRegistry.hpp:255-419 — single source of truth.
  FOREACH_GLOBAL_CFG_FIELD(EMIT_GLOBAL_CFG_DEFAULT)

  // v5.15.5.F.4d.1.B.3 Step 8.6 (2026-05-24): poll_interval MATCH — registry INT(100) == manual; DELETED.
  // warmup_ticks DIFFER: registry INT(0, 0, 100000000); manual=128. Manual=128 is operational floor
  // (minimum raw ticks before trading; pre-registry historical value). Keep manual; registry default
  // for new operators OR when key absent could be either; manual is the safer "always warmup" floor.
  cfg.warmup_ticks = 128; // KEEP — registry INT(0) too permissive for trading-ready boot
  // min_warmup_samples DIFFER: registry INT(64, 0, 128); manual=0 (disables; uses warmup_ticks only).
  // Manual=0 preserves the pre-cfg behavior where min_warmup_samples was unconditional 0. Keep manual.
  cfg.min_warmup_samples = 0;  // KEEP — registry INT(64) would add a samples-floor; manual=0 preserves pre-cfg behavior
  // v5.15.5.F.4d.1.B.3 Step 8.6: csv_sort_check_mode DIFFER — registry INT(1=STRICT); manual=CSV_SORT_WARN(0).
  cfg.csv_sort_check_mode = CSV_SORT_WARN; // KEEP — registry INT(1) STRICT would refuse mis-sorted CSVs; manual=WARN(0) logs + proceeds per v5.9.2c contract
  cfg.r2_threshold = FPN_FromDouble<F>(0.30);
  cfg.slope_scale_buy = FPN_FromDouble<F>(0.50);
  cfg.max_shift =
      FPN_FromDouble<F>(0.0001); // 0.01% of price — e.g. $7 at BTC $70k
  cfg.take_profit_pct = FPN_FromDouble<F>(0.03);
  cfg.stop_loss_pct = FPN_FromDouble<F>(0.015);
  cfg.starting_balance =
      FPN_FromDouble<F>(1000000.0); // 1M default so tests arent balance-limited
  cfg.fee_rate = FPN_FromDouble<F>(0.001); // 0.1% per trade (Binance default)
  // Phase 8 — Binance tier 0 BNB-discount default rates. Live engine uses
  // these per-fill based on order->is_maker. If user sets only fee_rate
  // (legacy mode), backward-compat clause below mirrors to both.
  cfg.fee_rate_maker = FPN_FromDouble<F>(0.00075); // 0.075% maker tier 0
  cfg.fee_rate_taker = FPN_FromDouble<F>(0.00100); // 0.100% taker tier 0
  cfg.pay_fees_in_bnb = 0;                          // v4.3.2: 1 = apply BNB 25% discount
  cfg.risk_pct = FPN_FromDouble<F>(0.02);  // risk 2% of balance per position
  cfg.volume_multiplier = FPN_FromDouble<F>(3.0);
  cfg.entry_offset_pct = FPN_FromDouble<F>(0.0015);
  cfg.spacing_multiplier = FPN_FromDouble<F>(2.0);
  cfg.offset_min = FPN_FromDouble<F>(0.0005);     // 0.05% - most aggressive
  cfg.offset_max = FPN_FromDouble<F>(0.005);      // 0.5%  - most defensive
  cfg.vol_mult_min = FPN_FromDouble<F>(1.5);      // 1.5x  - most aggressive
  cfg.vol_mult_max = FPN_FromDouble<F>(6.0);      // 6.0x  - most defensive
  cfg.filter_scale = FPN_FromDouble<F>(0.50);     // how fast filters adapt
  cfg.max_drawdown_pct = FPN_FromDouble<F>(0.10); // halt at 10% drawdown
  cfg.max_exposure_pct =
      FPN_FromDouble<F>(0.50); // max 50% of balance in positions
  // v5.15.5.F.4d.1.B.3 Step 8.6: max_positions MATCH — registry INT(1) == manual; DELETED.
  cfg.offset_stddev_mult = FPN_Zero<F>(); // 0 = disabled, use percentage mode
  cfg.offset_stddev_min =
      FPN_FromDouble<F>(0.5); // 0.5 stddev - most aggressive
  cfg.offset_stddev_max = FPN_FromDouble<F>(4.0); // 4.0 stddev - most defensive
  cfg.min_long_slope = FPN_Zero<F>();             // 0 = disabled
  cfg.min_buy_delta = FPN_FromDouble<F>(
      -0.3); // allow mild selling, block heavy (-0.3 threshold)
  cfg.min_stddev_pct =
      FPN_Zero<F>(); // 0 = disabled (set in engine.cfg for live: 0.0003)
  cfg.momentum_r2_min =
      FPN_Zero<F>(); // 0 = disabled (set in engine.cfg for live: 0.4)
  cfg.tp_hold_score = FPN_Zero<F>();          // 0 = disabled, use fixed TP
  cfg.tp_trail_mult = FPN_FromDouble<F>(1.0); // trail 1 stddev below price
  cfg.sl_trail_mult = FPN_FromDouble<F>(2.0); // trail SL 2 stddevs below price
  cfg.fee_floor_mult =
      FPN_FromDouble<F>(3.0);      // TP floor = entry × fee_rate × 3
  cfg.vwap_offset = FPN_Zero<F>(); // 0 = disabled (backward compat)
  cfg.min_sl_tp_ratio = FPN_FromDouble<F>(0.5); // 2:1 reward/risk floor
  cfg.ror_tp_bonus =
      FPN_FromDouble<F>(1.2); // 20% wider TP on accelerating trend
  cfg.momentum_tp_r2_min = FPN_FromDouble<F>(0.5); // TP scale at R²=0
  cfg.momentum_sl_r2_max = FPN_FromDouble<F>(1.5); // SL scale at R²=0
  cfg.squeeze_decay = FPN_FromDouble<F>(0.10);     // 10% of gap per cycle
  cfg.offset_adapt_scale = FPN_FromDouble<F>(0.001);
  cfg.stddev_adapt_scale = FPN_FromDouble<F>(0.1);
  cfg.vol_adapt_scale = FPN_FromDouble<F>(0.1);
  cfg.breakout_min = FPN_FromDouble<F>(0.5); // 0.5 stddev floor
  // v5.15.5.F.4d.1.B.3 Step 8.6: slow_path_max_secs DIFFER — registry INT(60); manual=3. Manual=3 is
  // per-cycle slow-path budget (seconds per rebuild iteration); registry default 60 too permissive
  // for HFT cadence. Keep manual.
  cfg.slow_path_max_secs = 3;  // KEEP — registry INT(60) too permissive for HFT slow-path cadence
  cfg.max_hold_ticks = 0; // 0 = disabled
  // v5.12.3.C — all per-core overrides default 0 (use global). Operator
  // sets specific cores via core_<N>_time_exit_ticks=<value> in cfg.
  for (int i = 0; i < 16; ++i) cfg.core_time_exit_ticks[i] = 0;
  cfg.min_hold_gain_pct =
      FPN_FromDouble<F>(0.001); // 0.1% — only time-exit if below this gain
  // regime detection
  cfg.regime_slope_threshold =
      FPN_FromDouble<F>(0.00002); // legacy (unused by crossover classifier)
  cfg.regime_crossover_threshold = FPN_FromDouble<F>(
      0.0005); // 0.05% EMA-SMA gap = mild trend (~$35 at BTC $70k)
  cfg.regime_strong_crossover = FPN_FromDouble<F>(
      0.0015); // 0.15% EMA-SMA gap = strong trend (~$102 at BTC $68k)
  cfg.regime_r2_threshold =
      FPN_FromDouble<F>(0.70); // 70% consistency for trending
  cfg.regime_volatile_stddev =
      FPN_FromDouble<F>(0.0005); // 0.05% stddev/price (legacy compat)
  cfg.regime_vol_spike_ratio =
      FPN_FromDouble<F>(2.0);   // variance spike: 2x baseline = volatile
  cfg.regime_hysteresis = 5;    // 5 slow-path cycles before switch
  cfg.idle_reset_cycles = 30;   // ~90s idle before gate decay to initial
  cfg.sl_cooldown_cycles = 5;   // 5 slow-path cycles pause after SL
  cfg.sl_cooldown_adaptive = 0; // 0 = fixed, 1 = adaptive (backward compat)
  cfg.sl_cooldown_base = 2;     // min cooldown (spike recovery)
  cfg.sl_cooldown_extra = 8;    // max extra (strong downtrend)
  // momentum strategy
  cfg.momentum_breakout_mult = FPN_FromDouble<F>(1.5); // buy 1.5σ above avg
  // v5.7.5 — MOM quality filters default 0 (off, preserves pre-v5.7 behavior)
  cfg.momentum_min_tp_margin_pct    = FPN_Zero<F>();
  cfg.momentum_min_buy_delta_recent = FPN_Zero<F>();
  cfg.momentum_min_r2               = FPN_Zero<F>();
  cfg.momentum_require_last_win     = 0;
  cfg.momentum_tp_mult = FPN_FromDouble<F>(3.0);       // wider TP for trends
  cfg.momentum_sl_mult = FPN_FromDouble<F>(1.0);       // tighter SL than MR
  // EMA cross strategy
  cfg.emacross_dip_mult = FPN_FromDouble<F>(0.5);         // buy 0.5σ below EMA
  cfg.emacross_crossover_min = FPN_FromDouble<F>(0.0003); // 0.03% min spread
  cfg.emacross_trail_mult =
      FPN_FromDouble<F>(1.5); // 1.5x trail when EMA rising
  // volume spike detection
  cfg.spike_threshold = FPN_FromDouble<F>(5.0); // 5x rolling max triggers spike
  cfg.spike_spacing_reduction = FPN_FromDouble<F>(0.5); // half spacing on spike
  // v5.14.9.F — lifecycle_cfg_flags defaults via AUTOPOPULATE_FROM_TRIPLE:
  //   partial_exit_enabled  = 0 (disabled — backward compat)
  //   breakeven_on_partial  = 1 (move SL to entry after TP1 hit)
  //   breakeven_on_profit   = 0 (disabled — currently dormant per TECH_DEBT-024)
  LIFECYCLE_CFG_FLAG_AUTOPOPULATE_FROM_TRIPLE(cfg.lifecycle_cfg_flags,
      /*partial_exit_enabled*/  0,
      /*breakeven_on_partial*/  1,
      /*breakeven_on_profit*/   0);
  // v5.14.9.F.1 — gate_cfg_flags defaults: all 6 flags off (backward compat)
  GATE_CFG_FLAG_AUTOPOPULATE_FROM_HEX(cfg.gate_cfg_flags,
      /*depth_enabled*/                0,
      /*gate_ema_enabled*/             0,
      /*no_trade_band_enabled*/        0,
      /*cost_gate_enabled*/            0,
      /*barrier_gate_enabled*/         0,
      /*param_staleness_gate_enabled*/ 0);
  // v5.14.9.F.2 — ml_cfg_flags defaults: all 7 flags off (backward compat)
  ML_CFG_FLAG_AUTOPOPULATE_FROM_SEPTUPLE(cfg.ml_cfg_flags,
      /*confidence_enabled*/           0,
      /*confidence_composite_enabled*/ 0,
      /*bandit_enabled*/               0,
      /*exit_bandit_enabled*/          0,
      /*use_exit_model*/               0,
      /*foxml_vol_scaling_enabled*/    0,
      /*lazy_rebuild_enabled*/         0);
  // v5.14.9.F.3 — risk_cfg_flags defaults: kill_switch ON (safety-first), rest OFF
  RISK_CFG_FLAG_AUTOPOPULATE_FROM_TRIPLE(cfg.risk_cfg_flags,
      /*kill_switch_enabled*/          1,
      /*vol_sizing_enabled*/           0,
      /*ws_dead_time_flatten_enabled*/ 0);
  // v5.14.9.F.3 — ops_cfg_flags defaults: all flags off (backward compat).
  // v5.15.5.A.7 — Cohort grew from 2 → 4 entries with ACKNOWLEDGE_INFERENCE_CFG_DRIFT
  // + ACKNOWLEDGE_CROSS_BINARY_DRIFT migration. FROM_PAIR macro retired (couldn't
  // generalize to 4-arg cleanly); direct zero-init suffices since all 4 entries
  // default OFF. Operator cfg keys set bits via FOREACH_OPS_CFG_FLAG parser walker
  // at line ~2220 (legacy_field column auto-routes legacy key names).
  cfg.ops_cfg_flags = 0;
  cfg.partial_exit_pct = FPN_FromDouble<F>(0.5); // 50% at TP1, 50% rides
  cfg.tp2_mult = FPN_FromDouble<F>(2.0);         // TP2 = 2x TP1 distance
  cfg.breakeven_buffer_pct =
      FPN_FromDouble<F>(0.0005);    // +0.05% above entry (lock in tiny profit)
  cfg.slippage_pct = FPN_Zero<F>(); // 0 = disabled (backward compat)
  // session_filter_enabled migrated to ops_cfg_flags (default 0)
  // v5.15.5.B.5 — session multipliers default-init via FOREACH_SESSION_PHASE.
  // Per-session default is the MULT column of the registry tuple.
#define X(NAME_U, name_l, START, END, MULT, DOC) \
  cfg.session_##name_l##_mult = FPN_FromDouble<F>(MULT);
  FOREACH_SESSION_PHASE(X)
#undef X
  // depth_enabled migrated to gate_cfg_flags (default 0; set above via AUTOPOPULATE)
  cfg.min_book_imbalance = FPN_Zero<F>(); // 0 = disabled
  // EMA gate — gate_ema_enabled migrated to gate_cfg_flags (default 0)
  cfg.gate_ema_alpha = FPN_FromDouble<F>(0.997); // ~333-tick effective window
  cfg.gate_ema_one_minus_alpha = FPN_FromDouble<F>(0.003); // 1.0 - 0.997
  cfg.default_strategy = -1; // -1 = regime auto (backward compat)
  cfg.use_real_money = 0;    // 0 = paper trading (default safe)
  // v5.15.5.F.4d.1.B.3 Step 8.6: acknowledge_hardcoded_strategy_in_live MATCH — registry BOOL(0) == manual; DELETED.
  // init_arena_use_hugepages MATCH — registry BOOL(0) == manual; DELETED.
  // require_mlockall DIFFER: registry BOOL(0); manual=1 (HFT-correct; safety-first). Keep manual —
  // registry default 0 is laptop-dev permissive; manual 1 forces operator to opt-out for dev (set to 0
  // for laptop where RLIMIT_MEMLOCK is tight).
  cfg.require_mlockall = 1;  // KEEP — registry BOOL(0) too permissive; HFT-correct default safety-first
  // kill switch
  // kill_switch_enabled migrated to risk_cfg_flags (default 1 — safety-first; set via AUTOPOPULATE above)
  cfg.kill_switch_daily_loss_pct =
      FPN_FromDouble<F>(0.03); // 3% daily loss triggers kill
  cfg.kill_switch_drawdown_pct =
      FPN_FromDouble<F>(0.05); // 5% drawdown from session peak
  cfg.kill_recovery_warmup =
      50; // 50 slow-path cycles observation after kill reset
  // vol-scaled sizing
  // vol_sizing_enabled migrated to risk_cfg_flags (default 0)
  cfg.vol_scale_min = FPN_FromDouble<F>(0.25);
  cfg.vol_scale_max = FPN_FromDouble<F>(2.0);
  // no-trade band
  // no_trade_band_enabled migrated to gate_cfg_flags (default 0)
  cfg.no_trade_band_mult = FPN_FromDouble<F>(3.0);
  // ML inference (disabled by default — zero overhead when off)
  // v5.15.5.F.4d.1.B.3 Step 8.6: ml_backend MATCH — registry INT(0) == manual; DELETED.
  cfg.ml_model_path[0] = '\0';
  cfg.ml_buy_threshold = FPN_FromDouble<F>(0.6);
  cfg.ml_tp_pct = FPN_FromDouble<F>(0.015); // 1.5% TP
  cfg.ml_sl_pct = FPN_FromDouble<F>(0.008); // 0.8% SL
  // v5.15.5.F.4d.1.B.3 Step 8.6: regime_model_backend MATCH — registry INT(0) == manual; DELETED.
  cfg.regime_model_path[0] = '\0';
  cfg.regime_model_weight = FPN_FromDouble<F>(2.0);
  // v5.15.5.F.4d.1.B.3 Step 8.6: danger_enabled DIFFER — registry BOOL(0); manual=1 (gradient ON).
  // Keep manual — registry default 0 would disable the danger gradient by default; manual=1 is
  // the operationally-correct "always-on safety gradient" setting. Future operator opts out via cfg.
  // danger gradient
  cfg.danger_enabled = 1;  // KEEP — registry BOOL(0) would disable safety gradient; manual=1 is safety-first
  cfg.danger_warn_stddevs =
      FPN_FromDouble<F>(3.0); // gradient starts at 3σ below avg
  cfg.danger_crash_stddevs =
      FPN_FromDouble<F>(6.0); // full gate kill at 6σ below avg
  // v5.15.5.F.4d.1.B.3 Step 8.6: 3 record_* fields MATCH registry defaults (BOOL(0)/BOOL(0)/INT(30)); DELETED.
  // (tick recording disabled by default — no disk usage unless operator opts in)
  // Phase 8b — operational alerts (all opt-in; default = no behavior change)
  // notify_enabled migrated to ops_cfg_flags (default 0)
  // v5.15.5.F.4d.1.B.3 Step 8.6: notify_backend MATCH (registry INT(0)); notify_cooldown_secs MATCH (INT(60)); both DELETED.
  cfg.notify_command[0] = '\0';
  // FoxML integration — Phase 6C (all OFF by default, zero behavior change)
  // cost_gate_enabled migrated to gate_cfg_flags (default 0)
  // foxml_vol_scaling_enabled migrated to ml_cfg_flags (default 0)
  cfg.foxml_vol_scaling_z_max = FPN_FromDouble<F>(3.0);
  // bandit_enabled migrated to ml_cfg_flags (default 0)
  cfg.bandit_blend_ratio = FPN_FromDouble<F>(0.30);
  // confidence_enabled migrated to ml_cfg_flags (default 0)
  // Phase 6 prep — defaults match the pre-amend hardcoded values
  cfg.confidence_window           = 32;                          // CONFIDENCE_IC_WINDOW_DEFAULT
  // v5.14.9.D — DELETED cfg.confidence_freshness_tau default (TECH_DEBT-004 close).
  cfg.confidence_threshold_scale  = FPN_FromDouble<F>(2.0);      // hardcoded `2.0` in gate formula
  // v5.9.1 — hard-block floor. 0.0 = disabled (pre-v5.9.1 behavior).
  // Operator opts in (audit-recommended 0.05) for the noise-floor protection.
  cfg.confidence_hard_block_threshold = FPN_FromDouble<F>(0.0);
  // v5.15.5.F.4d.1.B.3 Phase F HIGH-1 (b) — held_out_fraction default removed (was 0.20);
  // FOREACH_GLOBAL_CFG_FIELD(EMIT_GLOBAL_CFG_DEFAULT) at function start applies registry DBL(0.20, 0.0, 1.0).
  // Sister to gap_acceptable_threshold cleanup at Step 1.6.1 (TECH_DEBT-093 closure).
  // TECH_DEBT-093 FULL closure. Other 47 manual defaults audited and retained: some diverge from
  // registry defaults (e.g., warmup_ticks registry=0 but manual=128); registry default audit deferred
  // to follow-up TECH_DEBT entry (see TECH_DEBT-107 NEW at ship close).
  // v5.15.5.F.4d.1.B.3 Step 8.6: held_out_gate_strict MATCH (registry INT(0)); allow_cross_major_engine MATCH (BOOL(0)); auto_stamp_on_held_out MATCH (BOOL(1)); 3 manual defaults DELETED.
  cfg.held_out_stamp_secret[0]    = '\0';                         // empty = accept-any (dev)
  cfg.auto_stamp_secret[0]        = '\0';                         // v5.11.47 default empty = devmode (signs stamps but accepts any signature at load); operator sets in cfg or GUI Validation panel
  cfg.health_log_path[0]          = '\0';                         // empty = disabled
  // v5.15.5.F.4d.1.B.3 Step 8.6: health_log_max_bytes DIFFER — registry INT(1048576); manual=0 (no rotation; back-compat). Keep manual.
  cfg.health_log_max_bytes        = 0;                            // KEEP — registry INT(1MB) would rotate by default; manual=0 preserves back-compat (no rotation)
  // health_log_keep_count DIFFER — registry INT(5); manual=0 (keep all retained files; back-compat). Keep manual.
  cfg.health_log_keep_count       = 0;                            // KEEP — registry INT(5) caps retained files; manual=0 preserves back-compat (no rotation)
  // v5.15.5.A.7 — acknowledge_cross_binary_version_drift + acknowledge_inference_cfg_drift
  // migrated to ops_cfg_flags bitmap. Both default OFF via `cfg.ops_cfg_flags = 0` init above.
  // Legacy default semantics preserved: WARN on cross-binary drift; REFUSE/WARN on inference_cfg.
  // v5.15.5.F.4d.1.B.3 Step 8.6: acknowledge_hot_swap_with_open_positions MATCH — registry BOOL(0) == manual; DELETED.
  cfg.confidence_ic_floor                       = 0.02;           // v5.10.0e — Spearman correlation > random
  cfg.confidence_ic_floor_window                = 86400u;          // v5.10.0e — 24h sustained-breach window
  cfg.auto_kill_on_drift                        = 0;              // v5.10.0e — log only by default; opt-in to auto-kill
  // v5.9.5h — XGBoost training hyperparams (cfg-tunable subset).
  // Defaults match pre-v5.9.5h hardcoded values bytewise; non-tuning
  // operators get identical training output post-upgrade.
  cfg.xgb_subsample           = FPN_FromDouble<F>(0.8);
  cfg.xgb_colsample_bytree    = FPN_FromDouble<F>(0.8);
  // v5.15.5.F.4d.1.B.3 Step 8.6: xgb_min_child_weight MATCH (registry INT(5)); xgb_seed MATCH (INT(42)); 2 manual defaults DELETED.
  strncpy(cfg.xgb_tree_method, "hist", sizeof(cfg.xgb_tree_method) - 1);
  cfg.xgb_tree_method[sizeof(cfg.xgb_tree_method) - 1] = '\0';
  // v5.10.0 Item D — hardware-aware cfg. Defaults match pre-v5.10
  // hardcoded behavior. Two distinct defaults reflect two distinct
  // pre-v5.10 hardcoded sites:
  //   - Train Model worker (BacktestPanels.hpp:2056) was nthread=4 for
  //     faster GUI iter (exploratory; reproducibility not required)
  //   - WF + HeldOut (BacktestEngine.hpp:1352, 1638) were nthread=1
  //     for deterministic per-fold output (validation parity)
  // Setting these NOW separable. Operators wanting all-deterministic
  // workflow set both to 1; operators with bigger boxes can bump both.
  // v5.15.5.F.4d.1.B.3 Step 8.6: xgb_train_nthread MATCH — registry INT(4) == manual 4; DELETED.
  // xgb_eval_nthread DIFFER — registry INT(4); manual=1 (determinism for validation parity).
  cfg.xgb_eval_nthread        = 1;   // KEEP — registry INT(4) breaks per-fold determinism; manual=1 matches BacktestEngine.hpp:1352, 1638 pre-v5.10
  // csv_load_workers DIFFER — registry INT(4); manual=1 (serial CSV load for back-compat).
  cfg.csv_load_workers        = 1;   // KEEP — registry INT(4) would parallelize CSV load; manual=1 matches pre-v5.10 serial behavior
  // multi_horizon_max_threads DIFFER — registry INT(4); manual=1 (CRITICAL: v5.11.45 segfault avoidance).
  cfg.multi_horizon_max_threads = 1; // KEEP — registry INT(4) would re-enable v5.11.45 segfault class; XGBoost+libgomp+pthread interaction fragile. 1 = forced serial (DEFAULT; stable). v5.11.45:
                                      // changed from 0 (auto) -> 1 after segfault reports.
                                      // XGBoost + libgomp + pthread interaction is fragile;
                                      // even with per-pthread omp_set_num_threads(1), libgomp
                                      // state can race across pthreads. Set >1 to opt into
                                      // experimental parallel mode (may segfault).
  // v5.15.5.F.4d.1.B.3 Step 8.6: feature_collect_max_gb DIFFER — registry INT(8); manual=12 (operator-favored ceiling).
  cfg.feature_collect_max_gb  = 12;  // KEEP — registry INT(8) too restrictive; advisory cap; WARN-only
  // wf_split_max_gb MATCH — registry INT(8) == manual; DELETED.
  // held_out_max_gb DIFFER — registry INT(8); manual=4 (tighter operator ceiling).
  cfg.held_out_max_gb         = 4;   // KEEP — registry INT(8) too loose; held-out fold is smaller cohort
  // v5.10.0a — multi-horizon training. Default empty = single-horizon
  // (Train Model uses TrainingPanel's label_forward_ticks). Operator opts
  // in by setting cfg.horizon_list=100,500,1000.
  for (int i = 0; i < ControllerConfig<F>::HORIZON_LIST_MAX; ++i)
      cfg.horizon_list[i] = 0;
  cfg.horizon_count = 0;
  // v5.10.0a.G.6 — ensemble cfg defaults. blend_mode "weighted" engages
  // G.7 Bandit-Exp3 path when ensemble active; "selection" stays on G.4
  // argmax-confidence. Other defaults are conservative (low eta, modest
  // warmup, 60% agreement gate).
  strncpy(cfg.ensemble_blend_mode, "weighted",
          sizeof(cfg.ensemble_blend_mode) - 1);
  cfg.ensemble_blend_mode[sizeof(cfg.ensemble_blend_mode) - 1] = '\0';
  cfg.ensemble_bandit_eta = 0.1;
  cfg.ensemble_min_warmup_predictions = 100;
  // v5.13.4 — sell-side bandit defaults
  // exit_bandit_enabled migrated to ml_cfg_flags (default 0)
  // v5.14.1.E — exit_blender_mode migrated to ml_cfg_flags (v5.14.11.C; default 0)
  // v5.14.1.F — Spearman default (only registered variant today).
  cfg.confidence_ic_variant = 0;
  // v5.14.1.G — portfolio turnover diagnostic defaults
  cfg.confidence_turnover_window = 100;
  cfg.confidence_turnover_topk   = 3;
  cfg.exit_bandit_lr      = 0.1;
  cfg.ensemble_min_agreement_pct = 0.6;
  cfg.ensemble_trade_reward_mult = 4.0;
  cfg.ensemble_bandit_save_interval = 5000;  // v5.10.0a.G.9
  // v5.14.10.B — Bayesian Thompson sampling bandit defaults
  cfg.bandit_algorithm        = 0;                              // 0=EXP3 (default; bytewise-identical pre-v5.14.10)
  cfg.thompson_mu_prior       = FPN_FromDouble<F>(0.0);
  cfg.thompson_precision_prior= FPN_FromDouble<F>(1.0);
  cfg.thompson_precision_obs  = FPN_FromDouble<F>(1.0);
  cfg.thompson_rng_seed       = 42ULL;                          // operator-tunable; 0 = use ThompsonBandit.hpp's THOMPSON_RNG_SEED_DEFAULT
  cfg.thompson_exp3_blend_alpha = FPN_FromDouble<F>(0.5);       // v5.15.5.F.4d — BLENDED state-4 default; 50/50 blend (only consumed when bandit_algorithm=4)
  // v5.10.0a.G.6 — per-core ensemble cfg defaults (empty = inherit global)
  // v5.11.18a — per-core feature_mask defaults (all-bits-on = no masking)
  for (int i = 0; i < 16; ++i) {
      cfg.core_horizon_list[i][0] = '\0';
      cfg.core_ensemble_blend_mode[i][0] = '\0';
      cfg.core_disabled_horizons[i][0] = '\0';
      cfg.core_feature_mask[i] = 0xFFFFFFFFFFFFFFFFULL;  // all features enabled
  }
  // v5.15.5.F.4d.1.B.3 Step 8.6: health_log_level DIFFER — registry INT(1=debug); manual=0=info (less verbose default).
  cfg.health_log_level            = 0;                            // KEEP — registry INT(1=debug) too verbose; manual=0=info matches operator expectation
  // reconcile_interval_sec DIFFER — registry INT(60=periodic); manual=0=boot-only (back-compat).
  cfg.reconcile_interval_sec      = 0;                            // KEEP — registry INT(60) would enable periodic reconcile; manual=0 boot-only preserves back-compat
  cfg.reconcile_dry_run           = 1;                            // legacy field; safer default
  // reconcile_mode DIFFER — registry INT(0=STRICT); manual=1=RECONCILE_WARN (matches dry_run=1 legacy behavior).
  cfg.reconcile_mode              = 1;                            // KEEP — registry INT(0) STRICT mode too aggressive; manual=1=WARN matches dry_run=1 legacy + v5.14.4 contract
  // trading_mode MATCH — registry INT(0) == TRADING_MODE_PAPER == 0; DELETED.
  cfg.cfg_keys_explicit           = 0;                            // v5.15.4 — no keys explicit by default
  cfg.prediction_normalize = 0;
  // barrier_gate_enabled migrated to gate_cfg_flags (default 0)
  // v5.15.5.F.4d.1.B.3 Step 8.6: model_verify_strict DIFFER — registry INT(-1=skip); manual=0=warn.
  cfg.model_verify_strict = 0;  // KEEP — registry INT(-1) SKIP would silently miss model mismatches; manual=0=WARN surfaces them
  cfg.peak_model_path[0] = '\0';
  cfg.valley_model_path[0] = '\0';
  // Per-core sharding (Phase 13+) — DEFAULT IS SHARDED. Sharded is the
  // production engine: per-core ExecutionCore + per-core PortfolioController
  // + central OMS, branchless ~60ns hot path, risk distributed across cores.
  // ENGINE_MODE_SINGLE_CORE remains available for benchmark/regression
  // baselines but is DEPRECATED and emits a runtime warning at startup.
  // Adding new features in legacy-only paths = silent production gap;
  // see CLAUDE.md "Cross-Mode Init Placement" invariant.
  cfg.engine_mode = ENGINE_MODE_SHARDED;
  // v5.0.0 (Phase F): per-core slow-path is the only sharded execution
  // mode. Each engine = a self-contained strategy unit (slow + hot
  // pthread pair). Train-serve parity preserved structurally: all
  // callers (live, backtest) execute the same OneCore helpers on the
  // same state.cores[c].
  // slow_path_pin_offset DIFFER — registry INT(-1); manual=0 (auto-derive drainer_cpu+1).
  cfg.slow_path_pin_offset = 0;  // KEEP — registry INT(-1) means no pin; manual=0 is "auto-derive (drainer_cpu + 1)" — operationally distinct
  // num_execution_cores DIFFER — registry INT(1); manual=4 (operator-default for 4-core deployment).
  cfg.num_execution_cores = 4;   // KEEP — registry INT(1) single-core too conservative for production; manual=4 is operator-typical
  // sharded_force_synthetic MATCH — registry BOOL(0) == manual; DELETED.
  for (int i = 0; i < 16; ++i) cfg.core_strategies[i] = 2;  // STRATEGY_SIMPLE_DIP
  cfg.core_strategies_explicit_set = 0;                       // v5.9.0c: no bits set = all defaulted
  cfg.source_cfg_path[0] = '\0';                              // v5.9.0c: populated by ControllerConfig_Load
  for (int i = 0; i < 16; ++i) cfg.core_risk_pct[i] = FPN_Zero<F>();  // 0 = shared
  // Phase 3: per-core kill switch overrides default to 0 (= use shared).
  for (int i = 0; i < 16; ++i) cfg.core_max_drawdown_pct[i] = FPN_Zero<F>();
  cfg.min_kill_loss = FPN_FromDouble<F>(5.0);   // $5 absolute-loss floor for trip
  cfg.enable_mtm_kill_switch = 1;                // mark-to-market enabled by default
  // v5.12.1.A — disabled by default. Operator opts in for live deployment;
  // backtest MUST keep this off (live-only safety net).
  // ws_dead_time_flatten_enabled migrated to risk_cfg_flags (default 0)
  // v5.15.5.F.4d.1.B.3 Step 8.6: ws_dead_time_flatten_threshold_secs MATCH — registry INT(60) == manual; DELETED.
  // v5.12.1.A.3 — recovery window after a flatten fires. New entries
  // refused for this many seconds while operator-side reconcile catches
  // up to exchange truth. After window expires, flatten_pending +
  // recovery_until_us auto-clear; trading resumes (assuming WS healthy).
  // v5.15.5.F.4d.1.B.3 Step 8.6: recovery_delay_secs DIFFER — registry INT(60); manual=30 (tighter operator-favored window).
  cfg.recovery_delay_secs = 30;  // KEEP — registry INT(60) too long; manual=30 matches reconcile UX expectation
  // v5.12.1.B.3 — disabled by default; flip after measuring slow-path p99.
  // param_staleness_gate_enabled migrated to gate_cfg_flags (default 0)
  // v5.15.5.F.4d.1.B.3 Step 8.6: param_max_age_ticks DIFFER — registry INT(100000); manual=1000 (tighter staleness window).
  cfg.param_max_age_ticks = 1000;  // KEEP — registry INT(100000) too permissive; manual=1000 catches stale params sooner
  // v5.14.8.E — stale-model age check (boot-time gate). Default 0 = disabled.
  cfg.model_max_age_hours = 0;
  // v5.12.1.D — disabled by default (DEPRECATED v5.14.9.A; replaced by
  // risk_degradation_curve below; kept for back-compat parser shim).
  cfg.risk_scale_by_confidence = 0;
  // v5.14.9.A — soft risk degradation ladder defaults. OFF preserves
  // pre-v5.14.9 behavior bytewise. Threshold defaults match composite
  // confidence's practical scale [0.001, 0.3]: full at 0.15, min at 0.05,
  // 10% size floor at min threshold.
  cfg.risk_degradation_curve     = 0;  // CURVE_OFF
  cfg.barrier_blend_mode         = 0;  // MODE_BARRIER_BLEND_LEGACY (v5.15.5.A.5)
  cfg.risk_full_size_threshold   = FPN_FromDouble<F>(0.15);
  cfg.risk_min_size_threshold    = FPN_FromDouble<F>(0.05);
  cfg.risk_min_size_pct          = FPN_FromDouble<F>(0.10);
  // v5.14.1.B — composite confidence: disabled by default. Activates the
  // 4-factor formula (IC × Freshness × Capacity × Stability_normalized).
  // Defaults: 1 hour freshness decay, unbounded capacity, 1.0 rmse baseline.
  // confidence_composite_enabled migrated to ml_cfg_flags (default 0; stamp-bound via FOREACH_STAMP_BOUND_CFG emit_source=BITMAP_BIT)
  cfg.confidence_freshness_tau_secs       = FPN_FromDouble<F>(3600.0);
  cfg.confidence_capacity_target_dollars  = FPN_FromDouble<F>(0.0);
  cfg.confidence_capacity_kappa           = FPN_FromDouble<F>(0.1);
  cfg.confidence_rmse_baseline            = FPN_FromDouble<F>(1.0);
  // v5.14.1.D — winsor percentile defaults (0.5% / 99.5% — sensible for
  // clean BTC; noisier markets may want 1%/99% or 5%/95%).
  cfg.winsor_pct_low                      = FPN_FromDouble<F>(0.005);
  cfg.winsor_pct_high                     = FPN_FromDouble<F>(0.995);
  // v5.14.0 — Ridge blending defaults: disabled; opt-in for paper-test.
  // Default behavior bytewise-identical to v5.13.6 bandit selection path.
  // ridge_within_horizon / ridge_across_horizons migrated to ml_cfg_flags (v5.14.11.C; default 0)
  cfg.ridge_lambda          = FPN_FromDouble<F>(0.15);
  cfg.ridge_cost_penalty    = FPN_FromDouble<F>(0.5);
  cfg.ridge_min_ic_floor    = FPN_FromDouble<F>(0.001);
  // v5.12.2.B — disabled by default; activate after parity-check confirms
  // regime histogram unchanged within tolerance under enabled mode.
  // lazy_rebuild_enabled migrated to ml_cfg_flags (default 0)
  // v5.15.5.F.4d.1.B.3 Step 8.6: lazy_rebuild_force_period_us MATCH — registry INT(1000000) == manual; DELETED.
  cfg.lazy_rebuild_price_threshold_pct = FPN_FromDouble<F>(0.0005);  // 0.05%
  // v5.12.2.D — disabled by default; operator opts in after tooling is wired.
  // v5.15.5.F.4d.1.B.3 Step 8.6: use_aot_inference MATCH — registry BOOL(0) == manual; DELETED.
  // v5.13.0 — sell-side ML defaults: disabled; opt-in for paper-test.
  // use_exit_model migrated to ml_cfg_flags (default 0)
  cfg.exit_threshold = FPN_FromDouble<F>(0.6);
  cfg.exit_signal_model_dir[0] = '\0';
  cfg.calibration_log_path[0] = '\0';
  for (int i = 0; i < 16; ++i) cfg.core_model_path[i][0] = '\0';    // empty = shared
  for (int i = 0; i < 16; ++i) cfg.core_model_dir[i][0] = '\0';     // empty = use model_path or shared
  // WIP2d-1.A — per-core symbol forward-compat. Empty = no override; BinanceConfig.symbol
  // (loaded from binance.cfg) drives. Non-empty = operator-set per-core symbol; main.cpp
  // syncs to BinanceConfig.symbol pre-EngineSharded_Run with uniformity check.
  for (int i = 0; i < 16; ++i) cfg.core_symbol[i][0] = '\0';
  // v4.0 per-core overrides — zero in every field = "inherit global".
  // v4.7.24: zeroing auto-derives from PER_CORE_OVERRIDE_FIELDS macro.
  for (int i = 0; i < 16; ++i) {
#define _ZERO_OV_FIELD(name) cfg.core_overrides[i].name = FPN_Zero<F>();
    PER_CORE_OVERRIDE_FIELDS(_ZERO_OV_FIELD, _ZERO_OV_FIELD)
#undef _ZERO_OV_FIELD
// v4.7.40: zero INT overrides too (0 = inherit).
#define _ZERO_OV_INT_FIELD(name) cfg.core_overrides[i].name = 0;
    PER_CORE_OVERRIDE_INT_FIELDS(_ZERO_OV_INT_FIELD)
#undef _ZERO_OV_INT_FIELD
// v5.14.9.F.6: zero BITMAP overrides (0 = no overrides, full inherit).
#define _ZERO_OV_BITMAP_FIELDS(d_lower, D_UPPER, stype, FOREACH_macro) \
    cfg.core_overrides[i].d_lower##_cfg_flags_override = (stype)0;     \
    cfg.core_overrides[i].d_lower##_cfg_flags_override_set = (stype)0;
    PER_CORE_OVERRIDE_BITMAP_DOMAINS(_ZERO_OV_BITMAP_FIELDS)
#undef _ZERO_OV_BITMAP_FIELDS
  }
  cfg.simpledip_tp_pct  = FPN_Zero<F>();  // 0 = use shared take_profit_pct
  cfg.simpledip_sl_pct  = FPN_Zero<F>();
  cfg.mr_tp_pct         = FPN_Zero<F>();
  cfg.mr_sl_pct         = FPN_Zero<F>();
  cfg.emacross_tp_pct   = FPN_Zero<F>();
  cfg.emacross_sl_pct   = FPN_Zero<F>();
  // OMS phase 03 — mode 1: OMS owns portfolio mutation + per-core
  // accounting (via FillRecord drained post-Tick). Required for partials
  // (mode 0 used event.core_id directly as portfolio slot, which breaks
  // when slot != core_id under paired-leg geometry). Mode 0 left in
  // place for tests that explicitly want the legacy OnEvent path; new
  // production code paths default to 1.
  cfg.oms_event_log_mode = 1;

  // v5.15.5.C.3 Phase 7.A — bench gate flag (default OFF / production).
  cfg.oms_bench_enabled = 0;

  // v5.15.5.F.4c.3 Step 2 — populate per-core authoritative view from flat fields.
  // Step 3 will replace this with [core N] section parser writing cores[c] directly.
  ControllerConfig_PopulateCoresFromFlat(&cfg);

  return cfg;
}
//======================================================================================================
// [CONFIG NORMALIZE — v5.15.4]
//======================================================================================================
// Post-parse pass that applies mode-specific default tightening when the
// operator hasn't explicitly set a key. Honors explicit overrides via
// `cfg_keys_explicit` bitmap.
//
// Currently: `trading_mode=LIVE` flips two defaults toward stricter
// production semantics. Paper/shadow modes are passthrough.
//   - `model_verify_strict`: 0 (WARN) → 1 (STRICT)
//   - `reconcile_mode`:      1 (WARN) → 0 (STRICT)
//
// Stderr logs each auto-flip at boot so operators see what changed +
// why. Setting either key explicitly in cfg suppresses the flip (the
// bitmap bit is set by the parser at parse time; normalize checks it).
//
// Called from EngineSharded_Run AFTER ControllerConfig_Load + BEFORE
// LiveReadiness_Verify so the boot gate sees normalized values.
//
// Forward-compat: future mode-specific flip rules (e.g., AUTO_SYNC mode
// flipping kill switch thresholds, SHADOW mode skipping reconciliation
// entirely) get added here + a new MASK_CFG_KEY_* bit per tracked key.
template <unsigned F>
inline void ControllerConfig_NormalizeForMode(ControllerConfig<F>& cfg) {
    if (cfg.trading_mode != TRADING_MODE_LIVE) return;

    // Flip rule 1: model_verify_strict 0 (WARN) → 1 (STRICT)
    if (!(cfg.cfg_keys_explicit & MASK_CFG_KEY_MODEL_VERIFY_STRICT) &&
        cfg.model_verify_strict == 0) {
        cfg.model_verify_strict = 1;
        fprintf(stderr,
            "[live_normalize] trading_mode=live: model_verify_strict 0→1 "
            "(STRICT). Set explicitly in cfg to override.\n");
    }

    // Flip rule 2: reconcile_mode WARN (1) → STRICT (0)
    if (!(cfg.cfg_keys_explicit & MASK_CFG_KEY_RECONCILE_MODE) &&
        cfg.reconcile_mode == 1) {
        cfg.reconcile_mode = 0;
        // Mirror to legacy field per existing reconcile_mode parsing convention.
        cfg.reconcile_dry_run = 0;
        fprintf(stderr,
            "[live_normalize] trading_mode=live: reconcile_mode WARN→STRICT. "
            "Set explicitly in cfg to override.\n");
    }
}

//======================================================================================================
// [CONFIG PARSER]
//======================================================================================================
// simple key=value text file parser, no JSON, no external libs
// returns defaults if file is missing or unreadable
//======================================================================================================
template <unsigned F>
inline ControllerConfig<F> ControllerConfig_Load(const char *filepath) {
  ControllerConfig<F> cfg = ControllerConfig_Default<F>();

  // v5.9.0c — capture the cfg path so the engine header panel can display
  // it. Operators distinguish engine.cfg vs backtest.cfg load at-a-glance.
  if (filepath) {
    size_t n = strlen(filepath);
    if (n >= sizeof(cfg.source_cfg_path)) n = sizeof(cfg.source_cfg_path) - 1;
    memcpy(cfg.source_cfg_path, filepath, n);
    cfg.source_cfg_path[n] = '\0';
  }

  // Phase 8: track whether the user explicitly set maker/taker rates in
  // the cfg file. Can't infer from value comparison alone — explicit
  // values matching defaults would falsely trigger legacy-mirroring.
  int maker_explicitly_set = 0;
  int taker_explicitly_set = 0;

  FILE *f = fopen(filepath, "r");
  if (!f)
    return cfg;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    // strip \r\n
    int len = 0;
    while (line[len] && line[len] != '\n' && line[len] != '\r')
      len++;
    line[len] = '\0';

    // skip empty lines and comments
    if (len == 0 || line[0] == '#')
      continue;

    // find '='
    int eq_pos = -1;
    for (int i = 0; i < len; i++) {
      if (line[i] == '=') {
        eq_pos = i;
        break;
      }
    }
    if (eq_pos < 0)
      continue;

    // null-terminate key, value starts after '='
    line[eq_pos] = '\0';
    char *key = line;
    char *val = &line[eq_pos + 1];

    // v5.11.33 — strip inline `# comment` from value, then strip trailing
    // whitespace. Pre-fix the parser only stripped `\n`/`\r`, so a cfg line
    // like `xgb_tree_method=hist                # comment` left the value
    // as `"hist                # comment"`. For numeric fields atof/atoi
    // stop at the first non-numeric char (silently OK), but string fields
    // (xgb_tree_method, ml_model_path, etc.) used the literal string
    // verbatim — XGBoost's strcmp-match rejected `"hist           "` with
    // an `Invalid Input: 'hist           ', valid values are: {...}` error
    // that surfaced as WF train+val 0.0 across all folds (pred fails →
    // shape-mismatch SKIP at the WF accuracy compute). Operator-flagged
    // 2026-05-07; root-caused via v5.11.30/31/32 observability work.
    {
        // find first unescaped '#' on the value line (start scanning from
        // val[0]; the `=` already split key from val so the only `#`
        // we'd see is an inline comment marker)
        char *hash = strchr(val, '#');
        if (hash) *hash = '\0';
        // strip trailing whitespace (space, tab) — both cfg-style padding
        // and editor-auto-trim leftovers
        size_t vl = strlen(val);
        while (vl > 0 && (val[vl - 1] == ' ' || val[vl - 1] == '\t')) {
            val[--vl] = '\0';
        }
        // also strip leading whitespace (less common but cheap)
        while (*val == ' ' || *val == '\t') ++val;
    }

    //==================================================================================================
    // [v5.15.5.F.4c.3] REGISTRY-DRIVEN DISPATCH — runs FIRST; manual CFG_PARSE_* below are fallback
    //==================================================================================================
    // For each FOREACH_GLOBAL_CFG_FIELD + FOREACH_PER_CORE_CFG_FIELD entry: if
    // `key` matches the row's name, call tt::cfg_parse_field<T>(cfg.name, descriptor, val)
    // — T is deduced from cfg.name's actual type (FPN<F> or scalar). 3-barrier
    // structural fix per DOCS/RECURRING_BUG_PATTERNS.md Class 23 +
    // DESIGN_SPECS/type-trait-dispatch-via-tt-namespace.md.
    //
    // .F.4c.3 — registry split into two scope-disjoint registries (global vs
    // per-core). Parser walks BOTH; consumer field references remain at
    // cfg.<name> until Step 2 restructures ControllerConfig.hpp with
    // PerCoreCfg<F> cores[16]. Future ship .F.4c.3 Step 3 introduces [core N]
    // section parser; this walker fires only in the GLOBAL section.
    //
    // Manual CFG_PARSE_FPN/PCT/U32/INT/FPN_POS macros below stay in place for
    // fields NOT yet in either registry (KIND_STRING/_FILE_PATH migrate at
    // .F.4e). For fields IN the registries, the walk's `continue;` makes
    // the manual lines unreachable — cleanup deletion happens after build/test
    // verifies the migration works.
    //
    // Locale-immunity bonus: tt::cfg_parse_field uses parse_double_fast
    // (locale-independent) instead of manual macros' atof (LC_NUMERIC-honoring).
    // Closes pre-existing locale-dependence bug for migrated fields.
    //==================================================================================================
    // v5.15.5.F.4c — HAS_SIDE_EFFECT bit: registry walker skips parse; manual parser block
    // below handles the side-effect logic (e.g., fee_rate_maker/_taker explicit_set tracking,
    // risk_scale_by_confidence DEPRECATED shim translation, crypto-init pairs).
    #define EMIT_GLOBAL_CFG_PARSER_CASE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload_init, tooltip, \
                                         applies_to_strategy, applies_to_op_mode, \
                                         applies_to_regime, applies_to_risk, lives_in_struct) \
        if (strcmp(key, #name) == 0 && !((meta) & CfgFieldDescriptor::HAS_SIDE_EFFECT)) { \
            tt::cfg_parse_field(cfg.name, g_global_cfg_field_descriptors[FIELD_IDX_GLOBAL_##name], val); \
            continue; \
        }
    FOREACH_GLOBAL_CFG_FIELD(EMIT_GLOBAL_CFG_PARSER_CASE)
    #undef EMIT_GLOBAL_CFG_PARSER_CASE

    // WIP2d-1.B.0 — parser walker filters by MANUAL_PARSER (was HAS_SIDE_EFFECT alias). Rows with
    // MANUAL_PARSER have custom string-form parsing (e.g., bandit_algorithm "thompson"/"exp3";
    // fee_rate_maker explicit_set flag). Rows with NO_FLAT_FIELD (which lack cfg.name access) ALSO
    // get implicit skip via the same bit because per design, NO_FLAT_FIELD rows ALWAYS have
    // manual parsers (no auto-parse possible to a non-existent flat scalar). Strategy carries both
    // bits; this walker skip avoids cfg.strategy reference at instantiation.
    #define EMIT_PER_CORE_CFG_PARSER_CASE(STORAGE_T, KIND_TOKEN, name, label, section, meta, payload_init, tooltip, \
                                           applies_to_strategy, applies_to_op_mode, \
                                           applies_to_regime, applies_to_risk, lives_in_struct) \
        if constexpr (!((meta) & CfgFieldDescriptor::MANUAL_PARSER) && \
                      !((meta) & CfgFieldDescriptor::NO_FLAT_FIELD)) { \
            if (strcmp(key, #name) == 0) { \
                tt::cfg_parse_field(cfg.name, g_per_core_cfg_field_descriptors[FIELD_IDX_PER_CORE_##name], val); \
                continue; \
            } \
        }
    FOREACH_PER_CORE_CFG_FIELD(EMIT_PER_CORE_CFG_PARSER_CASE)
    #undef EMIT_PER_CORE_CFG_PARSER_CASE

// table-driven parser: FPN fields parsed as atof(val) directly
// adding a new field = add ONE line to the matching table below
#define CFG_PARSE_FPN(name)                                                    \
  if (strcmp(key, #name) == 0) {                                               \
    cfg.name = FPN_FromDouble<F>(atof(val));                                   \
    continue;                                                                  \
  }

// FPN fields parsed as atof(val) / 100.0 (percentage: config says 15.0, stored
// as 0.15)
#define CFG_PARSE_PCT(name)                                                    \
  if (strcmp(key, #name) == 0) {                                               \
    cfg.name = FPN_FromDouble<F>(atof(val) / 100.0);                           \
    continue;                                                                  \
  }

// uint32_t fields
#define CFG_PARSE_U32(name)                                                    \
  if (strcmp(key, #name) == 0) {                                               \
    cfg.name = (uint32_t)atol(val);                                            \
    continue;                                                                  \
  }

// int fields
#define CFG_PARSE_INT(name)                                                    \
  if (strcmp(key, #name) == 0) {                                               \
    cfg.name = atoi(val);                                                      \
    continue;                                                                  \
  }

// FPN fields with min-zero clamp
#define CFG_PARSE_FPN_POS(name)                                                \
  if (strcmp(key, #name) == 0) {                                               \
    double v = atof(val);                                                      \
    if (v < 0)                                                                 \
      v = 0;                                                                   \
    cfg.name = FPN_FromDouble<F>(v);                                           \
    continue;                                                                  \
  }

    //--- FPN raw (value used directly) ---
    CFG_PARSE_FPN(r2_threshold)
    CFG_PARSE_FPN(slope_scale_buy)
    CFG_PARSE_FPN(max_shift)
    CFG_PARSE_FPN(starting_balance)
    CFG_PARSE_FPN(volume_multiplier)
    CFG_PARSE_FPN(spacing_multiplier)
    CFG_PARSE_FPN(vol_mult_min)
    CFG_PARSE_FPN(vol_mult_max)
    CFG_PARSE_FPN(filter_scale)
    CFG_PARSE_FPN(min_long_slope)
    CFG_PARSE_FPN(min_buy_delta)
    CFG_PARSE_FPN(vwap_offset)
    CFG_PARSE_FPN(min_stddev_pct)
    CFG_PARSE_FPN(momentum_r2_min)
    CFG_PARSE_FPN(min_sl_tp_ratio)
    CFG_PARSE_FPN(ror_tp_bonus)
    CFG_PARSE_FPN(momentum_tp_r2_min)
    CFG_PARSE_FPN(momentum_sl_r2_max)
    CFG_PARSE_FPN(squeeze_decay)
    CFG_PARSE_FPN(offset_adapt_scale)
    CFG_PARSE_FPN(stddev_adapt_scale)
    CFG_PARSE_FPN(vol_adapt_scale)
    CFG_PARSE_FPN(breakout_min)
    CFG_PARSE_FPN(regime_slope_threshold)
    CFG_PARSE_FPN(regime_crossover_threshold)
    CFG_PARSE_FPN(regime_strong_crossover)
    CFG_PARSE_FPN(regime_volatile_stddev)
    CFG_PARSE_FPN(regime_vol_spike_ratio)
    CFG_PARSE_FPN(momentum_breakout_mult)
    CFG_PARSE_FPN(momentum_min_tp_margin_pct)     // v5.7.5
    CFG_PARSE_FPN(momentum_min_buy_delta_recent)  // v5.7.5
    CFG_PARSE_FPN(momentum_min_r2)                // v5.7.5
    // v5.15.5.F.4c — momentum_require_last_win migrated to FOREACH_CFG_FIELD (KIND_BOOL).
    CFG_PARSE_FPN(momentum_tp_mult)
    CFG_PARSE_FPN(momentum_sl_mult)
    CFG_PARSE_FPN(emacross_dip_mult)
    CFG_PARSE_FPN(emacross_crossover_min)
    CFG_PARSE_FPN(emacross_trail_mult)
    CFG_PARSE_FPN(spike_threshold)
    CFG_PARSE_FPN(spike_spacing_reduction)
    // v5.15.5.B.5 — session_*_mult parser entries auto-flowed via FOREACH_SESSION_PHASE.
#define X(NAME_U, name_l, START, END, MULT, DOC) CFG_PARSE_FPN(session_##name_l##_mult)
    FOREACH_SESSION_PHASE(X)
#undef X

    //--- FPN percentage (config says 15.0, stored as 0.15) ---
    CFG_PARSE_PCT(take_profit_pct)
    CFG_PARSE_PCT(stop_loss_pct)
    CFG_PARSE_PCT(fee_rate)
    // Phase 8: track explicit-set for the post-parse legacy-mirror decision.
    // Inline parse instead of CFG_PARSE_PCT macro (which would `continue;`
    // before setting the flag). Same divide-by-100 semantics.
    if (strcmp(key, "fee_rate_maker") == 0) {
        cfg.fee_rate_maker = FPN_FromDouble<F>(atof(val) / 100.0);
        maker_explicitly_set = 1;
        continue;
    }
    if (strcmp(key, "fee_rate_taker") == 0) {
        cfg.fee_rate_taker = FPN_FromDouble<F>(atof(val) / 100.0);
        taker_explicitly_set = 1;
        continue;
    }
    CFG_PARSE_PCT(risk_pct)
    CFG_PARSE_PCT(entry_offset_pct)
    CFG_PARSE_PCT(offset_min)
    CFG_PARSE_PCT(offset_max)
    CFG_PARSE_PCT(max_drawdown_pct)
    // Phase 3: kill switch tunables
    CFG_PARSE_FPN_POS(min_kill_loss)
    // v5.15.5.F.4c — enable_mtm_kill_switch migrated to FOREACH_CFG_FIELD (KIND_BOOL; uint32 storage).
    // v5.12.1.A — WS dead-time emergency-flatten (live-only safety net)
    // ws_dead_time_flatten_enabled migrated to risk_cfg_flags (v5.14.9.F.3)
    // v5.15.5.F.4c — ws_dead_time_flatten_threshold_secs migrated to FOREACH_CFG_FIELD (KIND_INT).
    // v5.12.1.A.3 — post-flatten recovery refusal window
    // v5.15.5.F.4c — recovery_delay_secs migrated to FOREACH_CFG_FIELD (KIND_INT).
    // v5.12.1.B.3 — hot-path parameter freshness gate
    // param_staleness_gate_enabled migrated to gate_cfg_flags — parser branch added in lifecycle block (v5.14.9.F.1)
    // param_max_age_ticks is uint64_t; can't use CFG_PARSE_INT (atoi returns int).
    // v5.15.5.F.4c — param_max_age_ticks + model_max_age_hours migrated to FOREACH_CFG_FIELD (KIND_INT).
    // Registry walker handles uint64/uint32 destination types via tt::cfg_parse_field<T> std::is_unsigned_v branch.
    // v5.12.1.D — confidence-conditional sizing infra (DEPRECATED v5.14.9.A;
    // back-compat parser shim translates to risk_degradation_curve below).
    if (strcmp(key, "risk_scale_by_confidence") == 0) {
      int legacy = atoi(val);
      cfg.risk_scale_by_confidence = legacy;       // legacy field stays (until cleanup ship)
      cfg.risk_degradation_curve   = legacy;       // legacy values 0/1/2 map 1:1 to OFF/LINEAR/EXP
      fprintf(stderr,
              "[cfg] WARN: risk_scale_by_confidence is deprecated v5.14.9; "
              "use risk_degradation_curve. Translating value %d → curve %s.\n",
              legacy, DegradationCurve_ToString(legacy));
      continue;
    }
    // v5.14.9.A — soft risk degradation ladder. Accepts numeric (0-3) or
    // string ("OFF"/"LINEAR"/"EXP"/"STEP", case-insensitive).
    if (strcmp(key, "risk_degradation_curve") == 0) {
      int parsed = DegradationCurve_FromString(val);
      if (parsed < 0) {
        fprintf(stderr, "[cfg] WARN: risk_degradation_curve='%s' invalid; "
                "expected one of OFF/LINEAR/EXP/STEP or 0-3. Using OFF.\n", val);
        cfg.risk_degradation_curve = 0;
      } else {
        cfg.risk_degradation_curve = parsed;
      }
      continue;
    }
    // v5.15.5.A.5 — per-horizon barrier blend mode (5-mode enum from
    // FOREACH_BARRIER_BLEND_MODE registry). Accepts numeric (0-4) or
    // case-insensitive string token (LEGACY / BLEND / DOMINANT /
    // BOTH_BLEND_DRIVES / BOTH_DOMINANT_DRIVES).
    if (strcmp(key, "barrier_blend_mode") == 0) {
      int parsed = BarrierBlendMode_FromString(val);
      if (parsed < 0 || parsed >= MODE_BARRIER_BLEND_COUNT) {
        fprintf(stderr, "[cfg] WARN: barrier_blend_mode='%s' invalid; "
                "expected one of LEGACY/BLEND/DOMINANT/BOTH_BLEND_DRIVES/"
                "BOTH_DOMINANT_DRIVES or 0-4. Using LEGACY.\n", val);
        cfg.barrier_blend_mode = 0;
      } else {
        cfg.barrier_blend_mode = parsed;
      }
      continue;
    }
    if (strcmp(key, "risk_full_size_threshold") == 0) {
      cfg.risk_full_size_threshold = FPN_FromDouble<F>(atof(val));
      continue;
    }
    if (strcmp(key, "risk_min_size_threshold") == 0) {
      cfg.risk_min_size_threshold = FPN_FromDouble<F>(atof(val));
      continue;
    }
    if (strcmp(key, "risk_min_size_pct") == 0) {
      cfg.risk_min_size_pct = FPN_FromDouble<F>(atof(val));
      continue;
    }
    // v5.14.1.B — composite confidence
    // confidence_composite_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
    if (strcmp(key, "confidence_freshness_tau_secs") == 0) {
      cfg.confidence_freshness_tau_secs = FPN_FromDouble<F>(atof(val));
      continue;
    }
    if (strcmp(key, "confidence_capacity_target_dollars") == 0) {
      cfg.confidence_capacity_target_dollars = FPN_FromDouble<F>(atof(val));
      continue;
    }
    if (strcmp(key, "confidence_capacity_kappa") == 0) {
      cfg.confidence_capacity_kappa = FPN_FromDouble<F>(atof(val));
      continue;
    }
    if (strcmp(key, "confidence_rmse_baseline") == 0) {
      cfg.confidence_rmse_baseline = FPN_FromDouble<F>(atof(val));
      continue;
    }
    // v5.14.1.D — feature winsorization (cfg-tunable percentiles)
    if (strcmp(key, "winsor_pct_low") == 0) {
      cfg.winsor_pct_low = FPN_FromDouble<F>(atof(val));
      continue;
    }
    if (strcmp(key, "winsor_pct_high") == 0) {
      cfg.winsor_pct_high = FPN_FromDouble<F>(atof(val));
      continue;
    }
    // v5.14.0 — Ridge risk-parity blending (cfg gates default off)
    // ridge_within_horizon / ridge_across_horizons migrated to ml_cfg_flags (v5.14.11.C)
    if (strcmp(key, "ridge_lambda") == 0) {
      cfg.ridge_lambda = FPN_FromDouble<F>(atof(val));
      continue;
    }
    if (strcmp(key, "ridge_cost_penalty") == 0) {
      cfg.ridge_cost_penalty = FPN_FromDouble<F>(atof(val));
      continue;
    }
    if (strcmp(key, "ridge_min_ic_floor") == 0) {
      cfg.ridge_min_ic_floor = FPN_FromDouble<F>(atof(val));
      continue;
    }
    // v5.12.2.B — lazy slow-path rebuild
    // lazy_rebuild_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
    // v5.15.5.F.4c — lazy_rebuild_force_period_us migrated to FOREACH_CFG_FIELD (KIND_INT; uint64 storage).
    // v5.15.5.F.4d TECH_DEBT-082 — lazy_rebuild_price_threshold_pct migrated to FOREACH_PER_CORE_CFG_FIELD (KIND_DOUBLE_PCT; FPN<F>; auto-flow parser via tt::cfg_*_field<T>). Class 23 manual-parser anti-pattern closure at this site.
    // v5.12.2.D — Treelite AOT backend opt-in (infrastructure-only)
    // v5.15.5.F.4c — use_aot_inference migrated to FOREACH_CFG_FIELD (KIND_BOOL; IS_BOOT_ONLY).
    // v5.13.0 — sell-side ML opt-in (Path 3 architecture)
    // use_exit_model migrated to ml_cfg_flags (v5.14.9.F.2)
    // v5.15.5.F.4d TECH_DEBT-082 — exit_threshold migrated to FOREACH_PER_CORE_CFG_FIELD (KIND_DOUBLE; FPN<F>; auto-flow parser via tt::cfg_*_field<T>). Class 23 manual-parser anti-pattern closure at this site.
    if (strcmp(key, "exit_signal_model_dir") == 0) {
      strncpy(cfg.exit_signal_model_dir, val,
              sizeof(cfg.exit_signal_model_dir) - 1);
      cfg.exit_signal_model_dir[sizeof(cfg.exit_signal_model_dir) - 1] = '\0';
      continue;
    }
    if (strcmp(key, "calibration_log_path") == 0) {
      strncpy(cfg.calibration_log_path, val,
              sizeof(cfg.calibration_log_path) - 1);
      cfg.calibration_log_path[sizeof(cfg.calibration_log_path) - 1] = '\0';
      continue;
    }
    CFG_PARSE_PCT(max_exposure_pct)
    CFG_PARSE_PCT(min_hold_gain_pct)
    CFG_PARSE_PCT(regime_r2_threshold)
    CFG_PARSE_PCT(slippage_pct)
    CFG_PARSE_PCT(kill_switch_daily_loss_pct)
    CFG_PARSE_PCT(kill_switch_drawdown_pct)
    CFG_PARSE_PCT(ml_tp_pct)
    CFG_PARSE_PCT(ml_sl_pct)

    //--- FPN with min-zero clamp ---
    CFG_PARSE_FPN_POS(offset_stddev_mult)
    CFG_PARSE_FPN_POS(offset_stddev_min)
    CFG_PARSE_FPN_POS(offset_stddev_max)
    CFG_PARSE_FPN_POS(tp_hold_score)
    CFG_PARSE_FPN_POS(tp_trail_mult)
    CFG_PARSE_FPN_POS(sl_trail_mult)
    // fee_floor_mult: min 1.0 (special case)
    if (strcmp(key, "fee_floor_mult") == 0) {
      double v = atof(val);
      if (v < 1)
        v = 1;
      cfg.fee_floor_mult = FPN_FromDouble<F>(v);
      continue;
    }

    //--- uint32_t ---
    // v5.15.5.F.4c — poll_interval migrated to FOREACH_CFG_FIELD (KIND_INT; uint32 storage).
    // v5.15.5.F.4c — pay_fees_in_bnb migrated to FOREACH_CFG_FIELD (KIND_BOOL; uint32 storage).
    // Was: CFG_PARSE_U32 // v4.3.2 Track C.1 — Binance BNB 25% fee discount
    // v5.15.5.F.4c — warmup_ticks migrated to FOREACH_CFG_FIELD (KIND_INT; IS_BOOT_ONLY).
    // v5.15.5.F.4c — slow_path_max_secs migrated to FOREACH_CFG_FIELD (KIND_INT; uint32 storage).
    // v5.15.5.F.4c — max_hold_ticks + regime_hysteresis + min_warmup_samples migrated to FOREACH_CFG_FIELD (KIND_INT).
    // v5.15.5.F.4c — csv_sort_check_mode migrated to FOREACH_CFG_FIELD (KIND_INT; STRICT/WARN/DISABLED; ships pending TECH_DEBT-068 ML enum registry).
    // v5.15.5.F.4c — idle_reset_cycles + sl_cooldown_cycles migrated to FOREACH_CFG_FIELD (KIND_INT).
    // v5.15.5.F.4c — sl_cooldown_base + sl_cooldown_extra migrated to FOREACH_CFG_FIELD (KIND_INT; uint32 storage).
    // v5.15.5.F.4c — kill_recovery_warmup migrated to FOREACH_CFG_FIELD (KIND_INT).
    // v5.15.5.F.4c — max_positions migrated to FOREACH_CFG_FIELD (KIND_INT; clamp [1, 16] in payload).
    // Registry walker's WARN_ON_CLAMP bit emits if operator wrote outside [1, 16].

    //--- int ---
    // v5.15.5.F.4c — sl_cooldown_adaptive migrated to FOREACH_CFG_FIELD (KIND_BOOL).
    // v5.14.9.F.4 — Parser auto-flow via FOREACH walks. Replaces 21 inline if-strcmp
    // branches (~130 lines) with 5 FOREACH walks (~40 lines). Adding a new bool flag
    // to ANY of the 5 domain registries = 1 row in the FOREACH macro; parser auto-flows
    // via this walk. Closes TECH_DEBT-009 boolean subset.
    //
    // Pattern: per-domain X macro expands to one if-strcmp branch; FOREACH_<DOMAIN>_CFG_FLAG
    // walks N entries. The `continue;` inside each branch breaks out of the parser's
    // outer for/while loop (NOT a do-while — that's why X is expanded inline, not wrapped).
    //
    // Each X is locally #define'd + #undef'd to avoid namespace pollution.

    // LIFECYCLE bitmap walk
    #define X(name, legacy_field, display_label, section, doc) \
      if (strcmp(key, #legacy_field) == 0) { \
        int _v = atoi(val); \
        if (_v) cfg.lifecycle_cfg_flags |=  MASK_LIFECYCLE_CFG_##name; \
        else    cfg.lifecycle_cfg_flags &= (uint8_t)~MASK_LIFECYCLE_CFG_##name; \
        continue; \
      }
    FOREACH_LIFECYCLE_CFG_FLAG(X)
    #undef X

    // GATE bitmap walk
    // v5.15.5.F.4d.1.B.3 Step 0.5d.a.0 — FOREACH_GATE_CFG_FLAG migrated to 6-arg sig per
    // Meta-gap M1b cohort migration discipline (sister to .B.2 FOREACH_ML_CFG_FLAG migration).
    #define X(name, legacy_field, display_label, section, metadata_flags, doc) \
      if (strcmp(key, #legacy_field) == 0) { \
        int _v = atoi(val); \
        if (_v) cfg.gate_cfg_flags |=  MASK_GATE_CFG_##name; \
        else    cfg.gate_cfg_flags &= (uint8_t)~MASK_GATE_CFG_##name; \
        continue; \
      }
    FOREACH_GATE_CFG_FLAG(X)
    #undef X

    // ML bitmap walk
    // v5.15.5.F.4d.1.B.2 — FOREACH_ML_CFG_FLAG migrated to 6-arg sig.
    #define X(name, legacy_field, display_label, section, metadata_flags, doc) \
      if (strcmp(key, #legacy_field) == 0) { \
        int _v = atoi(val); \
        if (_v) cfg.ml_cfg_flags |=  MASK_ML_CFG_##name; \
        else    cfg.ml_cfg_flags &= (uint16_t)~MASK_ML_CFG_##name; \
        continue; \
      }
    FOREACH_ML_CFG_FLAG(X)
    #undef X

    // RISK bitmap walk
    #define X(name, legacy_field, display_label, section, doc) \
      if (strcmp(key, #legacy_field) == 0) { \
        int _v = atoi(val); \
        if (_v) cfg.risk_cfg_flags |=  MASK_RISK_CFG_##name; \
        else    cfg.risk_cfg_flags &= (uint8_t)~MASK_RISK_CFG_##name; \
        continue; \
      }
    FOREACH_RISK_CFG_FLAG(X)
    #undef X

    // OPS bitmap walk
    #define X(name, legacy_field, display_label, section, doc) \
      if (strcmp(key, #legacy_field) == 0) { \
        int _v = atoi(val); \
        if (_v) cfg.ops_cfg_flags |=  MASK_OPS_CFG_##name; \
        else    cfg.ops_cfg_flags &= (uint8_t)~MASK_OPS_CFG_##name; \
        continue; \
      }
    FOREACH_OPS_CFG_FLAG(X)
    #undef X
    CFG_PARSE_PCT(breakeven_buffer_pct)
    // depth_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
    CFG_PARSE_INT(use_real_money)
    // v5.15.5.F.4c — acknowledge_hardcoded_strategy_in_live + require_mlockall +
    // init_arena_use_hugepages migrated to FOREACH_CFG_FIELD (KIND_BOOL; IS_BOOT_ONLY).
    // Registry walker at line 1896+ handles parse; manual CFG_PARSE_INT removed.
    // session_filter_enabled migrated to ops_cfg_flags (v5.14.9.F.3)
    // gate_ema_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
    // v5.15.5.F.4c — default_strategy migrated to FOREACH_CFG_FIELD (KIND_INT; IS_BOOT_ONLY; pending TECH_DEBT-068).
    // kill_switch_enabled migrated to risk_cfg_flags (v5.14.9.F.3)
    // vol_sizing_enabled migrated to risk_cfg_flags (v5.14.9.F.3)
    // no_trade_band_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
    // v5.15.5.F.4c — ml_backend + regime_model_backend migrated to FOREACH_CFG_FIELD (KIND_INT; IS_BOOT_ONLY; pending TECH_DEBT-068).

    //--- partial exit + depth + EMA FPN ---
    CFG_PARSE_FPN(partial_exit_pct)
    CFG_PARSE_FPN(tp2_mult)
    CFG_PARSE_FPN(min_book_imbalance)
    CFG_PARSE_FPN(vol_scale_min)
    CFG_PARSE_FPN(vol_scale_max)
    CFG_PARSE_FPN(no_trade_band_mult)
    CFG_PARSE_FPN(ml_buy_threshold)
    CFG_PARSE_FPN(regime_model_weight)
    // v5.15.5.F.4c — danger_enabled migrated to FOREACH_CFG_FIELD (KIND_BOOL).
    // Registry walker handles parse; manual CFG_PARSE_INT removed.
    CFG_PARSE_FPN(danger_warn_stddevs)
    CFG_PARSE_FPN(danger_crash_stddevs)

    //--- tick recording ---
    // v5.15.5.F.4c — record_ticks + record_depth migrated to FOREACH_CFG_FIELD (KIND_BOOL;
    // HIGH-6 tooltip byte-identity preserved). Registry walker handles parse.
    // v5.15.5.F.4c — record_max_days migrated to FOREACH_CFG_FIELD (KIND_INT; uint32 storage).

    //--- operational alerts (Phase 8b) ---
    // notify_enabled migrated to ops_cfg_flags (v5.14.9.F.3)
    // v5.15.5.F.4c — notify_backend + notify_cooldown_secs migrated to FOREACH_CFG_FIELD (KIND_INT; notify_backend pending TECH_DEBT-068).
    // notify_command is a string — no macro for that, inline parse below
    if (strcmp(key, "notify_command") == 0) {
        strncpy(cfg.notify_command, val, sizeof(cfg.notify_command) - 1);
        cfg.notify_command[sizeof(cfg.notify_command) - 1] = '\0';
        // strip trailing newline if any (the cfg parser usually does this,
        // but be defensive — bad commands break alerts silently otherwise)
        size_t nl = strlen(cfg.notify_command);
        while (nl > 0 && (cfg.notify_command[nl-1] == '\n' ||
                          cfg.notify_command[nl-1] == '\r')) {
            cfg.notify_command[--nl] = '\0';
        }
        continue;
    }

    //--- FoxML integration (Phase 6C) ---
    // cost_gate_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
    // foxml_vol_scaling_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
    CFG_PARSE_FPN(foxml_vol_scaling_z_max)
    // bandit_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
    CFG_PARSE_FPN(bandit_blend_ratio)
    // confidence_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
    // v5.15.5.F.4c — confidence_window migrated to FOREACH_CFG_FIELD (KIND_INT; HIGH-6 tooltip preserved).
    // v5.14.9.D — DELETED legacy `confidence_freshness_tau` parser branch
    // (TECH_DEBT-004 close). Cfg field deleted; legacy 3-factor formula
    // uses CONFIDENCE_FRESHNESS_TAU_DEFAULT (300.0) hardcoded constant.
    // Operator who has confidence_freshness_tau=N in their cfg file gets
    // unknown-key error at parse — clean signal to remove the line per
    // CHANGELOG migration note. Heavy-WIP stance accepted — no
    // tolerant parser shim.
    CFG_PARSE_FPN(confidence_threshold_scale)
    CFG_PARSE_FPN_POS(confidence_hard_block_threshold)
    // v5.15.5.F.4d.1.B.3 Phase F HIGH-1 (b) — held_out_fraction manual parser removed (was CFG_PARSE_FPN).
    // Registry auto-parser via FOREACH_GLOBAL_CFG_FIELD walker at line 2110 handles via tt::cfg_parse_field<FPN<F>>.
    // v5.15.5.F.4d.1.B.3 Step 1.6.1 — gap_acceptable_threshold migrated to registry auto-parser
    // (TECH_DEBT-093 closure). HAS_SIDE_EFFECT bit removed at CfgFieldRegistry.hpp registry row;
    // tt::cfg_parse_field<FPN<F>> handles FPN_FromDouble + clamp via DBL(0.05, 0.0, 1.0) payload.
    // v5.15.5.F.4c — allow_cross_major_engine migrated to FOREACH_CFG_FIELD (KIND_BOOL; IS_BOOT_ONLY).
    // Registry walker handles parse; manual CFG_PARSE_INT removed.
    // v5.9.5h — XGBoost hyperparam parsers
    CFG_PARSE_FPN_POS(xgb_subsample)
    CFG_PARSE_FPN_POS(xgb_colsample_bytree)
    // v5.15.5.F.4c — xgb_min_child_weight + xgb_seed migrated to FOREACH_CFG_FIELD (KIND_INT; HIGH-6 tooltips preserved; IS_BOOT_ONLY).
    if (strcmp(key, "xgb_tree_method") == 0) {
        size_t n = strlen(val);
        if (n >= sizeof(cfg.xgb_tree_method)) n = sizeof(cfg.xgb_tree_method) - 1;
        memcpy(cfg.xgb_tree_method, val, n);
        cfg.xgb_tree_method[n] = '\0';
        continue;
    }
    // v5.10.0 Item D — hardware-aware cfg parsers. CFG_PARSE_INT clamps
    // negatives to defaults; we want >=0 (0 = auto-detect via nproc, NOT
    // currently implemented; reserved for future). Setting nthread or
    // workers > 1 emits a one-shot WARN at boot (handled in engine boot
    // path, not parser).
    // v5.15.5.F.4c — xgb_*_nthread + csv_load_workers + multi_horizon_max_threads +
    // feature_collect_max_gb + wf_split_max_gb + held_out_max_gb all migrated to FOREACH_CFG_FIELD (KIND_INT; IS_BOOT_ONLY).
    // v5.10.0a.G.6 — global ensemble cfg parsers (string + numeric).
    if (strcmp(key, "ensemble_blend_mode") == 0) {
        // Validate against known modes; reject unknown with WARN.
        if (strcmp(val, "weighted") == 0 || strcmp(val, "selection") == 0) {
            strncpy(cfg.ensemble_blend_mode, val,
                    sizeof(cfg.ensemble_blend_mode) - 1);
            cfg.ensemble_blend_mode[sizeof(cfg.ensemble_blend_mode) - 1] = '\0';
        } else {
            fprintf(stderr, "[cfg] ensemble_blend_mode='%s' unknown; "
                    "valid: weighted|selection. Keeping default '%s'.\n",
                    val, cfg.ensemble_blend_mode);
        }
        continue;
    }
    if (strcmp(key, "ensemble_bandit_eta") == 0) {
        double v = atof(val);
        // Clamp to safe range; out-of-range silently produces uninformative
        // bandits (eta=0 = no learning; eta>1 = unstable).
        if (v < 0.01) v = 0.01;
        if (v > 1.0)  v = 1.0;
        cfg.ensemble_bandit_eta = v;
        continue;
    }
    // v5.15.5.F.4c — ensemble_min_warmup_predictions migrated to FOREACH_CFG_FIELD (KIND_INT).
    // v5.13.4 — sell-side bandit
    // exit_bandit_enabled migrated to ml_cfg_flags (v5.14.9.F.2)
    // v5.14.1.E — exit_blender_mode migrated to ml_cfg_flags (v5.14.11.C)
    // v5.14.1.F — IC variant selector
    // v5.15.5.F.4c — confidence_ic_variant migrated to FOREACH_CFG_FIELD (KIND_INT; ships pending TECH_DEBT-068).
    // v5.14.1.G — portfolio turnover diagnostic
    // v5.15.5.F.4c — confidence_turnover_window + confidence_turnover_topk migrated to FOREACH_CFG_FIELD (KIND_INT).
    if (strcmp(key, "exit_bandit_lr") == 0) {
        double v = atof(val);
        if (v < 0.01) v = 0.01;
        if (v > 1.0)  v = 1.0;
        cfg.exit_bandit_lr = v;
        continue;
    }
    if (strcmp(key, "ensemble_min_agreement_pct") == 0) {
        double v = atof(val);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        cfg.ensemble_min_agreement_pct = v;
        continue;
    }
    if (strcmp(key, "ensemble_trade_reward_mult") == 0) {
        double v = atof(val);
        // Clamp to sane range; 0 disables trade-close rewards entirely
        // (only slow-path lookback feeds bandit). Upper bound prevents
        // a single trade dominating thousands of slow-path signals.
        if (v < 0.0) v = 0.0;
        if (v > 100.0) v = 100.0;
        cfg.ensemble_trade_reward_mult = v;
        continue;
    }
    // v5.15.5.F.4c — ensemble_bandit_save_interval migrated to FOREACH_CFG_FIELD (KIND_INT; clamp [1, 1000000]).
    // Consumer-side semantic preserved: 0 = disable periodic saves (still saves on shutdown).
    // Registry walker's clamp_min=1 prevents 0; if operator NEEDS 0-disable semantic, restore via
    // sentinel handling in consumer code OR adjust clamp_min to 0 + register sentinel meaning.
    // v5.14.10.B — Bayesian Thompson sampling bandit cfg fields.
    // bandit_algorithm accepts numeric (0/1/2) or string (EXP3/THOMPSON/BOTH;
    // case-insensitive) via BanditAlgorithm_FromString. Out-of-range silently
    // clamps to EXP3 (preserves pre-v5.14.10 behavior on cfg corruption).
    if (strcmp(key, "bandit_algorithm") == 0) {
        int v = BanditAlgorithm_FromString(val);
        if (v < 0) {
            fprintf(stderr, "[CFG WARN] bandit_algorithm: unknown value '%s' (expected EXP3|THOMPSON|BOTH or 0|1|2); defaulting to EXP3\n", val);
            v = 0;
        }
        cfg.bandit_algorithm = v;
        continue;
    }
    CFG_PARSE_FPN(thompson_mu_prior)
    CFG_PARSE_FPN(thompson_precision_prior)
    CFG_PARSE_FPN(thompson_precision_obs)
    if (strcmp(key, "thompson_rng_seed") == 0) {
        // Accept hex (0x...) or decimal; 0 means "use ThompsonBandit.hpp default".
        uint64_t v = 0ULL;
        if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
            v = strtoull(val + 2, nullptr, 16);
        } else {
            v = strtoull(val, nullptr, 10);
        }
        cfg.thompson_rng_seed = v;
        continue;
    }
    // v5.10.0a — horizon_list CSV parser. Comma-separated ints, max
    // HORIZON_LIST_MAX entries. Caller can't use CFG_PARSE_INT (single
    // int) or CFG_PARSE_FPN. Custom branch.
    if (strcmp(key, "horizon_list") == 0) {
        int n = 0;
        const char* p = val;
        while (*p && n < ControllerConfig<F>::HORIZON_LIST_MAX) {
            // skip whitespace + commas
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            char* end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) break;  // parse failure
            if (v > 0 && v <= 1000000)  // sanity: 1 to 1M ticks
                cfg.horizon_list[n++] = (int)v;
            p = end;
        }
        cfg.horizon_count = n;
        if (n == 0) {
            fprintf(stderr, "[cfg] horizon_list='%s' parsed 0 valid horizons; "
                    "expected CSV like '100,500,1000'. Multi-horizon disabled.\n",
                    val);
        }
        continue;
    }
    // v5.15.5.F.4c — held_out_gate_strict migrated to FOREACH_CFG_FIELD (KIND_INT; tri-state clamp [-1, 1]).
    if (strcmp(key, "held_out_stamp_secret") == 0) {
        size_t n = strlen(val);
        if (n >= sizeof(cfg.held_out_stamp_secret)) n = sizeof(cfg.held_out_stamp_secret) - 1;
        memcpy(cfg.held_out_stamp_secret, val, n);
        cfg.held_out_stamp_secret[n] = '\0';
        continue;
    }
    if (strcmp(key, "auto_stamp_secret") == 0) {
        size_t vn = strnlen(val, sizeof(cfg.auto_stamp_secret) - 1);
        memcpy(cfg.auto_stamp_secret, val, vn);
        cfg.auto_stamp_secret[vn] = '\0';
        continue;
    }
    // v5.15.5.F.4c — auto_stamp_on_held_out migrated to FOREACH_CFG_FIELD (KIND_BOOL).
    // Was: `else if (strcmp(key, "auto_stamp_on_held_out") == 0)` — registry walker handles parse.
    if (strcmp(key, "health_log_path") == 0) {
        size_t n = strlen(val);
        if (n >= sizeof(cfg.health_log_path)) n = sizeof(cfg.health_log_path) - 1;
        memcpy(cfg.health_log_path, val, n);
        cfg.health_log_path[n] = '\0';
        continue;
    }
    // v5.15.5.F.4c — health_log_level + health_log_max_bytes + health_log_keep_count migrated to FOREACH_CFG_FIELD (KIND_INT).
    // Registry walker's clamp_min=1 for keep_count replaces the manual `< 0 → 0` clamp (slight semantic change: 0 input now warns + clamps to 1; previously silently to 0).
    // v5.15.5.A.7 — acknowledge_cross_binary_version_drift + acknowledge_inference_cfg_drift
    // CFG_PARSE_INT removed; legacy cfg keys auto-route via FOREACH_OPS_CFG_FLAG walker
    // (legacy_field column matches keys at the ops_cfg_flags parser block line ~2220).
    // v5.15.5.F.4c — acknowledge_hot_swap_with_open_positions migrated to FOREACH_CFG_FIELD (KIND_BOOL).
    // Registry walker handles parse; manual CFG_PARSE_INT removed.
    // v5.15.5.F.4d TECH_DEBT-082 — confidence_ic_floor migrated to FOREACH_PER_CORE_CFG_FIELD (KIND_DOUBLE; double; auto-flow parser via tt::cfg_*_field<T>). Class 23 manual-parser anti-pattern closure at this site.
    // v5.15.5.F.4c — confidence_ic_floor_window migrated to FOREACH_CFG_FIELD (KIND_INT; uint32 storage).
    if (false) {
        cfg.confidence_ic_floor_window = (uint32_t)strtoul(val, nullptr, 10);
        continue;
    }
    CFG_PARSE_INT(auto_kill_on_drift)  // v5.10.0e
    // v5.15.5.F.4c — reconcile_interval_sec migrated to FOREACH_CFG_FIELD (KIND_INT).
    if (strcmp(key, "reconcile_dry_run") == 0) {
        // v5.14.4 back-compat: legacy operator cfgs may have this field.
        // Translate to reconcile_mode (1 → WARN; 0 → STRICT). Also keep
        // the legacy field set so any downstream code reading
        // reconcile_dry_run still works during the transition window.
        int dry_run = atoi(val);
        cfg.reconcile_dry_run = dry_run;
        cfg.reconcile_mode    = dry_run ? 1 /*WARN*/ : 0 /*STRICT*/;
        // v5.15.4 — back-compat field is an explicit choice from operator.
        cfg.cfg_keys_explicit |= MASK_CFG_KEY_RECONCILE_MODE;
        continue;
    }
    if (strcmp(key, "reconcile_mode") == 0) {
        // v5.14.4 — accepts string ("strict"/"warn"/"auto_sync") OR
        // numeric (0/1/2). String form is operator-friendly + matches
        // FOREACH_RECONCILE_MODE registry; numeric is for back-compat
        // and tests. Falls back to numeric atoi if string match fails.
        // (NOTE: ReconcileMode_FromString lives in Reconcile.hpp; we
        // duplicate the small string switch here to keep ControllerConfig
        // header free of Reconcile.hpp dependency. If FOREACH_RECONCILE_MODE
        // gains many entries, refactor to include Reconcile.hpp here.)
        if      (strcmp(val, "strict")    == 0) cfg.reconcile_mode = 0;
        else if (strcmp(val, "warn")      == 0) cfg.reconcile_mode = 1;
        else if (strcmp(val, "auto_sync") == 0) cfg.reconcile_mode = 2;
        else                                     cfg.reconcile_mode = (uint8_t)atoi(val);
        // Mirror to legacy field for code still reading reconcile_dry_run
        // (will be removed when transition window closes).
        cfg.reconcile_dry_run = (cfg.reconcile_mode == 0) ? 0 : 1;
        // v5.15.4 — track explicit parse so NormalizeForMode honors it.
        cfg.cfg_keys_explicit |= MASK_CFG_KEY_RECONCILE_MODE;
        continue;
    }
    if (strcmp(key, "trading_mode") == 0) {
        // v5.15.2 — accepts string ("paper"/"live"/"shadow") OR numeric
        // (0/1/2). String form is operator-friendly (cfg files);
        // numeric is for back-compat + tests. Falls back to atoi on
        // unknown string (preserves "unset" → 0=PAPER behavior).
        if      (strcmp(val, "paper")  == 0) cfg.trading_mode = TRADING_MODE_PAPER;
        else if (strcmp(val, "live")   == 0) cfg.trading_mode = TRADING_MODE_LIVE;
        else if (strcmp(val, "shadow") == 0) cfg.trading_mode = TRADING_MODE_SHADOW;
        else                                  cfg.trading_mode = (uint8_t)atoi(val);
        continue;
    }
    CFG_PARSE_INT(prediction_normalize)
    // barrier_gate_enabled migrated to gate_cfg_flags (v5.14.9.F.1)
    // v5.15.4 — model_verify_strict needs explicit-tracking for the
    // NormalizeForMode flip rule, so inline parse instead of CFG_PARSE_INT
    // (which doesn't have an injection point for the bitmap OR).
    if (strcmp(key, "model_verify_strict") == 0) {
        cfg.model_verify_strict = atoi(val);
        cfg.cfg_keys_explicit  |= MASK_CFG_KEY_MODEL_VERIFY_STRICT;
        continue;
    }

    // Per-core sharding (Phase 13) — engine_mode accepts both string and int
    // forms. The GUI SettingsPanel uses CFG_BOOL which writes "0"/"1"; manual
    // edits to engine.cfg can use "single_core"/"sharded" for clarity.
    if (strcmp(key, "engine_mode") == 0) {
      if (strcmp(val, "sharded") == 0 || strcmp(val, "1") == 0)
        cfg.engine_mode = ENGINE_MODE_SHARDED;
      else
        cfg.engine_mode = ENGINE_MODE_SINGLE_CORE;
      continue;
    }
    // v5.15.5.F.4c — slow_path_pin_offset + num_execution_cores migrated to FOREACH_CFG_FIELD (KIND_INT).
    // num_execution_cores clamp [1, 16] preserved in INT(1, 1, 16) payload.
    // v5.15.5.F.4c — sharded_force_synthetic migrated to FOREACH_CFG_FIELD (KIND_BOOL; uint8_t storage).
    // Registry walker's KIND_BOOL branch handles truthy-int normalization.
    // v5.12.3.C — per-core time-exit override: core_0_time_exit_ticks=5000
    // means core 0 forces exit after 5000 ticks held (overrides global
    // cfg.max_hold_ticks for this core). Match BEFORE generic _exit_*
    // patterns to avoid substring collisions.
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_time_exit_ticks")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        cfg.core_time_exit_ticks[core_idx] = (uint32_t)atol(val);
      }
      continue;
    }
    // Per-core risk: core_0_risk_pct=20.0 means core 0 risks 20% of balance
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_risk_pct")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        cfg.core_risk_pct[core_idx] = FPN_FromDouble<F>(atof(val) / 100.0);
      }
      continue;
    }
    // Phase 3: per-core kill switch override. core_0_max_drawdown_pct=15.0
    // means core 0 trips at 15% drawdown (overrides shared max_drawdown_pct).
    // Match must come before _max checks (substring "_max" is in
    // "_max_drawdown_pct"). Specific suffix match keeps it safe.
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_max_drawdown_pct")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        cfg.core_max_drawdown_pct[core_idx] = FPN_FromDouble<F>(atof(val) / 100.0);
      }
      continue;
    }
    // Per-core model path: core_0_model_path=models/aggressive.xgb
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_model_path")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        strncpy(cfg.core_model_path[core_idx], val,
                sizeof(cfg.core_model_path[core_idx]) - 1);
        cfg.core_model_path[core_idx][sizeof(cfg.core_model_path[core_idx]) - 1] = '\0';
      }
      continue;
    }
    // WIP2d-1.A — Per-core symbol: core_0_symbol=BTCUSDT
    // Forward-compat for multi-symbol DataStream. Uniformity-checked at boot (main.cpp);
    // bridges to BinanceConfig.symbol if operator set a non-empty value.
    // strstr "_symbol" comes BEFORE _strategy block (avoid match-substring shadowing).
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_symbol")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        strncpy(cfg.core_symbol[core_idx], val,
                sizeof(cfg.core_symbol[core_idx]) - 1);
        cfg.core_symbol[core_idx][sizeof(cfg.core_symbol[core_idx]) - 1] = '\0';
      }
      continue;
    }
    // Per-core model dir: core_0_model_dir=models/aggressive/
    // when set, engine auto-discovers role-specific models in the directory
    // (barrier.json, buy_signal.json, regime.json, exit.json) and loads each
    // present file into the per-core CoreModelZoo.
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_model_dir")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        strncpy(cfg.core_model_dir[core_idx], val,
                sizeof(cfg.core_model_dir[core_idx]) - 1);
        cfg.core_model_dir[core_idx][sizeof(cfg.core_model_dir[core_idx]) - 1] = '\0';
      }
      continue;
    }
    // Per-core strategy: core_0_strategy=simple_dip, core_1_strategy=none, etc.
    if (strncmp(key, "core_", 5) == 0 && strstr(key, "_strategy")) {
      int core_idx = atoi(key + 5);
      if (core_idx >= 0 && core_idx < 16) {
        uint8_t sid = 0xFF; // STRATEGY_NONE
        if (strcmp(val, "mr") == 0 || strcmp(val, "mean_reversion") == 0) sid = 0;
        else if (strcmp(val, "momentum") == 0 || strcmp(val, "mom") == 0) sid = 1;
        else if (strcmp(val, "simple_dip") == 0 || strcmp(val, "dip") == 0) sid = 2;
        else if (strcmp(val, "ml") == 0) sid = 3;
        else if (strcmp(val, "ema_cross") == 0 || strcmp(val, "ema") == 0) sid = 4;
        else if (strcmp(val, "auto") == 0 || strcmp(val, "regime") == 0) sid = 5;  // v4.0.3 STRATEGY_AUTO
        else if (strcmp(val, "none") == 0) sid = 0xFF;
        else sid = (uint8_t)atoi(val);  // numeric fallback
        cfg.core_strategies[core_idx] = sid;
        // v5.9.0c — set explicit bit so TUI can distinguish deliberate from defaulted.
        cfg.core_strategies_explicit_set |= (uint16_t)(1u << core_idx);
      }
      continue;
    }
    // Per-core overrides (v4.0). Parses `core_N_<field>=<value>` for any
    // PerCoreOverrides field. Empty/0 = inherit global; resolver handles
    // the fallback. Two categories — pct (atof/100, e.g. take_profit_pct)
    // and raw FPN (atof, e.g. ml_buy_threshold).
    //
    // v4.7.24: parser auto-derives from PER_CORE_OVERRIDE_FIELDS macro.
    // Adding a new override field = ONE line in the macro list near the
    // top of this header. The struct, init, resolver, and parser all
    // pick up the new field automatically.
    if (strncmp(key, "core_", 5) == 0) {
      int core_idx = -1;
      const char* suffix = nullptr;
      // parse core_N_<suffix>
      const char* p = key + 5;
      core_idx = atoi(p);
      while (*p && *p != '_') p++;
      if (*p == '_' && core_idx >= 0 && core_idx < 16) {
        suffix = p + 1;
        PerCoreOverrides<F>& ov = cfg.core_overrides[core_idx];
#define _PARSE_OV_PCT(name) if (strcmp(suffix, #name) == 0) { ov.name = FPN_FromDouble<F>(atof(val)/100.0); continue; }
#define _PARSE_OV_RAW(name) if (strcmp(suffix, #name) == 0) { ov.name = FPN_FromDouble<F>(atof(val));       continue; }
        PER_CORE_OVERRIDE_FIELDS(_PARSE_OV_PCT, _PARSE_OV_RAW)
#undef _PARSE_OV_PCT
#undef _PARSE_OV_RAW
// v4.7.40: INT overrides — atoi parse, 0 = inherit.
#define _PARSE_OV_INT(name) if (strcmp(suffix, #name) == 0) { ov.name = (uint32_t)atoi(val); continue; }
        PER_CORE_OVERRIDE_INT_FIELDS(_PARSE_OV_INT)
#undef _PARSE_OV_INT
// v5.14.9.F.6: BITMAP per-bit overrides. `core_N_<legacy_field> = X` sets the
// corresponding bit on <domain>_cfg_flags_override + marks it in
// <domain>_cfg_flags_override_set so the resolver knows to use the override
// for that specific bit. Other bits in the domain inherit from global.
//
// Walks the 5 FOREACH_<DOMAIN>_CFG_FLAG registries; per-entry checks if suffix
// matches legacy_field; on match: set/clear override bit + mark override_set bit.
// Auto-flows: adding a new flag to ANY domain registry = parser picks it up.
#define _PARSE_OV_BITMAP_DOMAIN(d_lower, D_UPPER, stype, FOREACH_macro) \
    {                                                                          \
        int _val_b = -1;                                                       \
        const char* _legacy_match = nullptr;                                   \
        (void)_legacy_match;                                                   \
        stype _mask_b = (stype)0;                                              \
        (void)_mask_b;                                                         \
        FOREACH_macro(_PARSE_OV_BITMAP_ROW_##d_lower)                          \
        if (_val_b >= 0) {                                                     \
            if (_val_b) ov.d_lower##_cfg_flags_override |= _mask_b;            \
            else        ov.d_lower##_cfg_flags_override &= (stype)~_mask_b;    \
            ov.d_lower##_cfg_flags_override_set |= _mask_b;                    \
            continue;                                                          \
        }                                                                       \
    }
// Per-domain per-row macros: capture mask + val into outer scope when matched
#define _PARSE_OV_BITMAP_ROW_lifecycle(name, legacy_field, dl, sec, doc) \
    if (strcmp(suffix, #legacy_field) == 0) { _val_b = atoi(val); _mask_b = MASK_LIFECYCLE_CFG_##name; }
// v5.15.5.F.4d.1.B.3 Step 0.5d.a.0 — _gate variant takes 6 args (FOREACH_GATE_CFG_FLAG 5→6 sig
// migration per Meta-gap M1b cohort discipline; sister to .B.2 _ml migration).
#define _PARSE_OV_BITMAP_ROW_gate(name, legacy_field, dl, sec, metadata_flags, doc) \
    if (strcmp(suffix, #legacy_field) == 0) { _val_b = atoi(val); _mask_b = MASK_GATE_CFG_##name; }
// v5.15.5.F.4d.1.B.2 — _ml variant takes 6 args (FOREACH_ML_CFG_FLAG 5→6 sig migration).
// Remaining 3 _PARSE_OV_BITMAP_ROW_* domains (lifecycle/risk/ops) stay 5-arg per Meta-gap M1b
// § DEFER with explicit rationale — no STAMP_BOUND-eligible consumer at this ship.
#define _PARSE_OV_BITMAP_ROW_ml(name, legacy_field, dl, sec, metadata_flags, doc) \
    if (strcmp(suffix, #legacy_field) == 0) { _val_b = atoi(val); _mask_b = MASK_ML_CFG_##name; }
#define _PARSE_OV_BITMAP_ROW_risk(name, legacy_field, dl, sec, doc) \
    if (strcmp(suffix, #legacy_field) == 0) { _val_b = atoi(val); _mask_b = MASK_RISK_CFG_##name; }
#define _PARSE_OV_BITMAP_ROW_ops(name, legacy_field, dl, sec, doc) \
    if (strcmp(suffix, #legacy_field) == 0) { _val_b = atoi(val); _mask_b = MASK_OPS_CFG_##name; }
        PER_CORE_OVERRIDE_BITMAP_DOMAINS(_PARSE_OV_BITMAP_DOMAIN)
#undef _PARSE_OV_BITMAP_DOMAIN
#undef _PARSE_OV_BITMAP_ROW_lifecycle
#undef _PARSE_OV_BITMAP_ROW_gate
#undef _PARSE_OV_BITMAP_ROW_ml
#undef _PARSE_OV_BITMAP_ROW_risk
#undef _PARSE_OV_BITMAP_ROW_ops
        // v5.10.0a.G.6 — string-typed per-core ensemble fields. X-macro
        // doesn't support string types; explicit branches here. All three
        // default empty (inherit global).
        if (strcmp(suffix, "horizon_list") == 0) {
            strncpy(cfg.core_horizon_list[core_idx], val,
                    sizeof(cfg.core_horizon_list[core_idx]) - 1);
            cfg.core_horizon_list[core_idx][
                sizeof(cfg.core_horizon_list[core_idx]) - 1] = '\0';
            continue;
        }
        if (strcmp(suffix, "ensemble_blend_mode") == 0) {
            if (strcmp(val, "weighted") == 0 || strcmp(val, "selection") == 0) {
                strncpy(cfg.core_ensemble_blend_mode[core_idx], val,
                        sizeof(cfg.core_ensemble_blend_mode[core_idx]) - 1);
                cfg.core_ensemble_blend_mode[core_idx][
                    sizeof(cfg.core_ensemble_blend_mode[core_idx]) - 1] = '\0';
            } else {
                fprintf(stderr, "[cfg] core_%d_ensemble_blend_mode='%s' unknown; "
                        "valid: weighted|selection. Falling back to global.\n",
                        core_idx, val);
            }
            continue;
        }
        if (strcmp(suffix, "disabled_horizons") == 0) {
            strncpy(cfg.core_disabled_horizons[core_idx], val,
                    sizeof(cfg.core_disabled_horizons[core_idx]) - 1);
            cfg.core_disabled_horizons[core_idx][
                sizeof(cfg.core_disabled_horizons[core_idx]) - 1] = '\0';
            continue;
        }
        // v5.11.18a — per-core feature_mask (uint64_t, hex or decimal).
        // Accepts `0xDEADBEEFCAFEBABE` (any case), `0XDEADBEEF`, or plain
        // decimal `18446744073709551615`. Out-of-range / unparseable
        // values fall back to default 0xFFFF..F (all features enabled)
        // with a WARN — never silently zero-out the mask, which would
        // disable ALL features for that core.
        if (strcmp(suffix, "feature_mask") == 0) {
            char* end = nullptr;
            errno = 0;
            uint64_t parsed;
            if ((val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))) {
                parsed = strtoull(val + 2, &end, 16);
            } else {
                parsed = strtoull(val, &end, 10);
            }
            if (end == val || (end != nullptr && *end != '\0' && *end != '\n')
                    || errno == ERANGE) {
                fprintf(stderr, "[cfg] core_%d_feature_mask='%s' unparseable; "
                        "expected 0xHEX or decimal. Falling back to all-on "
                        "(0xFFFFFFFFFFFFFFFF).\n", core_idx, val);
                cfg.core_feature_mask[core_idx] = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                cfg.core_feature_mask[core_idx] = parsed;
                if (parsed == 0ULL) {
                    fprintf(stderr, "[cfg] WARN: core_%d_feature_mask=0x0 "
                            "disables ALL features for this core. Did you "
                            "mean 0xFFFFFFFFFFFFFFFF (all enabled)?\n",
                            core_idx);
                }
            }
            continue;
        }
      }
    }
    // Per-strategy TP/SL overrides (percentage, parsed with /100)
    CFG_PARSE_PCT(simpledip_tp_pct)
    CFG_PARSE_PCT(simpledip_sl_pct)
    CFG_PARSE_PCT(mr_tp_pct)
    CFG_PARSE_PCT(mr_sl_pct)
    CFG_PARSE_PCT(emacross_tp_pct)
    CFG_PARSE_PCT(emacross_sl_pct)
    // OMS phase 03 — accept both string and int values for clarity in cfg files
    if (strcmp(key, "oms_event_log_mode") == 0) {
      if (strcmp(val, "legacy") == 0)
        cfg.oms_event_log_mode = 0;
      else if (strcmp(val, "event_log") == 0)
        cfg.oms_event_log_mode = 1;
      else
        cfg.oms_event_log_mode = (uint32_t)atoi(val);
      continue;
    }
    // v5.15.5.C.3 Phase 7.A — runtime bench gate substrate flag.
    // Phase 7.B integration pending; flag has no observable effect today.
    if (strcmp(key, "oms_bench_enabled") == 0) {
      cfg.oms_bench_enabled = (uint32_t)atoi(val);
      continue;
    }

    // ML model paths (string fields — not atof)
    if (strcmp(key, "ml_model_path") == 0) {
      strncpy(cfg.ml_model_path, val, sizeof(cfg.ml_model_path) - 1);
      cfg.ml_model_path[sizeof(cfg.ml_model_path) - 1] = '\0';
      continue;
    }
    if (strcmp(key, "regime_model_path") == 0) {
      strncpy(cfg.regime_model_path, val, sizeof(cfg.regime_model_path) - 1);
      cfg.regime_model_path[sizeof(cfg.regime_model_path) - 1] = '\0';
      continue;
    }
    if (strcmp(key, "peak_model_path") == 0) {
      strncpy(cfg.peak_model_path, val, sizeof(cfg.peak_model_path) - 1);
      cfg.peak_model_path[sizeof(cfg.peak_model_path) - 1] = '\0';
      continue;
    }
    if (strcmp(key, "valley_model_path") == 0) {
      strncpy(cfg.valley_model_path, val, sizeof(cfg.valley_model_path) - 1);
      cfg.valley_model_path[sizeof(cfg.valley_model_path) - 1] = '\0';
      continue;
    }

    // EMA alpha: parse alpha and precompute 1-alpha
    if (strcmp(key, "gate_ema_alpha") == 0) {
      double a = atof(val);
      cfg.gate_ema_alpha = FPN_FromDouble<F>(a);
      cfg.gate_ema_one_minus_alpha = FPN_FromDouble<F>(1.0 - a);
      continue;
    }

#undef CFG_PARSE_FPN
#undef CFG_PARSE_PCT
#undef CFG_PARSE_U32
#undef CFG_PARSE_INT
#undef CFG_PARSE_FPN_POS
  }

  fclose(f);

  // v5.9.0c — surface "all per-core strategies defaulted" silent failure.
  // V5_9_AUDIT-#5. The today-bug: backtest.cfg lacked core_N_strategy=
  // lines → all 16 cores fell to STRATEGY_SIMPLE_DIP default → operator
  // saw "0!" hardcoded warnings, couldn't distinguish defaulted from
  // deliberate. Stderr WARN at boot makes the silent fallback visible.
  //
  // Fires only when num_execution_cores > 0 (we have actual cores) AND
  // explicit_set bitmap is zero (no core_N_strategy= lines parsed).
  if (cfg.num_execution_cores > 0 && cfg.core_strategies_explicit_set == 0) {
    fprintf(stderr,
        "[cfg] WARN: %s has no `core_N_strategy=` lines. "
        "All %u cores defaulting to SIMPLE_DIP (per ControllerConfig_Default). "
        "If this is unintended (e.g., you copied backtest.cfg without "
        "the per-core fields), add `core_0_strategy=mr` etc. to the cfg.\n",
        filepath ? filepath : "(unknown cfg)",
        (unsigned)cfg.num_execution_cores);
  }

  // Phase 8 — backward-compat for fee_rate_maker / fee_rate_taker.
  //
  // Three valid cfg shapes:
  //   1. Only fee_rate set (legacy): mirror to both maker + taker.
  //      Live engine effectively becomes "all-fee_rate" — same as pre-Phase-8.
  //   2. fee_rate_maker + fee_rate_taker set explicitly: live engine uses
  //      per-fill rates based on order->is_maker.
  //   3. Mixed: fee_rate set AND exactly ONE of maker/taker explicitly set.
  //      The other stays at its DEFAULT, which is almost certainly wrong —
  //      WARN loudly so the user can fix.
  //
  // Use the explicit-set flags tracked during parse (above). This handles
  // the case where the user explicitly sets maker/taker to values that
  // happen to equal Default() — value-comparison can't distinguish.
  {
    int legacy_set = !FPN_IsZero(cfg.fee_rate);

    if (!maker_explicitly_set && !taker_explicitly_set && legacy_set) {
      // Legacy mode: only fee_rate set in cfg, mirror to both.
      cfg.fee_rate_maker = cfg.fee_rate;
      cfg.fee_rate_taker = cfg.fee_rate;
      fprintf(stderr,
              "[CFG] fee_rate=%.5f → mirrored to maker+taker (legacy mode)\n",
              FPN_ToDouble(cfg.fee_rate));
    } else if (legacy_set && (maker_explicitly_set ^ taker_explicitly_set)) {
      // Mixed-cfg WARNING — almost certainly user error.
      fprintf(stderr,
              "[CFG] WARNING: fee_rate=%.5f set, but only one of "
              "fee_rate_maker (%.5f) / fee_rate_taker (%.5f) explicitly set. "
              "The other stayed at its default. If you meant to set both, "
              "set both explicitly. If you meant legacy mode, remove the "
              "explicitly-set one.\n",
              FPN_ToDouble(cfg.fee_rate),
              FPN_ToDouble(cfg.fee_rate_maker),
              FPN_ToDouble(cfg.fee_rate_taker));
    }
    // else: both maker+taker set explicitly (case 2 — silent, working as
    // intended) OR neither set + no legacy fee_rate (zero everywhere, fine).
  }

  // post-load validation/clamping. min_warmup_samples gates on rolling.count
  // which caps at the short rolling window size (W=128). Values above 128
  // mean "warmup never completes" — user-hostile silent failure. Clamp +
  // explain so the user understands what happened and what to use instead.
  // (Took us multiple hours of debugging Friday night before we figured this
  // out — the field name implied "ticks" but actually means "rolling window
  // samples." See CLAUDE.md "Label-type-aware metric invariant" for the
  // sibling rule about consulting source-of-truth helpers.)
  const uint32_t ROLLING_WINDOW_SHORT = 128; // matches RollingStats<F> default W
  if (cfg.min_warmup_samples > ROLLING_WINDOW_SHORT) {
    fprintf(stderr,
            "[CFG] WARNING: min_warmup_samples=%u exceeds rolling window size "
            "%u and would cause warmup to never complete. Clamped to %u.\n"
            "      If you want a longer total-tick warmup, use warmup_ticks "
            "instead (counts raw ticks, no upper bound).\n",
            cfg.min_warmup_samples, ROLLING_WINDOW_SHORT, ROLLING_WINDOW_SHORT);
    cfg.min_warmup_samples = ROLLING_WINDOW_SHORT;
  }

  // v5.15.5.F.4d.1.B.2 Step 6 — Winsor cross-field invariant validation.
  // Individual bounds (low ∈ [0.0, 0.5], high ∈ [0.5, 1.0]) already enforced by
  // WARN_ON_CLAMP at tt::cfg_parse_field<T> clamp dispatch (CoreFrameworks/CfgFieldDispatch.hpp).
  // Cross-field invariant low < high adds the missing piece per CRIT-4 audit finding.
  // Decision 4 (A) at triage: cross-field invariant only; leverage existing canonical
  // (avoids duplicating individual-bound clamps that WARN_ON_CLAMP already covers).
  // FPN<F>-native compare per MED-4 audit recommendation (now possible at .B.2 Step 6.5
  // operator overloads landing in FixedPointN.hpp). Compares in integer-limb domain
  // per H4 (no FPN_ToDouble round-trip for the comparison itself).
  if (!(cfg.winsor_pct_low < cfg.winsor_pct_high)) {
    fprintf(stderr,
        "[cfg] WARN: winsor_pct_low (%.6f) >= winsor_pct_high (%.6f); "
        "resetting to defaults (low=0.005, high=0.995)\n",
        FPN_ToDouble(cfg.winsor_pct_low), FPN_ToDouble(cfg.winsor_pct_high));
    cfg.winsor_pct_low  = FPN_FromDouble<F>(0.005);
    cfg.winsor_pct_high = FPN_FromDouble<F>(0.995);
  }

  // v5.15.5.F.4c.3 Step 2 — populate per-core authoritative view from flat fields.
  // Runs AFTER all parser passes (registry walker + manual blocks + per-core overrides
  // + NormalizeForMode). Ensures cfg.cores[c] reflects the fully-resolved per-core
  // view (flat + PerCoreOverrides<F> merged via ControllerConfig_ResolveForCore).
  // Step 3 will replace this with [core N] section parser writing cores[c] directly.
  ControllerConfig_PopulateCoresFromFlat(&cfg);

  return cfg;
}
//======================================================================================================
//======================================================================================================
#endif // CONTROLLER_CONFIG_HPP
