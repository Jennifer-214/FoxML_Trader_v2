# CODE_MAP.md

Auto-generated function index. Walks .hpp files in each subsystem and extracts `Pattern_FunctionName` style definitions with their one-line purpose (from the preceding `//` comment, when present).

**Re-generate**: `./tools/gen_code_map.sh`

**Last regenerated**: 2026-05-07 (commit c2ad684)

## CoreFrameworks/

### BinanceAdapter.hpp

- `BinanceAdapter_WorkerLoop` — line 140 — shutdown_requested flips.
- `BinanceAdapter_Init` — line 253 — 1; future commits scale up after the back-to-back stress test passes.
- `BinanceAdapter_ShutdownState` — line 307 — without a successful Init (shutdown_requested is already 0 by default).
- `BinanceAdapter_SubmitMarketBuy` — line 330 — holds with worker_count == 1.
- `BinanceAdapter_SubmitMarketSell` — line 350
- `BinanceAdapter_GetBalancesImpl` — line 384 — pausing submissions during a reconciliation pass.
- `BinanceAdapter_QueryOrderImpl` — line 392
- `BinanceAdapter_ShutdownImpl` — line 409
- `BinanceAdapter_Get` — line 422

### ControllerConfig.hpp

- `Fee_Compute` — line 799
- `ControllerConfig_ResolveForCore` — line 817
- `ControllerConfig_Load` — line 1155

### ControllerEventLoop.hpp

- `CoreSlowState_Init` — line 123
- `EventLoopState_Init` — line 500
- `EventLoopState_InitLegacy` — line 638
- `EventLoopState_Free` — line 669
- `EventLoopState_RegisterCore` — line 718
- `Sharded_LegSlot` — line 768 — All slow-path / boot-time. Trivially inlined.
- `Sharded_ValidatePartialExitCfg` — line 805
- `EventLoopState_SetCoreStrategy` — line 857
- `EventLoopState_AttachTradeLog` — line 877
- `EventLoopState_AttachOms` — line 891
- `EventLoopState_Balance` — line 909
- `EventLoopState_RealizedPnl` — line 914
- `EventLoopState_FeeRate` — line 919
- `EventLoopState_Portfolio` — line 924
- `EventLoopState_PortfolioMut` — line 929
- `EventLoopState_KsMinBalance` — line 934
- `EventLoopState_KsMaxDrawdownPct` — line 939
- `EventLoopState_KsPeakBalance` — line 944
- `EventLoopState_TradeLog` — line 959
- `EventLoopState_SetIntendedParams` — line 974
- `EventLoop_DrainPostFillOneCore` — line 1029
- `EventLoop_DrainPostFill` — line 1293
- `EventLoop_OnEvent` — line 1337
- `EventLoop_DrainEvents` — line 1497
- `EventLoop_QueueParameters` — line 1531
- `EventLoop_RebuildAllParameters` — line 1564
- `EventLoop_UpdateRollingStateOneCore` — line 1653
- `EventLoop_UpdateRollingStateAllCores` — line 1691
- `EventLoop_UpdateEmaPriceAllCores` — line 1707
- `EventLoop_RebuildAllParameters_PerCore` — line 1725
- `EventLoop_RebuildOneCore` — line 1764
- `EventLoop_PushParameters` — line 2477
- `EventLoopState_ConfigureKillSwitch` — line 2507
- `EventLoop_ClearAllPermissions` — line 2517
- `EventLoop_KillSwitchTrip` — line 2528
- `EventLoop_KillSwitchEvaluate` — line 2556
- `EventLoop_TimeExitOneCore` — line 2629
- `EventLoop_TimeExit` — line 2688
- `EventLoop_TrailingSLRatchetOneCore` — line 2724
- `EventLoop_TrailingSLRatchet` — line 2781
- `EventLoop_Unpause` — line 2796
- `EventLoop_SlowPath` — line 2819
- `EventLoop_RunController` — line 2844

### CoreLatencyStats.hpp

- `CoreLatencyStats_Init` — line 128 — stats mid-run without disabling them.
- `CoreLatencyStats_Reset` — line 140
- `CoreLatencyStats_Enable` — line 151
- `CoreLatencyStats_Disable` — line 155
- `CoreLatencyStats_Sample` — line 170 — rdtsc reading at sample time, used for "last seen" tracking in the TUI.
- `CoreLatencyStats_Snapshot` — line 198 — skip the conversion (cycle counts only).

### EngineSharded.hpp

- `EngineSharded_CalibrateTscGhz` — line 132 — raw cycles. ~50ms of busy work, plenty accurate for diagnostic display.
- `EngineSharded_PinThread` — line 159 — worse tail latency due to scheduler migration).
- `EngineSharded_GetSiblingCPU` — line 188 — (caller should fall back to the simple round-robin auto-derive).
- `EngineSharded_SmartSlowPathPins` — line 221 — out_pins[0..num_slow-1] gets the chosen CPU IDs. Returns 1 on success.
- `EngineSharded_DumpLatency` — line 284
- `CoreModelZoo_ValidateAgainstCfg` — line 362
- `EngineSharded_Run` — line 571

### EventLoopAggregates.hpp

- `EventLoop_GetAggregates` — line 103

### ExecutionCore.hpp

- `ExecutionCore_Init` — line 190
- `ExecutionCore_SetParameters` — line 228
- `ExecutionCore_SetPermission` — line 248
- `ExecutionCore_Tick_Impl` — line 278
- `ExecutionCore_Tick` — line 604

### GateParameters.hpp

- `BG_Evaluate` — line 152
- `SG_Evaluate` — line 174
- `GateParameters_Init` — line 191

### LegacyReferenceDriver.hpp

- `LegacyReference_Init` — line 95
- `LegacyReference_AddSlot` — line 120
- `LegacyReference_Tick` — line 138
- `LegacyReference_SlowPath` — line 183
- `LegacyReference_Run` — line 203

### MetricCompute.hpp

- `Compute_ProfitFactor` — line 46 — → backtest-suite layering boundary.
- `Compute_AllWinsRun` — line 52 — numerical profit_factor is 0.0 in this case, separately).
- `Compute_Expectancy` — line 56
- `Compute_WinRate` — line 68
- `Compute_AvgHoldTicks` — line 74
- `Compute_ReturnPct` — line 80
- `MaxDrawdown_UpdateIncremental` — line 91 — regression test needed.

### Notify.hpp

- `NotifyState_Init` — line 169
- `Notify_Send` — line 188 — Cooldown gate uses CLOCK_MONOTONIC (NTP-jump-safe).
- `NotifyState_Shutdown` — line 238 — Drain remaining events + join worker thread + free pthread resources.
- `NotifyBackend_Stderr` — line 253 — [STDERR BACKEND] — default, always available, ships in Phase 8b
- `Notify_ShellEscape` — line 316 — Document this for users; provide a wrapper script if needed.
- `Notify_BuildCommand` — line 339 — overflowed before completion (still tries to run what fit).
- `NotifyBackend_Command` — line 366

### OrderEventLog.hpp

- `OrderEventLog_Init` — line 187
- `OrderEventLog_Free` — line 230
- `OrderEventLog_ApplyEvent` — line 274
- `OrderEventLog_Append` — line 297
- `OrderEventLog_AsyncWriterRoutine` — line 332
- `OrderEventLog_StartAsyncWriter` — line 366
- `OrderEventLog_StopAsyncWriter` — line 381
- `OrderEventLog_InitWithFile` — line 400
- `OrderEventLog_Reset` — line 445
- `OrderEventLog_LoadFromDisk` — line 486
- `OrderEvent_MakeFill` — line 561
- `OrderEvent_MakeRejection` — line 586
- `Portfolio_FromEventLog` — line 633

### OrderGates.hpp

- `Gate_Zero` — line 62
- `Gate_ZeroAll` — line 72

### Order.hpp

- `Order_Init` — line 119
- `Order_IsTerminal` — line 158

### OrderManager.hpp

- `OrderManager_Init` — line 383
- `OMS_PushSubmit` — line 654
- `OMS_DrainSubmit` — line 699
- `OrderManager_HandleFill` — line 738
- `OrderManager_ProcessFillCommand` — line 924
- `OrderManager_ProcessReconcile` — line 1026
- `OrderManager_Tick` — line 1062
- `OrderManager_Shutdown` — line 1096
- `OrderManager_InflightCount` — line 1119

### ParameterSlot.hpp

- `ParameterSlot_Init` — line 143
- `ParameterSlot_Write` — line 180
- `ParameterSlot_Read` — line 225

### PortfolioController.hpp

- `PortfolioController_Init` — line 282
- `KillSwitch_Activate` — line 515
- `KillSwitch_Reset` — line 529
- `Buying_Halt` — line 538
- `PortfolioController_DrainExits` — line 727
- `PortfolioController_StrategyBuySignal` — line 754
- `PortfolioController_StrategyDispatch` — line 856
- `PortfolioController_Tick` — line 902
- `PortfolioController_Unpause` — line 1914
- `PortfolioController_CycleRegime` — line 1925
- `PortfolioController_HotReload` — line 1951
- `PortfolioController_SaveSnapshot` — line 2015
- `PortfolioController_LoadSnapshot` — line 2085

### Portfolio.hpp

- `ExitBuffer_PendingProceeds` — line 88
- `Portfolio_AddPositionWithExits` — line 150
- `Portfolio_OpenSlot` — line 201
- `Portfolio_CloseSlot` — line 217
- `Portfolio_SlotActive` — line 225
- `Portfolio_UpdatePosition` — line 241
- `Portfolio_Save` — line 362
- `Portfolio_Load` — line 393

### Reconcile.hpp

- `Reconcile_ParseOpenOrders` — line 188 — Output: fills `out` array up to `out_cap`. Returns count parsed.
- `Reconcile_ParseMyTrades` — line 221 — Output: fills `out` array up to `out_cap`. Returns count parsed.
- `Reconcile_Decide` — line 263 — Outputs ReconcileResult with planned actions. Caller applies them.
- `Reconcile_LogReport` — line 325 — 3. If refused_boot: caller exits / refuses to advance

### ReconciliationLoop.hpp

- `ReconciliationLoop_Pass` — line 93
- `ReconciliationLoop_Init` — line 194
- `ReconciliationLoop_Start` — line 224
- `ReconciliationLoop_TriggerNow` — line 234
- `ReconciliationLoop_Shutdown` — line 242

### ShardedBacktestDriver.hpp

- `ShardedBacktestDriver_Init` — line 142
- `ShardedBacktest_RunTick` — line 190
- `ShardedBacktest_Run` — line 378

### ShardedLiveSafety.hpp

- `EngineSharded_OrphanRecovery` — line 46 — expected positions, we can reconcile rather than blindly selling.
- `EngineSharded_ForceCloseOnShutdown` — line 146

### ShardedOrderLatency.hpp

- `ShardedOrderLatency_Reset` — line 46 — before the first order can fire.
- `ShardedOrderLatency_Sample` — line 59 — race in a future thread-pool world; safe and cheap with one writer too.

### ShardedSnapshot.hpp

- `TUI_CopySnapshotSharded` — line 39

### ShardedSnapshotPersist.hpp

- `ShardedSnapshot_Save` — line 90
- `ShardedSnapshot_Load` — line 285

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
- `MeanReversion_ExitAdjustSharded` — line 564

### MLStrategy.hpp

- `MLStrategy_Init` — line 54
- `MLStrategy_Adapt` — line 81
- `MLStrategy_Adapt_Canonical` — line 99
- `MLStrategy_BuySignal` — line 119
- `MLStrategy_ExitAdjust` — line 208
- `MLStrategy_ExitAdjustSharded` — line 273

### Momentum.hpp

- `Momentum_Init` — line 58
- `Momentum_Adapt` — line 90
- `Momentum_BuySignal` — line 183
- `Momentum_ExitAdjust` — line 265
- `Momentum_ExitAdjustSharded` — line 346

### RegimeDetector.hpp

- `CumDelta_Init` — line 123
- `CumDelta_Push` — line 131
- `TickRate_Init` — line 163
- `TickRate_Push` — line 171
- `TickRate_CurrentZ` — line 198
- `Regime_ComputeSignals` — line 222
- `Regime_Init` — line 478
- `Regime_Classify` — line 510
- `Regime_ToStrategy` — line 664
- `Regime_AdjustPositions` — line 682

### SimpleDip.hpp

- `SimpleDip_Init` — line 32
- `SimpleDip_Adapt` — line 43
- `SimpleDip_BuySignal` — line 57
- `SimpleDip_ExitAdjustSharded` — line 100

### StrategyLifecycle.hpp

- `Strategy_SeedFromCfg` — line 66
- `Strategy_SeedFromCfg` — line 71
- `Strategy_SeedFromCfg` — line 85
- `Strategy_FreePerCore` — line 113
- `Strategy_InitPerCore` — line 116
- `Strategy_AdaptPerCore` — line 178
- `Strategy_WriteRatchetSL` — line 264
- `Strategy_WriteRatchetTP` — line 301
- `Strategy_ExitAdjustPerCore` — line 333
- `Strategy_FreePerCore` — line 372

### StrategyParameters.hpp

- `Strategy_SpacingOk` — line 170
- `Strategy_TpFloor` — line 189
- `SimpleDip_BuildParameters` — line 232
- `MeanReversion_BuildParameters` — line 311
- `Momentum_BuildParameters` — line 382
- `EmaCross_BuildParameters` — line 496
- `ML_BuildParameters` — line 601
- `Strategy_BuildParameters` — line 1041

## Strategies/private/

### EmaCross.hpp

- `EmaCross_Init` — line 33
- `EmaCross_Adapt` — line 45
- `EmaCross_BuySignal` — line 57
- `EmaCross_ExitAdjust` — line 107
- `EmaCross_ExitAdjustSharded` — line 181

## DataStream/

### BinanceCrypto.hpp

- `BinanceStream_Init` — line 488 — (internally tracks whether its already been called)
- `BinanceStream_Close` — line 562 — clean shutdown: send close frame, SSL shutdown, close socket, free resources
- `BinanceStream_Reconnect` — line 598
- `BinanceStream_Poll` — line 644 — returns OR'd combination of POLL_NONE, POLL_SOCKET, POLL_STDIN
- `BinanceStream_ReadTick` — line 692
- `BinanceStream_InWindDown` — line 755 — BinanceStream_ShouldReconnect: returns 1 if it's time to close and reconnect
- `BinanceStream_ShouldReconnect` — line 768
- `BinanceStream_HasPending` — line 784 — returns 1 if SSL has buffered data that can be read without blocking
- `BinanceConfig_Load` — line 795 — same key=value format as ControllerConfig_Load, skips # comments and empty lines

### BinanceDepth.hpp

- `DepthStream_Init` — line 192

### BinanceOrderAPI.hpp

- `BinanceOrderAPI_Cleanup` — line 493
- `BinanceOrderAPI_MarketBuy` — line 503 — fill_price_out/fill_qty_out receive actual execution values (NULL = don't care)
- `BinanceOrderAPI_MarketSell` — line 549 — place a market sell order
- `BinanceOrderAPI_GetStatus` — line 595 — fills filled_qty and avg_price on success
- `BinanceOrderAPI_LoadFilters` — line 644 — returns 1 on success, 0 on failure (caller should treat as fatal)
- `BinanceOrderAPI_GetBalance` — line 679 — returns 1 on success, 0 on failure
- `BinanceOrderAPI_GetOpenOrders` — line 711 — network-independent (testable without real REST calls).
- `BinanceOrderAPI_GetMyTrades` — line 722 — the last-known-processed trade id to catch only new fills.
- `BinanceOrderAPI_GetBalances` — line 738 — returns 1 on success, 0 on failure
- `BinanceOrderAPI_SyncClock` — line 762 — re-sync clock offset (call periodically or after reconnect)
- `BinanceOrderAPI_Init` — line 776 — must be called after Cleanup, ServerTime, SyncClock, LoadFilters are defined

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

- `TUI_Init` — line 131
- `TUI_Cleanup` — line 164
- `TUI_Render` — line 187
- `TUI_HandleInput` — line 574
- `MLSnapshot_Populate` — line 694
- `TUISnapshot_InitSeq` — line 1267 — populated" state) — this only initializes the sequence counter.
- `TUISnapshot_Publish_Begin` — line 1282 — the new active.
- `TUISnapshot_Publish_End` — line 1298 — Any subsequent reader sees the just-filled buffer as active.
- `TUISnapshot_ReadInto` — line 1310 — effectively never observed.
- `TUI_CopySnapshot` — line 1340
- `TUI_CopySnapshot` — line 1346
- `TUI_CopySnapshot` — line 1353
- `TUI_PopulatePerCoreLatency` — line 1627
- `TUI_PopulatePerCoreSlowPathLatency` — line 1674
- `TUI_PopulateAdvancedTopology` — line 1712
- `TUI_PopulateTopology` — line 1749 — poll_interval[i]    — per-core resolved poll cadence
- `TUI_Render_Snapshot` — line 1787 — runs on TUI thread. reads only from snapshot (all doubles, no FPN).
- `TUI_ReadKey` — line 1998

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
- `FP64_Equal` — line 245
- `FP64_NotEqual` — line 250
- `FP64_LessThan` — line 254
- `FP64_LessThanOrEqual` — line 262
- `FP64_GreaterThan` — line 266
- `FP64_GreaterThanOrEqual` — line 270
- `FP64_IsZero` — line 280

### FixedPointN.hpp

- `FPN_BlendOnMask` — line 440

## MemHeaders/

### HealthLog.hpp

- `HealthLog_Singleton` — line 95 — Process-singleton. Engine init writes; all callers read.
- `Health_LogConfigureWithRotation` — line 105 — preserved for legacy callers (rotation disabled = max_bytes=0).
- `Health_LogConfigure` — line 124
- `Health_LogPruneRotated` — line 136 — Health_Log (would loop).
- `Health_LogEnabled` — line 165 — "level <= min_level" emits.
- `Health_Log` — line 176 — Returns 1 on success, 0 on i/o failure (ignored by most callers).
- `Health_LogCriticalRateLimited` — line 294 — double-emit-once on race, not data corruption).

### InitArena.hpp

- `InitArena_Create` — line 62 — continue but loses the pre-fault guarantee.
- `InitArena_Alloc` — line 97 — are 8 (for plain structs) or 64 (for cache-line-aligned hot structures).
- `InitArena_AllocOne` — line 122
- `InitArena_Destroy` — line 128 — struct). After Destroy, the arena is empty and capacity=0.
- `InitArena_Used` — line 145 — Introspection: how many bytes have been allocated from the arena so far.
- `InitArena_Remaining` — line 151 — is unknown; this is an upper bound).
- `InitArena_Owns` — line 163 — ctrl->rolling_long = nullptr;
- `InitArena_Global` — line 182 — spawn and after they join.

### RunHistory.hpp

- `RunHistory_Append` — line 74 — duration so a comma-decimal locale doesn't break JSON-numeric parsers.

## ML_Headers/

### BanditLearning.hpp

- `Bandit_Init` — line 82
- `Bandit_InitDefault` — line 103 — convenience: init with default FoxML parameters
- `Bandit_SetArmName` — line 109 — set arm name (call after init)
- `Bandit_GetProbabilities` — line 118 — p_i = (1 - gamma) * (w_i / sum_w) + gamma / K
- `Bandit_Select` — line 201 — returns arm index. use a PRNG or hardware RNG for the random value.
- `Bandit_Update` — line 222 — with adaptive eta: min(eta_max, sqrt(ln(K) / (K * T)))
- `Bandit_GetWeights` — line 265 — returns weights summing to 1.0 (for blending / display)
- `Bandit_EffectiveBlend` — line 287 — steps >= min+ramp:             effective_blend = blend_ratio
- `Bandit_BlendWeights` — line 296
- `Bandit_Print` — line 326
- `Bandit_SaveJSON` — line 369 — n_regimes (NUM_REGIMES). NULL → omits the field.
- `Bandit_JsonFindKey` — line 440 — returns position past the colon. Whitespace + escapes minimal.
- `Bandit_JsonParseDoubleArray` — line 455 — values; returns count parsed. Stops at ']'.
- `Bandit_JsonParseIntArray` — line 473
- `Bandit_LoadJSON` — line 503 — caller's prior Bandit_Init call — load is overlay only.

### BarrierGate.hpp

- `BarrierGate_Compute` — line 37 — compute barrier gate value from peak/valley predictions

### ConfidenceScore.hpp

- `RollingIC_Init` — line 64
- `RollingIC_Push` — line 71
- `RollingIC_Compute` — line 97 — returns IC in [-1, 1], or 0.0 if insufficient data
- `RollingRMSE_Init` — line 153
- `RollingRMSE_Push` — line 160
- `RollingRMSE_Compute` — line 168
- `Confidence_Freshness` — line 185 — stability: 1 / (1 + RMSE)
- `Confidence_Stability` — line 191
- `Confidence_Compute` — line 195
- `ConfidenceScorer_Init` — line 217
- `ConfidenceScorer_Update` — line 235 — feed a prediction + actual return pair (call after outcome is known)
- `ConfidenceScorer_Compute` — line 242 — compute current confidence given data age
- `DriftHistory_Init` — line 275
- `DriftHistory_Push` — line 279
- `DriftHistory_CheckBreach` — line 291 — out_avg_ic / out_samples are optional diagnostic outputs.

### CoreModelZoo.hpp

- `CoreModelZoo_Init` — line 67
- `CoreModelZoo_TryLoadRole` — line 85
- `CoreModelZoo_LoadFromDir` — line 336
- `CoreModelZoo_LoadLegacy` — line 381
- `CoreModelZoo_Free` — line 392
- `CoreModelZoo_HasAny` — line 402
- `CoreModelZoo_VerifyExpected` — line 430 — features in the pack, model crashes or produces garbage.
- `EnsembleModelZoo_Init` — line 680
- `EnsembleModelZoo_RecordPrediction` — line 729
- `EnsembleModelZoo_UpdateDrift` — line 758
- `EnsembleModelZoo_TickRewardsFromLookback` — line 803
- `EnsembleModelZoo_TradeCloseReward` — line 869
- `EnsembleModelZoo_InitBandits` — line 923
- `EnsembleModelZoo_SetDisabledHorizons` — line 962
- `EnsembleModelZoo_Free` — line 988
- `EnsembleModelZoo_LoadFromCfg` — line 1014
- `EnsembleZoo_VerifyGridMemberConsistency` — line 1141
- `EnsembleModelZoo_AutoDetectFromDir` — line 1211
- `EnsembleModelZoo_ComputeBundleId` — line 1354
- `EnsembleModelZoo_SaveBanditState` — line 1376
- `EnsembleModelZoo_LoadBanditState` — line 1399
- `EnsembleModelZoo_LoadBanditStateFromPath` — line 1439
- `EnsembleModelZoo_SetBanditSaveInterval` — line 1466
- `EnsembleModelZoo_MaybeSaveBanditPeriodic` — line 1482

### CostModel.hpp

- `CostModel_Estimate` — line 56 — k1, k2, k3:     cost coefficients
- `CostModel_EstimateDefault` — line 83 — convenience: estimate with default coefficients
- `CostModel_Breakeven` — line 93 — cost is in bps, divide by 10000 to get decimal return
- `CostModel_ShouldTrade` — line 98 — should we trade? returns 1 if expected alpha > breakeven

### FeatureRegistry.hpp

- `ML_Compute_ShortSlope` — line 87
- `ML_Compute_ShortR2` — line 92
- `ML_Compute_ShortVariance` — line 97
- `ML_Compute_LongSlope` — line 102
- `ML_Compute_LongR2` — line 107
- `ML_Compute_LongVariance` — line 112
- `ML_Compute_VolRatio` — line 117
- `ML_Compute_RorSlope` — line 122
- `ML_Compute_VolumeSlope` — line 127
- `ML_Compute_VolumeDelta` — line 132
- `ML_Compute_EmaSmaSpread` — line 151
- `ML_Compute_VwapDev` — line 156
- `ML_Compute_PriceStddev` — line 161
- `ML_Compute_PriceAvg` — line 166
- `ML_Compute_VolumeAvg` — line 171
- `ML_Compute_EmaAboveSma` — line 176
- `ML_Compute_MidSlope` — line 183
- `ML_Compute_MidR2` — line 188
- `ML_Compute_CumDelta` — line 193
- `ML_Compute_HourSin` — line 198
- `ML_Compute_HourCos` — line 203
- `ML_Compute_VolRegimeRatio` — line 208
- `ML_Compute_TickRateZ` — line 213
- `ML_Compute_DistToHigh` — line 218
- `ML_Compute_DistToLow` — line 223
- `ML_Compute_BookImbMeanShort` — line 228
- `ML_Compute_BookImbMeanLong` — line 233
- `ML_Compute_BookImbDrift` — line 238
- `ML_Compute_Flow10s` — line 243
- `ML_Compute_Flow1m` — line 248
- `ML_Compute_Flow5m` — line 253
- `ML_Compute_LargeTradeZ` — line 258
- `ML_Compute_SpreadBps` — line 263
- `ML_Compute_SpreadZscore` — line 268
- `Features_PackAll` — line 449

### FeatureStandardizer.hpp

- `FeatureStandardizer_Init` — line 149 — recommended for clarity.
- `FeatureStandardizer_Apply` — line 180 — MLStrategy.hpp:129, PortfolioController.hpp:1639/1806).
- `FeatureStandardizer_Load` — line 218 — magic / num_features / embedded SHA.
- `FeatureStandardizer_VerifyAgainstBuild` — line 293 — per held_out_gate_strict.
- `FeatureStandardizer_Compute` — line 310 — from Backtest_TrainModel.
- `FeatureStandardizer_Persist` — line 350 — Returns 1 on success, 0 on I/O failure.
- `FeatureStandardizer_Free` — line 403 — the struct so future use re-initializes).

### FlowFeatures.hpp

- `BookImbHistory_Init` — line 66
- `BookImbHistory_Push` — line 75
- `BookImbHistory_MeanLong` — line 89
- `BookImbHistory_Last` — line 96
- `BookImbHistory_MeanShort` — line 105
- `FlowState_Init` — line 143
- `FlowState_Push` — line 156 — Full RegimeSignals→FPN cascade is a v5.11 ship (large blast radius).
- `LargeTradeState_Init` — line 213
- `LargeTradeState_Push` — line 223
- `LargeTradeState_ZScore` — line 246
- `LargeTradeState_Last` — line 262
- `SpreadState_Init` — line 293
- `SpreadState_Push` — line 303
- `SpreadState_ZScore` — line 321
- `SpreadState_Last` — line 336

### ModelInference.hpp

- `FeatureLookback_Max` — line 190 — used by: ValidationSplit (purge gap), PortfolioController (warmup check)
- `FeatureLookback_CountEnabled` — line 200 — count enabled features (for validation)
- `Model_Init` — line 288
- `Model_Load` — line 334
- `Model_Predict` — line 461
- `Model_Predict_Ensemble` — line 531
- `Model_Predict_Ensemble_Weighted` — line 604
- `Model_PredictMulti` — line 716
- `Model_Free` — line 771
- `Model_IsLoaded` — line 792
- `ModelFeatures_Pack` — line 820

### RewardTracker.hpp

- `RewardTracker_Init` — line 36
- `RewardTracker_Push` — line 41
- `RewardTracker_DrainCSV` — line 58 — append all pending records to CSV, then clear

### RollingStats.hpp

- `RollingStats_Push` — line 194
- `RollingStats_VolumeSignificant` — line 410
- `RollingStats_EntrySpacing` — line 423
- `RollingStats_BuyPrice` — line 440

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
- `GUI_Panel_Account` — line 881 — PANEL: ACCOUNT (merged Portfolio + P&L + Risk)
- `GUI_Panel_Config` — line 1120 — PANEL: CONFIG
- `GUI_Panel_Positions` — line 1168 — PANEL: POSITIONS — proper table with aligned columns
- `GUI_Panel_PerCorePnL` — line 1451 — Pure GUI thread, doesn't touch engine state.
- `GUI_Panel_Stats` — line 1547 — PANEL: STATS
- `GUI_Panel_Latency` — line 1637 — PANEL: LATENCY (conditional on LATENCY_PROFILING)
- `GUI_Panel_MLIntelligence` — line 1693 — PANEL: ML INTELLIGENCE — bandit arms, confidence, cost, model info
- `GUI_RenderDashboard` — line 1860

### EngineHeaderPanel.hpp

- `EngineHeader_Render` — line 37 — nullptr (legacy callers), only the 3 build-time fields render.

### FoxmlTheme.hpp

- `Foxml_ApplyTheme` — line 45

### GuiThread.hpp

- `Gui_Init` — line 84 — GUI INIT
- `Gui_Shutdown` — line 190 — GUI SHUTDOWN
- `Gui_BeginFrame` — line 203 — GUI FRAME
- `Gui_EndFrame` — line 227
- `Gui_SetupDefaultLayout` — line 242 — chart 60% left, dashboard panels stacked 40% right
- `Gui_HandleKeys` — line 292 — GUI KEYBOARD — same controls as ANSI TUI

### LogViewerPanel.hpp

- `LogViewer_Init` — line 22
- `LogViewer_Refresh` — line 28
- `GUI_Panel_LogViewer` — line 57

### MLStatusPanel.hpp

- `MLStatus_Render` — line 36 — nullptr; the row is rendered only when a swap is actually pending.

### SettingsPanel.hpp

- `Settings_RescanModels` — line 547 — stays free of opendir/stat (per /readiness check 17 hardening).
- `Settings_Init` — line 605 — so Settings_Load knows where to read.
- `Settings_Load` — line 615
- `Settings_RenderGlobalTab` — line 756 — GLOBAL TAB — renders the auto-generated field_defs[] layout
- `Settings_RenderPerCoreTab` — line 893
- `GUI_Panel_Settings` — line 1202 — running cores, not cfg-only intent — engine doesn't add/remove cores live.

### StrategyQualityPanel.hpp

- `StrategyQuality_Init` — line 59 — log path is passed at render time via GUI_Panel_StrategyQuality).
- `StrategyQuality_Refresh` — line 147
- `GUI_Panel_StrategyQuality` — line 224

### TradeHistoryPanel.hpp

- `TradeHistory_Init` — line 35
- `TradeHistory_Refresh` — line 40
- `GUI_Panel_TradeHistory` — line 172

### TradeReader.hpp

- `TradeData_Init` — line 39
- `TradeData_Refresh` — line 76

## Backtest/

### BacktestEngine.hpp

- `BacktestData_DetectFormat` — line 57 — timestamp_us,price,quantity,is_buyer_maker
- `BacktestData_Load` — line 64
- `HistoricalTick_CmpByTime` — line 135 — Caller in STRICT mode should treat -1 as "abort run".
- `BacktestData_ValidateSort` — line 143
- `BacktestResults_Init` — line 269
- `BacktestResults_Free` — line 281
- `BacktestResults_Reset` — line 309 — against zero capacity (defense-in-depth) but this is the load-bearing fix.
- `BacktestResults_EnsureCapacity` — line 332 — grow sample buffers by 2x when full
- `BacktestResults_EnsureEquityCapacity` — line 359 — array, so silent truncation produces wrong Sharpe / max DD / return.
- `XGBoost_ComputeScalePosWeight` — line 392 — (0.0 = negative, 1.0 = positive, 0.5 = neutral and already filtered).
- `XGBoost_ComputeMulticlassWeights` — line 424 — receives per-class sample counts so caller can log them.
- `BacktestStats_Compute` — line 460 — fabs() inconsistency + 2-site max_drawdown reimplementation.
- `BacktestStats_ComputeFromEquity` — line 493 — sharpe — needs equity curve data too
- `BacktestSharded_Run` — line 531
- `Backtest_ComputeLabelsFromSamples` — line 579 — through samples; no per-file O(N) sample scans.
- `Backtest_Run` — line 839 — equity curve).
- `HeldOutSplit_TrainEval` — line 960 — helper has visibility into WalkForward_Compute* and XGBoost_Compute* funcs.
- `Backtest_RunWalkForward` — line 1010 — behavior bytewise.
- `Backtest_RunFullValidation` — line 1019
- `WalkForward_ComputeAccuracy` — line 1246 — uses > 0.5f for truth so neutral (0.5) labels are never counted as positive
- `WalkForward_ComputeMulticlassAccuracy` — line 1293 — argmax over each row, compare to integer truth (rounded from label float).
- `WalkForward_ComputeMSE` — line 1312 — regression: mean squared error. Lower = better. Sensitive to outliers.
- `WalkForward_ComputeCorrelation` — line 1328 — gets low MSE on small-magnitude targets while having zero predictive power).
- `Backtest_RunWalkForward` — line 1352
- `HeldOutSplit_TrainEval` — line 1782 — functions it uses (WalkForward_Compute*, XGBoost_Compute*) are visible.
- `ConfigField_Set` — line 2007 — handles both FPN and PCT fields (PCT keys are stored as decimal, value comes in as %).
- `Backtest_RunSweep` — line 2111
- `Backtest_RunHyperparamTrainSweep` — line 2201 — mean_val_correlation (regression). Stored as positive number; higher = better.

### BacktestPanels.hpp

- `DataPanel_Init` — line 44
- `DataPanel_Scan` — line 49
- `RunControl_Init` — line 150
- `SamplesSnapshot_Compute` — line 161 — only when running==0, giving a safe happens-before relationship.
- `RunControl_Start` — line 232
- `GUI_Panel_DataBrowser` — line 272
- `GUI_Panel_RunControl` — line 371
- `GUI_Panel_Results` — line 420
- `PastRuns_Init` — line 604
- `PastRuns_LoadOne` — line 643 — scan one run directory's metadata files
- `PastRuns_ScanOneDir` — line 730 — backward compat with flat models/{run_name}/ runs from before v4.3.
- `PastRuns_Scan` — line 745
- `PastRun_MetricLabel` — line 760 — label-type-aware metric label
- `GUI_Panel_PastRuns` — line 766
- `Comparison_Init` — line 1431
- `Comparison_Free` — line 1435
- `Comparison_SaveRun` — line 1442
- `GUI_Panel_Comparison` — line 1483
- `OptimizerPanel_Init` — line 1633
- `GUI_Panel_Optimizer` — line 1665
- `TrainingPanel_Init` — line 2006
- `GUI_Panel_Training` — line 3075

### BacktestSharded.hpp

- `SharedBacktest_FromHistorical` — line 78
- `BacktestSharded_Run` — line 102 — aggregates results.

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

- `HeldOutSplit_GenToken` — line 77 — non-reproducible by construction. Removed.
- `HeldOutSplit_Make` — line 92
- `HeldOutSplit_TestAccessAllowed` — line 125
- `HeldOutSplit_Unlock` — line 132 — Logs unlock event to stderr — caller can also Notify_Send for audit trail.
- `HeldOutSplit_Relock` — line 154 — not _Relock.

### LabelFunctions.hpp

- `Label_WinLoss` — line 73 — no trade was entered at that point.
- `Label_Barrier` — line 94 — same as win/loss but with configurable asymmetric barriers.
- `Label_ForwardPnl` — line 112 — useful for regression (predict magnitude, not just direction).
- `Label_Regime` — line 129 — useful for training a regime classifier model.
- `Label_VolBarrier` — line 152 — source: ~/FoxML/private/DATA_PROCESSING/targets/barrier.py
- `LabelType_NumClasses` — line 362 — ≥2 = multiclass softmax       (label values 0..K-1 as floats)
- `LabelType_IsBinary` — line 367
- `LabelType_IsRegression` — line 371
- `LabelType_IsMulticlass` — line 375

### OverfitDetection.hpp

- `OverfitDetection_Check` — line 68 — feat_cap:     feature count cap (0 to disable)
- `OverfitDetection_CheckDefaults` — line 133 — convenience: check with default FoxML thresholds
- `OverfitDetection_CheckRegression` — line 159 — interpretation is correlation-space. Both tunable separately if needed.
- `OverfitDetection_CheckRegressionDefaults` — line 210
- `OverfitDetection_CountOverfit` — line 221 — returns: number of folds flagged as overfit
- `OverfitDetection_Print` — line 230 — print report (for logging / debugging)

### PhaseTimers.hpp

- `PhaseTimer_Global` — line 48 — header inline-only (no separate .cpp).
- `PhaseTimer_Reset` — line 59
- `PhaseTimer_Summary` — line 75 — wf_eval / held_out_eval since it's nested inside both.
- `PhaseTimer_PopulateSnapshot` — line 117

### ValidationSplit.hpp

- `PurgeGap_Compute` — line 66 — first test tick to prevent any form of temporal leakage.
- `PurgeGap_ComputeExplicit` — line 73 — overload: caller provides explicit max_lookback (for testing or custom feature sets)
- `ValidationSplit_Generate` — line 126 — returns: number of valid folds generated (may be < n_splits if early folds skipped)
- `ValidationSplit_GenerateExplicit` — line 230 — used by walk-forward when splitting in non-neutral sample space where raw lookback doesn't apply
- `ValidationSplit_Verify` — line 296 — returns 1 if all folds are clean, 0 if leakage detected
- `ValidationSplit_Print` — line 317 — print fold summary (for logging / debugging)

### XGBHyperparams.hpp

- `XGBHyperparams_Defaults` — line 52 — modify the returned struct in-place.
- `XGBHyperparams_Apply` — line 62 — use 1 (deterministic per-fold output). Caller chooses.

## tests/

---

## Top-level files

- `main.cpp` — 1270 lines
- `Version.hpp` — 8 lines
- `Limits.hpp` — 30 lines

## Conventions

- Function names follow `Pattern_FunctionName` convention (e.g. `Portfolio_Init`, `BG_Evaluate`)
- Headers are inline-heavy — most functions live in `.hpp` and are `inline`
- Templates parameterize on `unsigned F` (FPN word count), default `F=64` (4096-bit)
- Lowercase helpers (`fan_out`, `drain_with_submit`) are local to a function and not in this map
- ALL_CAPS macros are not in this map; see headers directly
