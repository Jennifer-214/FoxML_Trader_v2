# Coding Standards

Rules and patterns specific to the tick_trader codebase.

## Code Style

- C-style with templates, no classes (free functions with struct pointers)
- `using namespace std;` used throughout
- Functions: `Module_FunctionName` (e.g. `Portfolio_Init`, `BuyGate`)
- Templates: `template <unsigned F>` for fixed-point precision parameter
- User's inline comments must be preserved exactly when editing

## Hot Path Rules

- **No branches** — use branchless mask-select: `mask = -(uint64_t)condition`
- **No allocation** — all memory pre-allocated at init
- **No I/O** — trade log uses ring buffer, CSV drain on slow path
- **No float** — all math uses `FPN<F>` fixed-point
- **Conditional writes only for rare events** — fills (~1/1000 ticks) and exits use
  predicted branches, not unconditional writes

## Fixed Point (FPN<F>)

- `FPN<64>` = 128-bit magnitude (2 × uint64_t) + int32_t sign
- All arithmetic branchless: `FPN_AddSat`, `FPN_Mul`, `FPN_DivNoAssert`
- Conversion: `FPN_FromDouble<F>(val)`, `FPN_ToDouble(fpn)`
- Zero: `FPN_Zero<F>()`
- Comparisons return int (0 or 1), suitable for mask generation

## Bitmap Patterns

- Portfolio: `uint16_t active_bitmap` — 16 position slots
- OrderPool: `uint64_t bitmap` — 64 order slots
- Walk: `while (bm) { int idx = __builtin_ctz(bm); ... bm &= bm - 1; }`
- Count: `__builtin_popcount(bitmap)`
- Full check: `bitmap == 0xFFFF` (portfolio) or popcount >= capacity

## Config Pattern

- Struct: `ControllerConfig<F>` with FPN fields
- Defaults: `ControllerConfig_Default<F>()`
- Parser: `ControllerConfig_Load<F>(filepath)` — strcmp chain
- Percentages in config file (e.g. `5.00`) divided by 100 in parser
- Hot reload: `PortfolioController_HotReload(ctrl, new_cfg)` — one function for all paths

## Snapshot Pattern

- Binary format with magic number + version
- Version check on load, reject old/incompatible
- Backward compat: load older version with defaults for missing fields
- Save on: slow path cycle, before reconnect, on quit

## Testing

- `tests/controller_test.cpp` — 101 assertions
- Pattern: setup config → init controller → push ticks → check state
- Zero-init controllers: `PortfolioController<FP> ctrl = {};`
- Tests that use noisy data: set `regime_volatile_stddev` high to prevent
  regime detector from triggering VOLATILE on test data
