// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [ML STATUS PANEL — v5.9.0b]
//======================================================================================================
// Per-core ML observability surface. Shows for each STRATEGY_ML core:
//   - Model load state: loaded / failed / no-model-configured (tri-state,
//     distinct semantics per V5_9_AUDIT-#2)
//   - Last prediction + threshold + effective threshold (post-confidence-
//     damping) + confidence (V5_9_AUDIT-#3)
//   - NaN/Inf event counters (feature pack + prediction)
//   - ConfidenceScorer IC + RMSE for diagnostic
//
// Stateless render — direct call from main loop, NOT in FOREACH_PANEL(X)
// registry. All data flows through TUISnapshot.per_core[i]. Mirrors the
// v5.8.6b engine header pattern.
//
// Closes silent-failure visibility gap from v5.8 paper testing: operator
// can no longer paper-soak for hours without knowing ML failed to load,
// fed garbage features, or fell through to SimpleDip.
//======================================================================================================
#pragma once

#include "imgui.h"
#include "FoxmlTheme.hpp"
#include "../DataStream/EngineTUI.hpp"
#include "../Strategies/StrategyInterface.hpp"  // v5.10.0a.G.10 — REGIME_INFO
#include "../MemHeaders/FailureModeRegistry.hpp"  // v5.14.8.C — FAILURE_IS_SET accessor
#include "../MemHeaders/PerCoreStateFlagsRegistry.hpp"  // v5.14.9.B.2 — STATE_FLAG_IS_SET

namespace tt {

// v5.10.0c — optional `shared` parameter exposes the pending hot-swap
// state so the panel can show "swap pending" hints. Callers can pass
// nullptr; the row is rendered only when a swap is actually pending.
inline void MLStatus_Render(const TUISnapshot* snap, const TUISharedState* shared = nullptr) {
    if (!snap) return;
    if (ImGui::Begin("ML Status")) {
        if (snap->per_core_count == 0) {
            ImGui::TextDisabled("No active cores.");
            ImGui::End();
            return;
        }

        // Header row.
        ImGui::TextColored(FoxmlColors::sand,
            "Per-core ML state. Failed/NaN counters surface silent failures.");
        ImGui::Separator();

        // Per-core rows.
        for (int i = 0; i < snap->per_core_count && i < 16; ++i) {
            const auto& pc = snap->per_core[i];

            // Only render rows for ML cores OR cores with ML symptoms
            // (load failed / NaN events / pre-warmup) — keeps the panel
            // quiet for non-ML cores in mixed deployments.
            int has_ml_signal = STATE_FLAG_IS_SET(pc, IS_ML) || FAILURE_IS_SET(pc, ml_model_load_failed) ||
                                 pc.ml_nan_feature_events > 0 ||
                                 pc.ml_nan_prediction_events > 0 ||
                                 (STATE_FLAG_IS_SET(pc, IS_ML) && pc.warmup_progress_pct < 100);
            if (!has_ml_signal) continue;

            ImGui::TextColored(FoxmlColors::sand, "core %d:", i);
            ImGui::SameLine(0, 8);

            // v5.9.1 — pre-warmup overlay. ML strategy is configured but
            // hasn't seen enough samples to make a decision yet. Operator
            // would otherwise see model=loaded + pred=0.0000 + thr=— with
            // no signal that the engine is still warming up.
            if (pc.warmup_progress_pct < 100) {
                ImGui::TextColored(FoxmlColors::sand,
                    "warmup: %u%%", (unsigned)pc.warmup_progress_pct);
                ImGui::SetItemTooltip(
                    "Per-core slow path is still gathering rolling samples.\n"
                    "Model + features won't fire predictions until rolling\n"
                    "count reaches min_warmup_samples. Once 100%%, expect a\n"
                    "boot-time stderr line confirming readiness.");
                ImGui::SameLine(0, 14);
            }

            // Model state — tri-state, with ensemble awareness (v5.11.62+).
            // Ensemble counts as "loaded" even when single-zoo buy_model
            // isn't populated — the strategy reads ezoo->primary_handles.
            if (FAILURE_IS_SET(pc, ml_model_load_failed)) {
                ImGui::TextColored(FoxmlColors::red, "model: LOAD FAILED");
                ImGui::SetItemTooltip(
                    "ML strategy was selected but no model could be loaded.\n"
                    "Stamp validation failed, role file missing under any name\n"
                    "(buy_signal/barrier/regime), or held_out_gate_strict=1\n"
                    "with mismatched registry hash. Check the boot log for details.\n"
                    "Operator action: verify cfg path + retrain if needed.");
            } else if (STATE_FLAG_IS_SET(pc, ML_MODEL_LOADED)) {
                ImGui::TextColored(FoxmlColors::green, "model: loaded");
            } else if (pc.ensemble_active && pc.ensemble_n_horizons > 0) {
                ImGui::TextColored(FoxmlColors::green,
                    "model: ensemble (%d horizons)", pc.ensemble_n_horizons);
                ImGui::SetItemTooltip(
                    "Multi-horizon ensemble is active. Single-zoo handle is\n"
                    "unset (no buy_signal.json at base path) but ezoo->primary\n"
                    "is wired to whichever role file was found in the _horizon_<H>\n"
                    "siblings (priority: buy_signal > barrier > regime). The\n"
                    "strategy uses ezoo->primary_handles for predictions.");
            } else {
                ImGui::TextColored(FoxmlColors::sand, "model: (none configured)");
            }

            // v5.10.0c — Hot-swap pending row. Renders only when shared
            // state is available AND a swap is currently pending for this
            // core. Operator's "Apply (live)" button in SettingsPanel sets
            // the request; engine slow-path consumer clears it on swap or
            // refusal. Pending state usually visible for at most one
            // slow-path cycle; sticks longer when deferred for open
            // position (acknowledge_hot_swap_with_open_positions=0).
            if (shared && i < 16) {
                uint8_t pending = __atomic_load_n(
                    &shared->swap_model_path_requested[i], __ATOMIC_ACQUIRE);
                if (pending) {
                    ImGui::SameLine(0, 14);
                    ImGui::TextColored(FoxmlColors::yellow, "→ swap pending:");
                    ImGui::SameLine();
                    ImGui::TextColored(FoxmlColors::sand, "%s",
                        shared->pending_model_path[i]);
                    ImGui::SetItemTooltip(
                        "Hot-swap requested via Settings → Apply (live).\n"
                        "Engine slow-path will reload the zoo on next cycle\n"
                        "unless deferred for an open position.");
                }
            }

            // Decision context — only meaningful when model loaded.
            // v5.11.62 — also count ensemble as "loaded" so the prediction
            // / threshold / confidence row shows for ensemble cores even
            // when single-zoo buy_model is empty.
            bool any_model_active = STATE_FLAG_IS_SET(pc, ML_MODEL_LOADED) ||
                (pc.ensemble_active && pc.ensemble_n_horizons > 0);
            if (any_model_active) {
                ImGui::SameLine(0, 14);
                ImGui::TextColored(FoxmlColors::sand, "pred:");
                ImGui::SameLine();
                ImGui::Text("%.4f", pc.ml_last_prediction);

                ImGui::SameLine(0, 8);
                ImGui::TextColored(FoxmlColors::sand, "thr:");
                ImGui::SameLine();
                // Effective threshold may differ from base when confidence
                // is enabled — show both if they differ.
                if (pc.ml_last_effective_threshold > 0.0 &&
                    pc.ml_last_effective_threshold != pc.ml_last_threshold) {
                    ImGui::Text("%.3f (eff: %.3f)",
                                pc.ml_last_threshold,
                                pc.ml_last_effective_threshold);
                    ImGui::SetItemTooltip(
                        "Base threshold (cfg ml_buy_threshold) vs effective\n"
                        "(post-confidence-damping). When confidence is low,\n"
                        "effective rises (entry harder); when high, drops\n"
                        "(entry easier). See StrategyParameters.hpp damping logic.");
                } else if (pc.ml_last_threshold > 0.0) {
                    ImGui::Text("%.3f", pc.ml_last_threshold);
                } else {
                    ImGui::TextDisabled("—");
                }

                if (pc.ml_last_confidence > 0.0) {
                    ImGui::SameLine(0, 8);
                    ImGui::TextColored(FoxmlColors::sand, "conf:");
                    ImGui::SameLine();
                    ImGui::Text("%.3f", pc.ml_last_confidence);
                }

                // v5.13.6.A — sell-side ML prediction display. Renders
                // when exit_predictor models loaded + BITMAP_IS_SET(cfg.ml_cfg_flags, MASK_ML_CFG_USE_EXIT_MODEL)=1
                // (signaled by ml_last_exit_prediction > 0). Operator sees
                // real-time exit-prob per core; closes parity Section J
                // observability gap. Threshold display (cfg.exit_threshold)
                // not surfaced separately yet — operator reads the cfg if
                // they need to compare; future v5.13.X may add it.
                if (pc.ml_last_exit_prediction > 0.0) {
                    ImGui::SameLine(0, 14);
                    ImGui::TextColored(FoxmlColors::sand, "exit:");
                    ImGui::SameLine();
                    if (pc.ml_last_exit_dominant_horizon >= 0) {
                        ImGui::Text("%.3f (h%d)",
                                    pc.ml_last_exit_prediction,
                                    pc.ml_last_exit_dominant_horizon);
                    } else {
                        ImGui::Text("%.3f", pc.ml_last_exit_prediction);
                    }
                    ImGui::SetItemTooltip(
                        "Sell-side ML prediction this slow-path cycle\n"
                        "(blended across loaded exit_predictor horizons;\n"
                        "h<idx> = dominant arm with max single-model prob).\n"
                        "When > cfg.exit_threshold (default 0.6) and any\n"
                        "position is open on this core, slow-path body\n"
                        "fires MARKET_SELL via OMS_PushSubmit + sets\n"
                        "SHALT_EXIT_PREDICTED on this core's halt reason.");
                }

                // v5.15.5.A.6 — buy-side per-horizon barrier dispatch display.
                // Mirrors exit-side `exit:` line above. Only renders when the
                // per-horizon feature was active this cycle (dominant >= 0
                // means dispatch ran; LEGACY mode never writes a non-negative
                // dominant). Surfaces which horizon's barriers drove the trade.
                if (pc.ml_last_buy_dominant_horizon >= 0) {
                    ImGui::SameLine(0, 14);
                    ImGui::TextColored(FoxmlColors::sand, "barrier:");
                    ImGui::SameLine();
                    static const char* mode_short[] = {
                        "L", "B", "D", "B+s", "D+s"  // LEGACY/BLEND/DOMINANT/BOTH_BLEND/BOTH_DOMINANT
                    };
                    int mode = pc.ml_last_barrier_mode_used;
                    const char* mlabel = (mode >= 0 && mode < 5) ? mode_short[mode] : "?";
                    ImGui::Text("h%d %s", pc.ml_last_buy_dominant_horizon, mlabel);
                    ImGui::SetItemTooltip(
                        "Per-horizon TP/SL barrier dispatch (v5.15.5+).\n"
                        "h<idx> = horizon arm whose stamp barriers drove\n"
                        "this trade's TP/SL (or contributed most weight in\n"
                        "BLEND mode). Mode tag:\n"
                        "  L = LEGACY (cfg-direct fallback)\n"
                        "  B = BLEND (Σ wᵢ · barrierᵢ)\n"
                        "  D = DOMINANT (argmax arm's barriers)\n"
                        "  B+s = BOTH_BLEND_DRIVES (blend drives; dominant shadow-logged)\n"
                        "  D+s = BOTH_DOMINANT_DRIVES (dominant drives; blend shadow-logged)\n"
                        "Enable: cfg.per_horizon_barrier_blend=1 + cfg.barrier_blend_mode=N.");
                    if (pc.ml_barrier_shadow_event_count > 0) {
                        ImGui::SameLine(0, 6);
                        ImGui::TextColored(FoxmlColors::comment, "(sh:%u)",
                                            pc.ml_barrier_shadow_event_count);
                        ImGui::SetItemTooltip(
                            "Cumulative shadow-mode events: count of BOTH_*_DRIVES\n"
                            "cycles where the non-driving result was logged for\n"
                            "offline A/B analysis. Operator uses this to gauge\n"
                            "shadow-data accumulation before promoting blend or\n"
                            "dominant to primary driving mode.");
                    }
                }
            }

            // v5.9.3a — scaler row (Gap H). Mutually-exclusive states.
            if (STATE_FLAG_IS_SET(pc, ML_MODEL_LOADED)) {
                ImGui::SameLine(0, 14);
                if (FAILURE_IS_SET(pc, ml_scaler_load_failed)) {
                    ImGui::TextColored(FoxmlColors::red, "scaler: WARN — load failed");
                    ImGui::SetItemTooltip(
                        "Stamp claimed scaler present but the .scaler sidecar\n"
                        "either failed verification or couldn't be read. Engine\n"
                        "is continuing with IDENTITY scaler applied (held_out_\n"
                        "gate_strict=0). Predictions WILL drift from training.\n"
                        "Operator action: verify .scaler sidecar exists +\n"
                        "matches stamp's scaler_sha256, or set strict=1 to\n"
                        "refuse load entirely.");
                } else if (STATE_FLAG_IS_SET(pc, ML_SCALER_PRESENT)) {
                    ImGui::TextColored(FoxmlColors::green, "scaler: applied");
                    ImGui::SetItemTooltip(
                        "Feature standardizer (mean-centering + unit-variance)\n"
                        "loaded from <model>.scaler sidecar. Features are\n"
                        "transformed before Model_Predict per training-time\n"
                        "stats. v5.9.3a infrastructure / v5.9.3b activates.");
                } else {
                    ImGui::TextColored(FoxmlColors::sand, "scaler: NONE");
                    ImGui::SetItemTooltip(
                        "Stamp does not claim a scaler sidecar (legacy v5.x\n"
                        "model OR v5.9.3+ stamp with feature_scaler_present=0).\n"
                        "Features fed to model raw, no standardization. To\n"
                        "enable: retrain with v5.9.3+ training pipeline.");
                }
            }

            // v5.9.5i — cfg drift summary. Tier 1 = directly affects
            // serving math; Tier 2 = forensic. Refused = strict mode
            // tripped on Tier 1 mismatch. Visible at-a-glance; details
            // in stderr boot log.
            if (pc.cfg_drift_tier1_count > 0 || pc.cfg_drift_tier2_count > 0) {
                ImGui::Indent(20);
                if (pc.cfg_drift_strict_refused) {
                    ImGui::TextColored(FoxmlColors::red,
                        "cfg drift: %u Tier 1 (REFUSED strict), %u Tier 2",
                        (unsigned)pc.cfg_drift_tier1_count,
                        (unsigned)pc.cfg_drift_tier2_count);
                } else if (pc.cfg_drift_tier1_count > 0) {
                    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
                        "cfg drift: %u Tier 1 (WARN), %u Tier 2",
                        (unsigned)pc.cfg_drift_tier1_count,
                        (unsigned)pc.cfg_drift_tier2_count);
                } else {
                    ImGui::TextColored(ImVec4(0.85f, 0.80f, 0.50f, 1.0f),
                        "cfg drift: %u Tier 2 (WARN)",
                        (unsigned)pc.cfg_drift_tier2_count);
                }
                ImGui::SetItemTooltip(
                    "Stamp-recorded cfg differs from runtime cfg.\n"
                    "Tier 1: freshness_tau, threshold_scale, barrier_gate\n"
                    "  (REFUSE strict / WARN otherwise — affects serving math)\n"
                    "Tier 2: hard_block, bandit, fees, hyperparams, build_flags\n"
                    "  (WARN regardless — forensics + reproducibility)\n"
                    "See engine boot stderr for per-field details.\n"
                    "Suppress: acknowledge_inference_cfg_drift=1 in cfg.");
                ImGui::Unindent(20);
            }

            // v5.10.3.B — Runtime IC drift detection observability (parity-check
            // Finding #9 closure; v5.10.0e). Distinguishes drift-kill from
            // MTM-kill / manual-kill (all of which set core_kill_tripped).
            if (STATE_FLAG_IS_SET(pc, DRIFT_BREACHED)) {
                ImGui::Indent(20);
                if (STATE_FLAG_IS_SET(pc, DRIFT_KILL_TRIPPED)) {
                    ImGui::TextColored(FoxmlColors::red,
                        "drift: KILLED (avg_ic=%.4f, n=%u)",
                        pc.drift_avg_ic, (unsigned)pc.drift_n_samples);
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f),
                        "drift: BREACHED (avg_ic=%.4f, n=%u)",
                        pc.drift_avg_ic, (unsigned)pc.drift_n_samples);
                }
                ImGui::SetItemTooltip(
                    "Runtime IC (information coefficient) drift detection.\n"
                    "BREACHED: rolling avg IC stayed below floor for the\n"
                    "  configured window — model predictions diverging from\n"
                    "  realized P&L direction.\n"
                    "KILLED: auto_kill_on_drift=1 + breach → core gated off.\n"
                    "Recovery: restart engine OR retrain model.\n"
                    "Floor + window cfg: confidence_ic_floor / _floor_window.");
                ImGui::Unindent(20);
            }

            // NaN/Inf counters — bright red if non-zero, dim if clean.
            if (pc.ml_nan_feature_events > 0 || pc.ml_nan_prediction_events > 0) {
                ImGui::Indent(20);
                ImGui::TextColored(FoxmlColors::red,
                    "NaN events: features=%u predictions=%u",
                    pc.ml_nan_feature_events, pc.ml_nan_prediction_events);
                ImGui::SetItemTooltip(
                    "Features_PackAll returned -1 sentinel (FPN_Binary out-of-range\n"
                    "or float NaN/Inf at packer boundary), or Model_Predict\n"
                    "produced NaN/Inf. Both cases skip the prediction this\n"
                    "cycle (no entry, fall-through to SimpleDip). Persistent\n"
                    "non-zero values mean upstream state corruption — check\n"
                    "depth/orderbook WS, rolling stats, or recent retrain.");
                ImGui::Unindent(20);
            }
        }

        // v5.10.0a.G.10 — Ensemble (multi-horizon) section.
        // Renders only when at least one core has ensemble active.
        // Heatmap: regimes (rows) × horizons (cols), values are bandit
        // probabilities (post-gamma + uniform mixing). Green = dominant,
        // yellow = mid, dim = low. "Last:" callout shows what the most
        // recent prediction picked.
        bool any_ensemble = false;
        for (int i = 0; i < snap->per_core_count && i < 16; ++i) {
            if (snap->per_core[i].ensemble_active) {
                any_ensemble = true;
                break;
            }
        }
        if (any_ensemble) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Ensemble (multi-horizon)",
                                          ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int i = 0; i < snap->per_core_count && i < 16; ++i) {
                    const auto& cs = snap->per_core[i];
                    if (!cs.ensemble_active) continue;

                    ImGui::TextColored(FoxmlColors::sand,
                        "core %d: %d horizons (mode: %s)",
                        i, (int)cs.ensemble_n_horizons, cs.ensemble_blend_mode);
                    // "Last predicted" callout — useful when bandit weights
                    // look noisy: shows what the dispatcher actually picked.
                    if (cs.ensemble_last_predicted_horizon_idx >= 0 &&
                        cs.ensemble_last_predicted_horizon_idx < (int)cs.ensemble_n_horizons) {
                        int reg = cs.ensemble_last_predicted_regime;
                        const char* reg_name = (reg >= 0 && reg < NUM_REGIMES)
                            ? REGIME_INFO[reg].short_name : "?";
                        int h_ticks = cs.ensemble_horizon_ticks[
                            cs.ensemble_last_predicted_horizon_idx];
                        ImGui::SameLine(0, 14);
                        ImGui::TextColored(FoxmlColors::comment,
                            "(last: regime=%s, horizon=h%d)", reg_name, h_ticks);
                    }

                    char tbl_id[32];
                    snprintf(tbl_id, sizeof(tbl_id), "ensemble_heatmap_c%d", i);
                    int n_h = (int)cs.ensemble_n_horizons;
                    if (n_h > 8) n_h = 8;
                    if (ImGui::BeginTable(tbl_id, n_h + 1,
                                            ImGuiTableFlags_Borders |
                                            ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("regime",
                                                 ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        for (int h = 0; h < n_h; ++h) {
                            char hdr[24];
                            int disabled = (cs.ensemble_disabled_horizon_mask & (1u << h)) ? 1 : 0;
                            snprintf(hdr, sizeof(hdr), "%sh%d%s",
                                     disabled ? "[" : "",
                                     cs.ensemble_horizon_ticks[h],
                                     disabled ? "]" : "");
                            ImGui::TableSetupColumn(hdr,
                                ImGuiTableColumnFlags_WidthFixed, 70.0f);
                        }
                        ImGui::TableHeadersRow();
                        for (int r = 0; r < NUM_REGIMES; ++r) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextColored(FoxmlColors::sand,
                                "%s (%d upd)",
                                REGIME_INFO[r].short_name,
                                cs.ensemble_n_updates_per_regime[r]);
                            for (int h = 0; h < n_h; ++h) {
                                ImGui::TableSetColumnIndex(h + 1);
                                double w = cs.ensemble_weights[r][h];
                                int disabled = (cs.ensemble_disabled_horizon_mask & (1u << h)) ? 1 : 0;
                                if (disabled) {
                                    ImGui::TextColored(FoxmlColors::comment, "—");
                                } else {
                                    // Color by magnitude: green=>0.4, yellow>0.2, dim otherwise
                                    ImVec4 col = (w > 0.4) ? FoxmlColors::green
                                               : (w > 0.2) ? FoxmlColors::sand
                                                            : FoxmlColors::comment;
                                    ImGui::TextColored(col, "%.3f", w);
                                }
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::SetItemTooltip(
                        "Per-regime bandit probabilities. Each row is one\n"
                        "regime; each column is one horizon (training window\n"
                        "length, in ticks). Probabilities sum to 1.0 across\n"
                        "horizons within a regime.\n\n"
                        "Brackets [h100] = disabled by cfg (operator override).\n"
                        "Disabled arms freeze at last weight + skip predict.\n\n"
                        "Last: shows which regime + horizon dominated the\n"
                        "most recent prediction (post-blend or post-selection).");
                    ImGui::Spacing();
                }
            }
        }

        // v5.14.10.D — FULL Bayesian dashboard for Thompson sampling bandit.
        // Renders only when at least one core has cfg.bandit_algorithm != 0
        // AND initialized_thompson_bandits=1 (signaled via per-core
        // thompson_state byte's MASK_THOMPSON_BANDIT_ACTIVE bit).
        // Shows per-arm posterior mean (mu_post) + precision (1/variance)
        // + pull count. Distinct from the Exp3 ensemble heatmap above:
        // ensemble_weights are EXP3 selection probabilities; thompson_mu_post
        // are Bayesian posterior means (different math; different semantics).
        bool any_thompson = false;
        for (int i = 0; i < snap->per_core_count && i < 16; ++i) {
            if (snap->per_core[i].thompson_state &
                TUISnapshot::PerCoreSnap::MASK_THOMPSON_BANDIT_ACTIVE) {
                any_thompson = true;
                break;
            }
        }
        // ──────────────────────────────────────────────────────────────────
        // v5.15.1 — Model Health CollapsingHeader (between Ensemble + Thompson).
        // Reads failure_flags drift bits set at CoreModelZoo_TryLoadRole
        // chokepoint + aggregated to PerCoreSnap by ShardedSnapshot publish.
        // Per CLAUDE.md item 12 (display↔execution invariant): drift state set
        // engine-side is visible operator-side.
        // ──────────────────────────────────────────────────────────────────
        static constexpr uint16_t MODEL_HEALTH_DRIFT_MASK =
            FAILURE_MASK_feature_hash_drift     |
            FAILURE_MASK_label_hash_drift       |
            FAILURE_MASK_build_flags_drift      |
            FAILURE_MASK_scaler_drift           |
            FAILURE_MASK_cfg_binding_drift      |
            FAILURE_MASK_stamp_hmac_not_verified|
            FAILURE_MASK_model_age_warn;
        static constexpr uint16_t MODEL_HEALTH_DRIFT_RED_MASK =
            FAILURE_MASK_feature_hash_drift |
            FAILURE_MASK_label_hash_drift   |
            FAILURE_MASK_scaler_drift;

        // Aggregate across cores for header summary (any drift tripped anywhere?).
        uint16_t any_drift_flags = 0;
        int max_tripped_per_core = 0;
        for (int i = 0; i < snap->per_core_count && i < 16; ++i) {
            uint16_t drift = snap->per_core[i].failure_flags & MODEL_HEALTH_DRIFT_MASK;
            any_drift_flags |= drift;
            int tripped_here = __builtin_popcount((unsigned)drift);
            if (tripped_here > max_tripped_per_core) max_tripped_per_core = tripped_here;
        }
        int total_tripped = __builtin_popcount((unsigned)any_drift_flags);
        const bool drift_red = (any_drift_flags & MODEL_HEALTH_DRIFT_RED_MASK) != 0;
        char model_health_label[96];
        if (total_tripped == 0) {
            snprintf(model_health_label, sizeof(model_health_label),
                     "Model Health: clean (no drift across cores)##model_health_header");
        } else {
            snprintf(model_health_label, sizeof(model_health_label),
                     "Model Health: %d drift bit%s tripped %s##model_health_header",
                     total_tripped,
                     total_tripped == 1 ? "" : "s",
                     drift_red ? "[RED]" : "[YELLOW]");
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader(model_health_label,
                                      total_tripped > 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            if (total_tripped == 0) {
                ImGui::TextColored(FoxmlColors::green,
                    "No drift detected on any loaded model. "
                    "Stamp + scaler + cfg + HMAC + age all aligned with runtime.");
            } else {
                for (int i = 0; i < snap->per_core_count && i < 16; ++i) {
                    const auto& pc = snap->per_core[i];
                    uint16_t drift = pc.failure_flags & MODEL_HEALTH_DRIFT_MASK;
                    if (drift == 0) continue;
                    ImGui::TextColored(FoxmlColors::sand, "core %d:", i);
                    ImGui::Indent(20);

                    auto render_bit = [&](uint16_t mask, const char *label, const char *tooltip,
                                          const ImVec4& color) {
                        if (drift & mask) {
                            ImGui::TextColored(color, "%s", label);
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
                        }
                    };
                    render_bit(FAILURE_MASK_feature_hash_drift,
                               "feat: HASH DRIFT",
                               "Model's stamp-bound feature_registry_hash does not match the\n"
                               "current FEATURE_REGISTRY_HASH at runtime.\n"
                               "Indicates schema drift since training (feature added / removed /\n"
                               "reordered).\n"
                               "Operator action: retrain with current feature set OR document drift.",
                               FoxmlColors::red);
                    render_bit(FAILURE_MASK_label_hash_drift,
                               "label: HASH DRIFT",
                               "Model's stamp-bound label_registry_hash does not match the\n"
                               "current LABEL_REGISTRY_HASH at runtime.\n"
                               "Indicates label-kind schema drift since training.\n"
                               "Operator action: retrain with current label set.",
                               FoxmlColors::red);
                    render_bit(FAILURE_MASK_scaler_drift,
                               "scaler: BIND DRIFT",
                               "Loaded scaler's training-time registry_hash does not match the\n"
                               "model's training-time feature_registry_hash. Scaler binding\n"
                               "broke between training and runtime (sidecar copied from a\n"
                               "different training session?).\n"
                               "Operator action: retrain scaler with current model + features.",
                               FoxmlColors::red);
                    render_bit(FAILURE_MASK_build_flags_drift,
                               "build: FLAG DRIFT",
                               "Model's stamp-bound build_flags_hash does not match the current\n"
                               "build's compile-time flags hash. Predictions may diverge if a\n"
                               "build flag (LATENCY_PROFILING, USE_XGBOOST, etc.) affects\n"
                               "feature compute.\n"
                               "Operator action: rebuild engine with matching flags OR retrain.",
                               FoxmlColors::yellow);
                    render_bit(FAILURE_MASK_cfg_binding_drift,
                               "cfg: BIND DRIFT",
                               "One or more stamp-bound cfg fields diverge between training-time\n"
                               "and runtime cfg.* values. Examples: confidence_threshold_scale,\n"
                               "barrier_gate_enabled, bandit_blend_ratio.\n"
                               "Operator action: review boot log for per-field WARN; retrain if\n"
                               "intentional or revert cfg to training-time values.",
                               FoxmlColors::yellow);
                    render_bit(FAILURE_MASK_stamp_hmac_not_verified,
                               "stamp: HMAC NOT VERIFIED",
                               "Stamp body's HMAC signature was not verified at load\n"
                               "(cfg.held_out_stamp_secret empty OR cfg.model_verify_strict=skip).\n"
                               "Live trading should always run with non-empty secret + strict\n"
                               "verification (REFUSE in v5.15.2 boot gate when trading_mode=live).\n"
                               "Operator action: set held_out_stamp_secret + model_verify_strict=1.",
                               FoxmlColors::yellow);
                    render_bit(FAILURE_MASK_model_age_warn,
                               "model: AGE WARN",
                               "Model's training_timestamp_us indicates age beyond\n"
                               "cfg.model_max_age_hours. Stale model may produce predictions\n"
                               "detached from current market regime.\n"
                               "Operator action: retrain on recent data OR adjust\n"
                               "model_max_age_hours.",
                               FoxmlColors::yellow);
                    // Model age display (sand; informational, even when no AGE_WARN bit tripped).
                    if (pc.handle_training_timestamp_us > 0) {
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL +
                                          (uint64_t)ts.tv_nsec / 1000ULL;
                        if (now_us > pc.handle_training_timestamp_us) {
                            uint64_t age_h = (now_us - pc.handle_training_timestamp_us) /
                                             (3600ULL * 1000000ULL);
                            ImGui::TextColored(FoxmlColors::sand,
                                "model age: %llu h", (unsigned long long)age_h);
                        }
                    }
                    ImGui::Unindent(20);
                }
            }
        }

        if (any_thompson) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Thompson Bayesian dashboard (v5.14.10.D)",
                                          ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int i = 0; i < snap->per_core_count && i < 16; ++i) {
                    const auto& cs = snap->per_core[i];
                    if (!(cs.thompson_state &
                          TUISnapshot::PerCoreSnap::MASK_THOMPSON_BANDIT_ACTIVE)) continue;
                    int chosen_arm = (cs.thompson_state &
                                       TUISnapshot::PerCoreSnap::MASK_THOMPSON_CHOSEN_ARM) >>
                                       TUISnapshot::PerCoreSnap::SHIFT_THOMPSON_CHOSEN_ARM;
                    ImGui::TextColored(FoxmlColors::sand,
                        "core %d: Thompson active (last chose arm %d)",
                        i, chosen_arm);

                    char tbl_id[32];
                    snprintf(tbl_id, sizeof(tbl_id), "thompson_dash_c%d", i);
                    int n_h = (int)cs.ensemble_n_horizons;
                    if (n_h > 8) n_h = 8;
                    if (n_h <= 0) continue;
                    if (ImGui::BeginTable(tbl_id, n_h + 1,
                                            ImGuiTableFlags_Borders |
                                            ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("metric",
                                                 ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        for (int h = 0; h < n_h; ++h) {
                            char hdr[24];
                            snprintf(hdr, sizeof(hdr), "h%d%s",
                                     cs.ensemble_horizon_ticks[h],
                                     (h == chosen_arm) ? "*" : "");
                            ImGui::TableSetupColumn(hdr,
                                ImGuiTableColumnFlags_WidthFixed, 75.0f);
                        }
                        ImGui::TableHeadersRow();

                        // Row 1: mu_post (posterior mean per arm)
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(FoxmlColors::sand, "mu_post");
                        for (int h = 0; h < n_h; ++h) {
                            ImGui::TableSetColumnIndex(h + 1);
                            float mu = cs.thompson_mu_post[h];
                            ImVec4 col = (h == chosen_arm) ? FoxmlColors::green : FoxmlColors::sand;
                            ImGui::TextColored(col, "%.3f", mu);
                        }
                        // Row 2: precision_post (1/variance per arm)
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(FoxmlColors::sand, "precision");
                        for (int h = 0; h < n_h; ++h) {
                            ImGui::TableSetColumnIndex(h + 1);
                            float prec = cs.thompson_precision_post[h];
                            // Color by magnitude: green=tight (high precision); dim=loose (low)
                            ImVec4 col = (prec > 5.0f) ? FoxmlColors::green
                                       : (prec > 2.0f) ? FoxmlColors::sand
                                                        : FoxmlColors::comment;
                            ImGui::TextColored(col, "%.2f", prec);
                        }
                        // Row 3: total_pulls (Bayesian pull count per arm)
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextColored(FoxmlColors::sand, "pulls");
                        for (int h = 0; h < n_h; ++h) {
                            ImGui::TableSetColumnIndex(h + 1);
                            uint32_t pulls = cs.thompson_total_pulls[h];
                            ImGui::TextColored(FoxmlColors::comment, "%u", pulls);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::SetItemTooltip(
                        "Bayesian Thompson sampling posterior dashboard.\n\n"
                        "mu_post:    posterior mean reward per arm (highest = preferred).\n"
                        "precision:  1/variance; HIGHER = tighter posterior (more confident).\n"
                        "pulls:      total reward updates received per arm.\n\n"
                        "Header h<N>* marks the arm Thompson sampled this cycle.\n"
                        "Note: posterior values are FLOAT-cast for display from the\n"
                        "underlying double-precision Bayesian state in ThompsonBanditState.");
                    ImGui::Spacing();
                }
            }
        }
    }
    ImGui::End();
}

}  // namespace tt
