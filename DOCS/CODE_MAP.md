# CODE_MAP.md

Auto-generated function index. Walks .hpp files in each subsystem and extracts `Pattern_FunctionName` style definitions with their one-line purpose (from the preceding `//` comment, when present).

**Re-generate**: `./tools/gen_code_map.sh`

**Last regenerated**: 2026-04-29 (commit 439dcd2)

## CoreFrameworks/

### BinanceAdapter.hpp

- `BinanceAdapter_WorkerLoop` — line 140 — shutdown_requested flips.
- `BinanceAdapter_Init` — line 225 — 1; future commits scale up after the back-to-back stress test passes.
- `BinanceAdapter_ShutdownState` — line 279 — without a successful Init (shutdown_requested is already 0 by default).
- `BinanceAdapter_SubmitMarketBuy` — line 302 — holds with worker_count == 1.
- `BinanceAdapter_SubmitMarketSell` — line 322
- `BinanceAdapter_GetBalancesImpl` — line 356 — pausing submissions during a reconciliation pass.
- `BinanceAdapter_QueryOrderImpl` — line 364
- `BinanceAdapter_ShutdownImpl` — line 381
- `BinanceAdapter_Get` — line 394

### ControllerConfig.hpp

- `Fee_Compute` — line 541
- `ControllerConfig_ResolveForCore` — line 559
- `ControllerConfig_Load` — line 816

### ControllerEventLoop.hpp

- `CoreSlowState_Init` — line 117
- `EventLoopState_Init` — line 395
- `EventLoopState_InitLegacy` — line 487
- `EventLoopState_Free` — line 505
- `EventLoopState_RegisterCore` — line 528
- `Sharded_LegSlot` — line 578 — All slow-path / boot-time. Trivially inlined.
- `Sharded_ValidatePartialExitCfg` — line 615
- `EventLoopState_SetCoreStrategy` — line 667
- `EventLoopState_AttachTradeLog` — line 687
- `EventLoopState_AttachOms` — line 701
- `EventLoopState_Balance` — line 719
- `EventLoopState_RealizedPnl` — line 724
- `EventLoopState_FeeRate` — line 729
- `EventLoopState_Portfolio` — line 734
- `EventLoopState_PortfolioMut` — line 739
- `EventLoopState_KsMinBalance` — line 744
- `EventLoopState_KsMaxDrawdownPct` — line 749
- `EventLoopState_KsPeakBalance` — line 754
- `EventLoopState_TradeLog` — line 769
- `EventLoopState_SetIntendedParams` — line 784
- `EventLoop_DrainPostFillOneCore` — line 839
- `EventLoop_DrainPostFill` — line 936
- `EventLoop_OnEvent` — line 970
- `EventLoop_DrainEvents` — line 1130
- `EventLoop_QueueParameters` — line 1164
- `EventLoop_RebuildAllParameters` — line 1197
- `EventLoop_UpdateRollingStateOneCore` — line 1286
- `EventLoop_UpdateRollingStateAllCores` — line 1324
- `EventLoop_UpdateEmaPriceAllCores` — line 1340
- `EventLoop_RebuildAllParameters_PerCore` — line 1358
- `EventLoop_RebuildOneCore` — line 1397
- `EventLoop_PushParameters` — line 1871
- `EventLoopState_ConfigureKillSwitch` — line 1901
- `EventLoop_ClearAllPermissions` — line 1911
- `EventLoop_KillSwitchTrip` — line 1922
- `EventLoop_KillSwitchEvaluate` — line 1950
- `EventLoop_TimeExitOneCore` — line 2023
- `EventLoop_TimeExit` — line 2082
- `EventLoop_TrailingSLRatchetOneCore` — line 2118
- `EventLoop_TrailingSLRatchet` — line 2175
- `EventLoop_Unpause` — line 2190
- `EventLoop_SlowPath` — line 2213
- `EventLoop_RunController` — line 2238

### CoreLatencyStats.hpp

- `CoreLatencyStats_Init` — line 128 — stats mid-run without disabling them.
- `CoreLatencyStats_Reset` — line 140
- `CoreLatencyStats_Enable` — line 151
- `CoreLatencyStats_Disable` — line 155
- `CoreLatencyStats_Sample` — line 170 — rdtsc reading at sample time, used for "last seen" tracking in the TUI.
- `CoreLatencyStats_Snapshot` — line 198 — skip the conversion (cycle counts only).

### EngineSharded.hpp

- `EngineSharded_CalibrateTscGhz` — line 128 — raw cycles. ~50ms of busy work, plenty accurate for diagnostic display.
- `EngineSharded_PinThread` — line 155 — worse tail latency due to scheduler migration).
- `EngineSharded_GetSiblingCPU` — line 184 — (caller should fall back to the simple round-robin auto-derive).
- `EngineSharded_SmartSlowPathPins` — line 217 — out_pins[0..num_slow-1] gets the chosen CPU IDs. Returns 1 on success.
- `EngineSharded_DumpLatency` — line 280
- `EngineSharded_Run` — line 326

### EventLoopAggregates.hpp

- `EventLoop_GetAggregates` — line 103

### ExecutionCore.hpp

- `ExecutionCore_Init` — line 152
- `ExecutionCore_SetParameters` — line 189
- `ExecutionCore_SetPermission` — line 209
- `ExecutionCore_Tick` — line 227

### GateParameters.hpp

- `BG_Evaluate` — line 141
- `SG_Evaluate` — line 163
- `GateParameters_Init` — line 176

### LegacyReferenceDriver.hpp

- `LegacyReference_Init` — line 95
- `LegacyReference_AddSlot` — line 120
- `LegacyReference_Tick` — line 138
- `LegacyReference_SlowPath` — line 183
- `LegacyReference_Run` — line 203

### Notify.hpp

- `NotifyState_Init` — line 169
- `Notify_Send` — line 188 — Cooldown gate uses CLOCK_MONOTONIC (NTP-jump-safe).
- `NotifyState_Shutdown` — line 238 — Drain remaining events + join worker thread + free pthread resources.
- `NotifyBackend_Stderr` — line 253 — [STDERR BACKEND] — default, always available, ships in Phase 8b
- `Notify_ShellEscape` — line 316 — Document this for users; provide a wrapper script if needed.
- `Notify_BuildCommand` — line 339 — overflowed before completion (still tries to run what fit).
- `NotifyBackend_Command` — line 366

### OrderEventLog.hpp

- `OrderEventLog_Init` — line 127
- `OrderEventLog_Free` — line 145
- `OrderEventLog_Append` — line 170
- `OrderEventLog_InitWithFile` — line 216
- `OrderEventLog_Reset` — line 261
- `OrderEventLog_LoadFromDisk` — line 302
- `OrderEvent_MakeFill` — line 370
- `OrderEvent_MakeRejection` — line 395
- `Portfolio_FromEventLog` — line 442

### OrderGates.hpp

- `Gate_Zero` — line 62
- `Gate_ZeroAll` — line 72

### Order.hpp

- `Order_Init` — line 119
- `Order_IsTerminal` — line 158

### OrderManager.hpp

- `OrderManager_Init` — line 372
- `OMS_PushSubmit` — line 614
- `OMS_DrainSubmit` — line 659
- `OrderManager_HandleFill` — line 698
- `OrderManager_ProcessFillCommand` — line 884
- `OrderManager_ProcessReconcile` — line 982
- `OrderManager_Tick` — line 1018
- `OrderManager_Shutdown` — line 1052
- `OrderManager_InflightCount` — line 1075

### ParameterSlot.hpp

- `ParameterSlot_Init` — line 143
- `ParameterSlot_Write` — line 180
- `ParameterSlot_Read` — line 225

### PortfolioController.hpp

- `PortfolioController_Init` — line 271
- `KillSwitch_Activate` — line 475
- `KillSwitch_Reset` — line 489
- `Buying_Halt` — line 498
- `PortfolioController_DrainExits` — line 687
- `PortfolioController_StrategyBuySignal` — line 714
- `PortfolioController_StrategyDispatch` — line 816
- `PortfolioController_Tick` — line 862
- `PortfolioController_Unpause` — line 1838
- `PortfolioController_CycleRegime` — line 1849
- `PortfolioController_HotReload` — line 1875
- `PortfolioController_SaveSnapshot` — line 1939
- `PortfolioController_LoadSnapshot` — line 2009

### Portfolio.hpp

- `ExitBuffer_PendingProceeds` — line 88
- `Portfolio_AddPositionWithExits` — line 150
- `Portfolio_OpenSlot` — line 201
- `Portfolio_CloseSlot` — line 217
- `Portfolio_SlotActive` — line 225
- `Portfolio_UpdatePosition` — line 241
- `Portfolio_Save` — line 362
- `Portfolio_Load` — line 393

### ReconciliationLoop.hpp

- `ReconciliationLoop_Pass` — line 93
- `ReconciliationLoop_Init` — line 194
- `ReconciliationLoop_Start` — line 224
- `ReconciliationLoop_TriggerNow` — line 234
- `ReconciliationLoop_Shutdown` — line 242

### ShardedBacktestDriver.hpp

- `ShardedBacktestDriver_Init` — line 142
- `ShardedBacktest_RunTick` — line 190
- `ShardedBacktest_Run` — line 369

### ShardedLiveSafety.hpp

- `EngineSharded_OrphanRecovery` — line 46 — expected positions, we can reconcile rather than blindly selling.
- `EngineSharded_ForceCloseOnShutdown` — line 146

### ShardedOrderLatency.hpp

- `ShardedOrderLatency_Reset` — line 46 — before the first order can fire.
- `ShardedOrderLatency_Sample` — line 59 — race in a future thread-pool world; safe and cheap with one writer too.

### ShardedSnapshot.hpp

- `TUI_CopySnapshotSharded` — line 38

### ShardedSnapshotPersist.hpp

- `ShardedSnapshot_Save` — line 72
- `ShardedSnapshot_Load` — line 246

### ShardedTradeLog.hpp

- `ShardedTradeLog_Init` — line 76 — without a trade log, you just don't get the CSV.
- `ShardedTradeLog_Flush` — line 132 — called once at engine shutdown.
- `ShardedTradeLog_Rotate` — line 148 — the existing file open).
- `ShardedTradeLog_Close` — line 178
- `ShardedTradeLog_RecordEntry` — line 200
- `ShardedTradeLog_RecordExit` — line 237

### SPSCRing.hpp

- `SPSCRing_Init` — line 95
- `SPSCRing_TryPush` — line 113
- `SPSCRing_TryPop` — line 142
- `SPSCRing_Depth` — line 164
- `SPSCRing_Capacity` — line 173

## Strategies/

### MeanReversion.hpp

- `MeanReversion_Init` — line 71
- `MeanReversion_Adapt` — line 123
- `MeanReversion_BuySignal` — line 309
- `MeanReversion_ExitAdjust` — line 457

### MLStrategy.hpp

- `MLStrategy_Init` — line 43
- `MLStrategy_Adapt` — line 64
- `MLStrategy_BuySignal` — line 84
- `MLStrategy_ExitAdjust` — line 130

### Momentum.hpp

- `Momentum_Init` — line 58
- `Momentum_Adapt` — line 90
- `Momentum_BuySignal` — line 183
- `Momentum_ExitAdjust` — line 265

### RegimeDetector.hpp

- `CumDelta_Init` — line 122
- `CumDelta_Push` — line 130
- `TickRate_Init` — line 162
- `TickRate_Push` — line 170
- `TickRate_CurrentZ` — line 197
- `Regime_ComputeSignals` — line 221
- `Regime_Init` — line 467
- `Regime_Classify` — line 497
- `Regime_ToStrategy` — line 610
- `Regime_AdjustPositions` — line 628

### SimpleDip.hpp

- `SimpleDip_Init` — line 32
- `SimpleDip_Adapt` — line 43
- `SimpleDip_BuySignal` — line 57

### StrategyParameters.hpp

- `Strategy_SpacingOk` — line 128
- `Strategy_TpFloor` — line 147
- `SimpleDip_BuildParameters` — line 190
- `MeanReversion_BuildParameters` — line 264
- `Momentum_BuildParameters` — line 313
- `EmaCross_BuildParameters` — line 377
- `ML_BuildParameters` — line 424
- `Strategy_BuildParameters` — line 637

## Strategies/private/

### EmaCross.hpp

- `EmaCross_Init` — line 33
- `EmaCross_Adapt` — line 45
- `EmaCross_BuySignal` — line 57
- `EmaCross_ExitAdjust` — line 107

## DataStream/

### BinanceCrypto.hpp

- `BinanceStream_Init` — line 471 — (internally tracks whether its already been called)
- `BinanceStream_Close` — line 545 — clean shutdown: send close frame, SSL shutdown, close socket, free resources
- `BinanceStream_Reconnect` — line 581
- `BinanceStream_Poll` — line 627 — returns OR'd combination of POLL_NONE, POLL_SOCKET, POLL_STDIN
- `BinanceStream_ReadTick` — line 675
- `BinanceStream_InWindDown` — line 736 — BinanceStream_ShouldReconnect: returns 1 if it's time to close and reconnect
- `BinanceStream_ShouldReconnect` — line 749
- `BinanceStream_HasPending` — line 765 — returns 1 if SSL has buffered data that can be read without blocking
- `BinanceConfig_Load` — line 776 — same key=value format as ControllerConfig_Load, skips # comments and empty lines

### BinanceDepth.hpp

- `DepthStream_Init` — line 192

### BinanceOrderAPI.hpp

- `BinanceOrderAPI_Cleanup` — line 476
- `BinanceOrderAPI_MarketBuy` — line 486 — fill_price_out/fill_qty_out receive actual execution values (NULL = don't care)
- `BinanceOrderAPI_MarketSell` — line 532 — place a market sell order
- `BinanceOrderAPI_GetStatus` — line 578 — fills filled_qty and avg_price on success
- `BinanceOrderAPI_LoadFilters` — line 627 — returns 1 on success, 0 on failure (caller should treat as fatal)
- `BinanceOrderAPI_GetBalance` — line 662 — returns 1 on success, 0 on failure
- `BinanceOrderAPI_GetBalances` — line 689 — returns 1 on success, 0 on failure
- `BinanceOrderAPI_SyncClock` — line 713 — re-sync clock offset (call periodically or after reconnect)
- `BinanceOrderAPI_Init` — line 727 — must be called after Cleanup, ServerTime, SyncClock, LoadFilters are defined

### BinanceUserData.hpp

- `BinanceUserData_Init` — line 583 — ws_result_queue: pointer into the OMS's dedicated WS SPSC ring
- `BinanceUserData_Start` — line 622
- `BinanceUserData_Shutdown` — line 631

### DepthRecorder.hpp

- `DepthRecorder_MkdirP` — line 63
- `DepthRecorder_DateInt` — line 78
- `DepthRecorder_OpenFile` — line 86
- `DepthRecorder_PruneOld` — line 122
- `DepthRecorder_Init` — line 157
- `DepthRecorder_LogGap` — line 193 — disconnect time, or the current snapshot's timestamp_us).
- `DepthRecorder_Write` — line 221
- `DepthRecorder_Close` — line 267

### DepthReplayState.hpp

- `DepthReplay_DateInt` — line 76
- `DepthReplayState_Init` — line 94
- `DepthReplayState_Free` — line 129
- `DepthReplayState_LoadDay` — line 162
- `DepthReplayState_Advance` — line 278
- `DepthReplayState_GetSnapshot` — line 303

### EngineTUI.hpp

- `TUI_Init` — line 129
- `TUI_Cleanup` — line 162
- `TUI_Render` — line 185
- `TUI_HandleInput` — line 571
- `MLSnapshot_Populate` — line 691
- `TUI_CopySnapshot` — line 1056
- `TUI_CopySnapshot` — line 1062
- `TUI_CopySnapshot` — line 1069
- `TUI_PopulatePerCoreLatency` — line 1329
- `TUI_PopulatePerCoreSlowPathLatency` — line 1376
- `TUI_PopulateAdvancedTopology` — line 1414
- `TUI_PopulateTopology` — line 1451 — poll_interval[i]    — per-core resolved poll cadence
- `TUI_Render_Snapshot` — line 1489 — runs on TUI thread. reads only from snapshot (all doubles, no FPN).
- `TUI_ReadKey` — line 1700

### FauxFIX.hpp

- `FIX_ParseTag` — line 75 — returns the tag number, writes value start and length into out params
- `FIX_ParseDouble` — line 106 — not meant to be fast, just correct enough for test data
- `FIX_Parse` — line 182 — validates checksum if tag 10 is present
- `FIX_BuildMarketDataMsg` — line 256 — writes into buf which must be at least FIX_MAX_MSG_LEN bytes

### MetricsLog.hpp

- `MetricsLog_Init` — line 40
- `MetricsLog_Close` — line 62
- `MetricsLog_SlowPath` — line 85
- `MetricsLog_Event` — line 139

### MockGenerator.hpp

- `MockRNG_Seed` — line 31
- `MockRNG_Double` — line 42 — returns a double in [0.0, 1.0)
- `MockRNG_Range` — line 47 — returns a double in [lo, hi)
- `MockGenerator_Init` — line 75
- `MockGenerator_NextTick` — line 88 — returns the length written to buf, and fills out the parsed message for convenience
- `MockGenerator_Batch` — line 122 — buf is scratch space for building FIX messages (reused each tick)

### TickRecorder.hpp

- `TickRecorder_MkdirP` — line 42
- `TickRecorder_DateInt` — line 57
- `TickRecorder_OpenFile` — line 70
- `TickRecorder_PruneOld` — line 105
- `TickRecorder_Init` — line 140
- `TickRecorder_Push` — line 172
- `TickRecorder_Close` — line 196

### TradeLog.hpp

- `TradeLog_Init` — line 57
- `TradeLog_Buy` — line 90
- `TradeLog_Sell` — line 103
- `TradeLog_Close` — line 115
- `TradeLogBuffer_Init` — line 137
- `TradeLogBuffer_PushBuy` — line 143 — hot path: push a record to the ring buffer (~10ns, no file I/O)
- `TradeLogBuffer_PushSell` — line 162
- `TradeLogBuffer_Drain` — line 184 — slow path: drain all buffered records to the CSV file

### TUIAnsi.hpp

- `ANSI_Section_Header` — line 330
- `ANSI_Section_TopBar` — line 404
- `ANSI_Section_Market` — line 435
- `ANSI_Section_Regime` — line 479 — new section: shows R², vol_ratio, ror_slope that weren't in previous TUI backends
- `ANSI_Section_BuyGate` — line 590
- `ANSI_Section_Portfolio` — line 666
- `ANSI_Section_PnL` — line 705
- `ANSI_Section_Risk` — line 730
- `ANSI_Section_Config` — line 758
- `ANSI_Section_Stats` — line 785
- `ANSI_Section_Positions` — line 846
- `ANSI_Section_Charts` — line 923
- `ANSI_Section_Controls` — line 963
- `ANSI_Section_Latency` — line 979
- `ANSI_Section_PerCoreLatency` — line 1018 — per-core latency the moment they flip engine_mode=sharded.
- `ANSI_Section_RightPanel` — line 1058 — hidden on narrow terminals (< 100 columns)
- `ANSI_Layout_Standard` — line 1119
- `ANSI_Layout_Charts` — line 1165 — ANSI_Section_RightPanel(ab, s, h, w, start_time);
- `ANSI_Layout_Compact` — line 1203
- `ANSI_Layout_Render` — line 1218
- `ANSI_Render` — line 1240 — call from TUI thread at desired FPS

## FixedPoint/

### FixedPoint64.hpp

- `FP64_ToDouble` — line 47
- `FP64_Equal` — line 196
- `FP64_NotEqual` — line 201
- `FP64_LessThan` — line 205
- `FP64_LessThanOrEqual` — line 213
- `FP64_GreaterThan` — line 217
- `FP64_GreaterThanOrEqual` — line 221
- `FP64_IsZero` — line 231

## MemHeaders/

## ML_Headers/

### BanditLearning.hpp

- `Bandit_Init` — line 75
- `Bandit_InitDefault` — line 96 — convenience: init with default FoxML parameters
- `Bandit_SetArmName` — line 102 — set arm name (call after init)
- `Bandit_GetProbabilities` — line 111 — p_i = (1 - gamma) * (w_i / sum_w) + gamma / K
- `Bandit_Select` — line 143 — returns arm index. use a PRNG or hardware RNG for the random value.
- `Bandit_Update` — line 164 — with adaptive eta: min(eta_max, sqrt(ln(K) / (K * T)))
- `Bandit_GetWeights` — line 207 — returns weights summing to 1.0 (for blending / display)
- `Bandit_EffectiveBlend` — line 229 — steps >= min+ramp:             effective_blend = blend_ratio
- `Bandit_BlendWeights` — line 238
- `Bandit_Print` — line 268

### BarrierGate.hpp

- `BarrierGate_Compute` — line 37 — compute barrier gate value from peak/valley predictions

### ConfidenceScore.hpp

- `RollingIC_Init` — line 62
- `RollingIC_Push` — line 69
- `RollingIC_Compute` — line 95 — returns IC in [-1, 1], or 0.0 if insufficient data
- `RollingRMSE_Init` — line 151
- `RollingRMSE_Push` — line 158
- `RollingRMSE_Compute` — line 166
- `Confidence_Freshness` — line 183 — stability: 1 / (1 + RMSE)
- `Confidence_Stability` — line 189
- `Confidence_Compute` — line 193
- `ConfidenceScorer_Init` — line 215
- `ConfidenceScorer_Update` — line 223 — feed a prediction + actual return pair (call after outcome is known)
- `ConfidenceScorer_Compute` — line 230 — compute current confidence given data age

### CoreModelZoo.hpp

- `CoreModelZoo_Init` — line 58
- `CoreModelZoo_TryLoadRole` — line 71
- `CoreModelZoo_LoadFromDir` — line 104
- `CoreModelZoo_LoadLegacy` — line 135
- `CoreModelZoo_Free` — line 146
- `CoreModelZoo_HasAny` — line 156
- `CoreModelZoo_VerifyExpected` — line 184 — features in the pack, model crashes or produces garbage.

### CostModel.hpp

- `CostModel_Estimate` — line 56 — k1, k2, k3:     cost coefficients
- `CostModel_EstimateDefault` — line 83 — convenience: estimate with default coefficients
- `CostModel_Breakeven` — line 93 — cost is in bps, divide by 10000 to get decimal return
- `CostModel_ShouldTrade` — line 98 — should we trade? returns 1 if expected alpha > breakeven

### FlowFeatures.hpp

- `BookImbHistory_Init` — line 66
- `BookImbHistory_Push` — line 75
- `BookImbHistory_MeanLong` — line 89
- `BookImbHistory_Last` — line 96
- `BookImbHistory_MeanShort` — line 105
- `FlowState_Init` — line 143
- `FlowState_Push` — line 150
- `LargeTradeState_Init` — line 197
- `LargeTradeState_Push` — line 207
- `LargeTradeState_ZScore` — line 224
- `LargeTradeState_Last` — line 239
- `SpreadState_Init` — line 270
- `SpreadState_Push` — line 280
- `SpreadState_ZScore` — line 295
- `SpreadState_Last` — line 309

### ModelInference.hpp

- `FeatureLookback_Max` — line 169 — used by: ValidationSplit (purge gap), PortfolioController (warmup check)
- `FeatureLookback_CountEnabled` — line 179 — count enabled features (for validation)
- `Model_Init` — line 215
- `Model_Load` — line 227
- `Model_Predict` — line 354
- `Model_PredictMulti` — line 406
- `Model_Free` — line 461
- `Model_IsLoaded` — line 482
- `ModelFeatures_Pack` — line 496

### RewardTracker.hpp

- `RewardTracker_Init` — line 36
- `RewardTracker_Push` — line 41
- `RewardTracker_DrainCSV` — line 58 — append all pending records to CSV, then clear

### RollingStats.hpp

- `RollingStats_Push` — line 116
- `RollingStats_VolumeSignificant` — line 244
- `RollingStats_EntrySpacing` — line 257
- `RollingStats_BuyPrice` — line 274

### ROR_regressor.hpp

- `RORRegressor_Init` — line 38
- `RORRegressor_Push` — line 60

### VolScaler.hpp

- `VolScaler_Size` — line 45 — positive = long, negative = short (we only go long in current engine)
- `VolScaler_SizeDefault` — line 60 — convenience: scale with default parameters
- `VolScaler_InverseAlpha` — line 68 — useful for: "what alpha does this position size imply?"
- `VolScaler_RawZ` — line 76 — raw z-score without clipping (for analytics / display)

### WelfordStats.hpp

- `Welford_Init` — line 27
- `Welford_Push` — line 39
- `Welford_Variance` — line 67
- `Welford_Stddev` — line 76
- `Welford_ZScore` — line 84
- `Welford_Reset` — line 93

## GUI/

### CandleAccumulator.hpp

- `CandleAccumulator_Init` — line 34
- `CandleAccumulator_Push` — line 41 — called from engine thread on every tick
- `CandleAccumulator_PushWithTime` — line 86 — instead of using wall-clock time(NULL)
- `CandleAccumulator_Snapshot` — line 132
- `CandleAccumulator_SetInterval` — line 157 — reset accumulator with new interval (clears all candle data)
- `CandleAccumulator_Destroy` — line 168

### ChartPanel.hpp

- `ChartState_Prepare` — line 66
- `GUI_PriceChart` — line 143
- `GUI_VolumeChart` — line 1110 — VOLUME CHART — separate dockable window
- `GUI_LivePnLChart` — line 1228 — LIVE P&L — streaming chart from pnl_history ring buffer
- `GUI_EquityChart` — line 1288 — EQUITY CURVE — separate dockable window (only renders with trade data)

### DashboardPanels.hpp

- `GUI_R2Bar` — line 27 — slope_dir: positive slope → green, negative → red, near zero → neutral
- `GUI_Panel_Header` — line 70 — PANEL: HEADER — fox kaomoji, version, state, uptime, session
- `GUI_Panel_TopBar` — line 195 — PANEL: TOP BAR — key metrics at a glance
- `GUI_Panel_Market` — line 232 — PANEL: MARKET (merged Market Structure + Regime Signals)
- `GUI_Panel_BuyGate` — line 438 — PANEL: BUY GATE
- `GUI_Panel_Account` — line 692 — PANEL: ACCOUNT (merged Portfolio + P&L + Risk)
- `GUI_Panel_Config` — line 912 — PANEL: CONFIG
- `GUI_Panel_Positions` — line 960 — PANEL: POSITIONS — proper table with aligned columns
- `GUI_Panel_PerCorePnL` — line 1216 — Pure GUI thread, doesn't touch engine state.
- `GUI_Panel_Stats` — line 1312 — PANEL: STATS
- `GUI_Panel_Latency` — line 1394 — PANEL: LATENCY (conditional on LATENCY_PROFILING)
- `GUI_Panel_MLIntelligence` — line 1450 — PANEL: ML INTELLIGENCE — bandit arms, confidence, cost, model info
- `GUI_RenderDashboard` — line 1617

### FoxmlTheme.hpp

- `Foxml_ApplyTheme` — line 45

### GuiThread.hpp

- `Gui_Init` — line 41 — GUI INIT
- `Gui_Shutdown` — line 147 — GUI SHUTDOWN
- `Gui_BeginFrame` — line 160 — GUI FRAME
- `Gui_EndFrame` — line 184
- `Gui_SetupDefaultLayout` — line 199 — chart 60% left, dashboard panels stacked 40% right
- `Gui_HandleKeys` — line 249 — GUI KEYBOARD — same controls as ANSI TUI

### LogViewerPanel.hpp

- `LogViewer_Init` — line 22
- `LogViewer_Refresh` — line 28
- `GUI_Panel_LogViewer` — line 57

### SettingsPanel.hpp

- `Settings_Load` — line 511
- `Settings_RenderGlobalTab` — line 652 — GLOBAL TAB — renders the auto-generated field_defs[] layout
- `Settings_RenderPerCoreTab` — line 789
- `GUI_Panel_Settings` — line 981 — running cores, not cfg-only intent — engine doesn't add/remove cores live.

### TradeHistoryPanel.hpp

- `TradeHistory_Init` — line 30
- `TradeHistory_Refresh` — line 35
- `GUI_Panel_TradeHistory` — line 131

### TradeReader.hpp

- `TradeData_Init` — line 38
- `TradeData_Refresh` — line 75

## Backtest/

### BacktestEngine.hpp

- `BacktestData_DetectFormat` — line 49 — timestamp_us,price,quantity,is_buyer_maker
- `BacktestData_Load` — line 56
- `BacktestResults_Init` — line 170
- `BacktestResults_Free` — line 182
- `BacktestResults_Reset` — line 210 — against zero capacity (defense-in-depth) but this is the load-bearing fix.
- `BacktestResults_EnsureCapacity` — line 233 — grow sample buffers by 2x when full
- `BacktestResults_EnsureEquityCapacity` — line 260 — array, so silent truncation produces wrong Sharpe / max DD / return.
- `XGBoost_ComputeScalePosWeight` — line 293 — (0.0 = negative, 1.0 = positive, 0.5 = neutral and already filtered).
- `XGBoost_ComputeMulticlassWeights` — line 325 — receives per-class sample counts so caller can log them.
- `BacktestStats_Compute` — line 354
- `BacktestStats_ComputeFromEquity` — line 402 — sharpe — needs equity curve data too
- `BacktestSharded_Run` — line 442
- `Backtest_ComputeLabelsFromSamples` — line 466 — gate.
- `Backtest_Run` — line 557 — equity curve).
- `Backtest_RunWalkForward` — line 668 — Backtest_RunFullValidation calls it on a sliced view of BacktestResults.
- `Backtest_RunFullValidation` — line 676
- `WalkForward_ComputeAccuracy` — line 738 — uses > 0.5f for truth so neutral (0.5) labels are never counted as positive
- `WalkForward_ComputeMulticlassAccuracy` — line 752 — argmax over each row, compare to integer truth (rounded from label float).
- `WalkForward_ComputeMSE` — line 771 — regression: mean squared error. Lower = better. Sensitive to outliers.
- `WalkForward_ComputeCorrelation` — line 787 — gets low MSE on small-magnitude targets while having zero predictive power).
- `Backtest_RunWalkForward` — line 811
- `ConfigField_Set` — line 1193 — handles both FPN and PCT fields (PCT keys are stored as decimal, value comes in as %).
- `Backtest_RunSweep` — line 1276

### BacktestPanels.hpp

- `DataPanel_Init` — line 44
- `DataPanel_Scan` — line 49
- `RunControl_Init` — line 150
- `SamplesSnapshot_Compute` — line 161 — only when running==0, giving a safe happens-before relationship.
- `RunControl_Start` — line 232
- `GUI_Panel_DataBrowser` — line 272
- `GUI_Panel_RunControl` — line 371
- `GUI_Panel_Results` — line 420
- `PastRuns_Init` — line 556
- `PastRuns_LoadOne` — line 591 — scan one run directory's metadata files
- `PastRuns_ScanOneDir` — line 649 — backward compat with flat models/{run_name}/ runs from before v4.3.
- `PastRuns_Scan` — line 664
- `PastRun_MetricLabel` — line 679 — label-type-aware metric label
- `GUI_Panel_PastRuns` — line 685
- `Comparison_Init` — line 930
- `Comparison_Free` — line 934
- `Comparison_SaveRun` — line 941
- `GUI_Panel_Comparison` — line 982
- `OptimizerPanel_Init` — line 1132
- `GUI_Panel_Optimizer` — line 1164
- `TrainingPanel_Init` — line 1368
- `GUI_Panel_Training` — line 1436

### BacktestSharded.hpp

- `SharedBacktest_FromHistorical` — line 76
- `BacktestSharded_Run` — line 93 — aggregates results.

### BacktestSnapshot.hpp

- `BacktestSnapshot_Copy` — line 20

### Fingerprint.hpp

- `SHA256_Init` — line 78
- `SHA256_Update` — line 85
- `SHA256_Final` — line 103
- `SHA256_ToHex` — line 125 — convenience: hash to hex string (65 bytes including null terminator)
- `Fingerprint_HashFile` — line 140 — streams file through SHA256 in 64KB chunks — handles multi-GB files.
- `Fingerprint_Compute` — line 174
- `Fingerprint_Short` — line 203 — short fingerprint (first 12 hex chars) for display

### HeldOutSplit.hpp

- `HeldOutSplit_GenToken` — line 62
- `HeldOutSplit_Make` — line 80
- `HeldOutSplit_TestAccessAllowed` — line 111
- `HeldOutSplit_Unlock` — line 118 — Logs unlock event to stderr — caller can also Notify_Send for audit trail.
- `HeldOutSplit_Relock` — line 133 — Generates a NEW token, so any code holding the old one can't unlock again.

### LabelFunctions.hpp

- `Label_WinLoss` — line 48 — no trade was entered at that point.
- `Label_Barrier` — line 69 — same as win/loss but with configurable asymmetric barriers.
- `Label_ForwardPnl` — line 87 — useful for regression (predict magnitude, not just direction).
- `Label_Regime` — line 104 — useful for training a regime classifier model.
- `Label_VolBarrier` — line 127 — source: ~/FoxML/private/DATA_PROCESSING/targets/barrier.py
- `LabelType_NumClasses` — line 309 — ≥2 = multiclass softmax       (label values 0..K-1 as floats)
- `LabelType_IsBinary` — line 314
- `LabelType_IsRegression` — line 318
- `LabelType_IsMulticlass` — line 322

### OverfitDetection.hpp

- `OverfitDetection_Check` — line 68 — feat_cap:     feature count cap (0 to disable)
- `OverfitDetection_CheckDefaults` — line 133 — convenience: check with default FoxML thresholds
- `OverfitDetection_CheckRegression` — line 159 — interpretation is correlation-space. Both tunable separately if needed.
- `OverfitDetection_CheckRegressionDefaults` — line 210
- `OverfitDetection_CountOverfit` — line 221 — returns: number of folds flagged as overfit
- `OverfitDetection_Print` — line 230 — print report (for logging / debugging)

### ValidationSplit.hpp

- `PurgeGap_Compute` — line 66 — first test tick to prevent any form of temporal leakage.
- `PurgeGap_ComputeExplicit` — line 73 — overload: caller provides explicit max_lookback (for testing or custom feature sets)
- `ValidationSplit_Generate` — line 126 — returns: number of valid folds generated (may be < n_splits if early folds skipped)
- `ValidationSplit_GenerateExplicit` — line 205 — used by walk-forward when splitting in non-neutral sample space where raw lookback doesn't apply
- `ValidationSplit_Verify` — line 271 — returns 1 if all folds are clean, 0 if leakage detected
- `ValidationSplit_Print` — line 292 — print fold summary (for logging / debugging)

## tests/

---

## Top-level files

- `main.cpp` — 1202 lines
- `Version.hpp` — 8 lines
- `Limits.hpp` — 30 lines

## Conventions

- Function names follow `Pattern_FunctionName` convention (e.g. `Portfolio_Init`, `BG_Evaluate`)
- Headers are inline-heavy — most functions live in `.hpp` and are `inline`
- Templates parameterize on `unsigned F` (FPN word count), default `F=64` (4096-bit)
- Lowercase helpers (`fan_out`, `drain_with_submit`) are local to a function and not in this map
- ALL_CAPS macros are not in this map; see headers directly
