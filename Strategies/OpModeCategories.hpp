// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
//======================================================================================================
// [OPERATIONAL MODE CATEGORIES]
//======================================================================================================
// v5.15.5.F.4b — categorical-applicability bitmap enum for operational mode classification.
// Used by CfgFieldRegistry's applies_to_op_mode_cat column to declare WHICH MODES a cfg
// field is relevant to (LIVE / PAPER / BACKTEST / TRAINING / OFFLINE).
//
// .F.4b initial population: most fields tagged OP_MODE_CAT_ALL (universal applicability).
// .F.4i + v5.15.6 will specialize backtest/controller/secrets/training cfg fields with
// narrower op-mode masks (e.g., backtest_data_path → OP_MODE_CAT_BACKTEST only).
//
// Pattern: DESIGN_SPECS/categorical-tag-applicability-pattern.md
//======================================================================================================
#pragma once
#include <cstdint>

enum OpModeCategory : uint16_t {
    OP_MODE_CAT_LIVE        = 1u << 0,    // live trading via Binance REST
    OP_MODE_CAT_PAPER       = 1u << 1,    // paper trading (default)
    OP_MODE_CAT_BACKTEST    = 1u << 2,    // historical replay via BacktestEngine
    OP_MODE_CAT_TRAINING    = 1u << 3,    // model training (foxml_suite Train Model worker)
    OP_MODE_CAT_OFFLINE     = 1u << 4,    // any non-live mode (umbrella; backtest + training)

    // ─── SENTINEL ────────────────────────────────────────────────────────────
    OP_MODE_CAT_ALL         = 0xFFFFu,    // applies universally
};

// Bitmap overflow guard per DESIGN_SPECS/bitmap-overflow-protection-discipline.md
static_assert(OP_MODE_CAT_OFFLINE < (1u << 16),
              "OpModeCategory bitmap overflowed uint16_t — upgrade to uint32_t");
