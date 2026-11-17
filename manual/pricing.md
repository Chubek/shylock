# Evaluated forecasts, pricing policies and stay quotes

The pricing engine now exposes configurable MuLIB forecasts, chronological model
selection and holdout evaluation, empirical error bands, scenario-risk demand
optimization, guarded forecast/formula/demand decisions and checked tax/fee quotes.
These are offline proposals, never automatic rate publication or booking truth.

To connect Dragnet discoveries and reviewed evidence directly, see
[collections and ledger pricing](dragnet-pricing.md): `forecast-ledger` and
`price-ledger` share the existing engine/policies while enforcing explicit
selection freshness and coverage before forecasting or recommending.

```sh
./shylock forecast tests/fixtures/series.json policies/forecast.json
./shylock forecast tests/fixtures/seasonal-history.json policies/seasonal.json
./shylock optimize-model policies/model.json
./shylock quote policies/quote.json
```

Existing `estimate`, `backtest`, `recommend`, `adjust` and `optimize` commands and
their v1 inputs/results remain supported. The new workflows have separate,
versioned schemas; no implicit migration or database rewrite occurs.

To produce a suitably populated reviewed cohort and all downstream artifacts:

```sh
python3 examples/offline-pricing.py /tmp/shylock-pricing-example
```

This deliberately synthetic fixture workflow creates a **new** output directory,
12 verified captures/reviews, cohort, forecast, formula/demand decisions, scenario
risk result and three `.tex` reports. It refuses existing output directories and
does not fetch live data, call a model or publish rates. The bundled pricing
policy needs at least seven samples; the original two-sample `recommend` example
alone is not enough for separate model selection and validation.

## Forecast policy and chronological evaluation

`forecast SERIES_JSON FORECAST_POLICY_JSON` accepts the same legacy series or
validated `shylock.cohort/v1` export as `estimate`. Use reviewed cohorts when
currency, context, sampling and evidence lineage matter. Legacy arrays assert
equal spacing but cannot establish it or authenticate missing context.

`policies/forecast.json` is a complete `shylock.forecast-policy/v1` example.
All fields are required, with unknown fields and types rejected:

| Field | Contract |
| --- | --- |
| `horizon` | 1..365 sample intervals, not necessarily days |
| `minimum_training` | >=2, <=window; every model must have enough initial samples |
| `window` | 2..1024 most recent prefix samples used by each fit |
| `selection_samples` | 1..64 chronological targets for comparing candidates |
| `validation_samples` | 1..64 later targets, never used to choose the winner |
| `interval_bp` | 1..9999 absolute-error quantile, in basis points |
| `market_method` | median, trimmed, winsorized, huber, quantile or kde |
| `market_parameter_ppm` | parameter divided by 1,000,000; zero for median/huber |
| `candidates` | 1..16 named model configurations; unique 1..64-byte names |

Trim/winsor parameters are below 0.5, quantile parameters in [0,1], and KDE
bandwidth is nonnegative (zero requests MuLIB's robust automatic bandwidth).
The market reference is a summary of the supplied history, not a new independent
cross-sectional market survey or a demand estimate.

Each candidate contains `name`, `model`, and only its model-specific parameters:

| Model | Parameters and additional limits |
| --- | --- |
| `last` | Last-value baseline, none |
| `median` | MuLIB median level baseline, none |
| `ema` | `alpha_bp`, 0..10000 |
| `holt` | `alpha_bp`, `beta_bp` in 0..10000; `phi_bp` in 1..10000 |
| `seasonal` | `alpha_bp`, `beta_bp`, `gamma_bp` in 0..10000; `season` 2..512; minimum/window >=2 cycles |
| `kalman` | nonnegative integer `process_variance`, `measurement_variance`, not both zero; squared minor units |
| `ar1` | Stationary AR(1), minimum training >=3; invalid/nonstationary fits are reported |
| `robust-trend` | MuLIB Theil-Sen; window <=128 to bound repeated quadratic fits |

For N samples, S selection targets, V validation targets and horizon H:

1. Selection targets are indexes `[N-V-S, N-V)`.
2. A target at index t uses only the prefix of length `t-H+1`; the fitted window
   is the last `min(prefix_length, window)` values. No fold sees its target.
3. Candidates must evaluate **all** selection folds. Partial coverage cannot
   make a model look artificially better. Selection minimizes MAE, then RMSE,
   then retains original candidate order for exact ties.
4. Freeze that winner before evaluating validation targets `[N-V, N)`. Validation
   is expanding/prequential: later folds may use earlier observed holdout values,
   but never their own target. Validation does not retune the selected parameters.
5. Fit the winner to the final historical window and forecast H intervals ahead.

The input must satisfy `N-V-S-H+1 >= minimum_training`. Every source sample is
nonnegative integer minor units <=1e12. All numerical fitting uses canonical
MuLIB histprice/statsprice kernels; On1x owns validation, cutoffs and selection.

`shylock.forecast/v2` includes policy/forecast IDs, raw series digest, complete
cohort lineage when present, all selection folds, the selected configuration,
separate validation coverage/MAE/RMSE/bias, final forecast/reference, historical
market reference and model observation variance when available (Kalman).
Missing variance is null, not falsely zero. A model failure remains an explicit
failed fold. No eligible model, incomplete validation, or an invalid final fit
returns an error forecast with exit 2 rather than silently swapping estimators.

The interval uses the winner's validation absolute errors and the target rank
`ceil(interval_bp * V / 10000)`. Its band is centered on the final point and
clipped to [0,1e12]. This is a **descriptive empirical error band**, not a normal
confidence interval, a conformal coverage guarantee, or independently backtested
interval coverage. Small holdouts, selection bias, seasonality and dependent
errors can make it misleading. Evaluate on suitable histories before deployment.

Use `reports/evaluated-forecast.json` to typeset successful v2 forecasts.

## Checked stay quotes

`quote QUOTE_INPUT_JSON` accepts `policies/quote.json`: schema
`shylock.quote-input/v1`, explicit `currency`, `scale`, `rate_minor`, and `charges`.
Charges contain exactly `nights` (1..365), `cleaning_minor`, `other_fees_minor`,
`tax_bp` (0..10000), `tax_mode` (`inclusive`/`exclusive`) and `tax_scope`
(`accommodation`/`accommodation_and_fees`). Amounts must be nonnegative integers
<=1e12 at a supported currency scale. Currency conversion is not performed.

MuLIB money performs checked multiplication, addition, subtraction and rational
scaling. Tax is rounded once on the entire declared stay tax base, half upward:

- Accommodation = nightly rate times nights; fees are once-per-stay amounts.
- Exclusive tax = taxable amount * tax_bp / 10000; add it to the quote total.
- Inclusive tax = taxable amount * tax_bp / (10000 + tax_bp); identify it within
  the unchanged total rather than charging it again.

All intermediates and totals must stay in [0,1e12]; overflow/limits fail, never
wrap or silently round via floating-point money. `shylock.quote/v1` preserves
currency/scale, source rate/rules, accommodation, fees, tax base, tax, subtotal
excluding tax, total, rounding/runtime contract and `quote_id`.

This is a declared calculation, not jurisdictional tax compliance. No exemption,
compound tax, remittance, FX, accounting approval or unknown-fee inference is
claimed. A nightly quote, cleaning fee and stay total remain different fields.

## Unified policy decisions

`price COHORT_JSON PRICING_POLICY_JSON [DEMAND_JSON]` connects a **reviewed cohort**
to evaluated forecasting, a restricted Mune formula, rate guards, optional replayed
demand optimization, and a complete stay quote. `policies/pricing.json` is the
full example. Formula paths resolve relative to that policy file; identity
behavior can be requested explicitly with `formulas/identity.lua`.

The closed policy includes schema `shylock.pricing-policy/v1`, currency, scale,
`as_of`, nested forecast policy, `formula` (path, Boolean weekend, premium_bp),
`constraints`, `decision` (`formula` or `demand`) and the charge rules above.

Constraints are explicit floor/cap/tick, `unit_cost_minor`,
`minimum_margin_minor`, `previous_minor`, `max_change_bp` (0..10000) and objective
(`profit` or `revenue`). Cost plus minimum margin raises the floor. Movement
limits are computed with checked-size integer expressions and rounded inward:

- previous=0 disables movement limits and requires max_change_bp=0;
- previous>0 and max_change_bp=0 **freezes** the rate;
- otherwise use the previous rate +/- the specified basis-point change.

The final interval is intersected with floor/cap and snapped inward to the tick
grid. Empty grids are rejected, never weakened. The formula's proposal is rounded
and clamped with the same On1x grid logic as `adjust`.

Additional admission checks prevent accidental context mixing:

- Require a valid provenance-bearing cohort, matching currency/scale, exact stay
  night count (Gregorian month/year/leap boundaries), and a stay not before the
  UTC date of `as_of`.
- Every capture must be at or before `as_of`. Quote tax mode must match the
  cohort's included/excluded tax scope. Fees already included in cohort rates
  cannot have extra fees silently added a second time.
- `decision=demand` requires a replayable `shylock.demand/v1` artifact; legacy
  unlabelled demand arrays remain available only to the old `optimize` workflow.
- Demand must replay exactly and match cohort name, currency/scale, guests and
  tax/fee scopes. Its own cutoff may not exceed `as_of`.

The cutoff contract is **capture-and-finalization/v1**: timestamps are caller
assertions. It does not authenticate time or prove that the current review
revision was available historically. Real historical evaluation must provide
appropriate historical review snapshots; later corrections can otherwise leak
knowledge despite earlier capture timestamps.

In demand mode MuLIB chooses among the actual observed rate candidates on the
allowed grid. Formula-adjusted and demand-selected rates stay separate; no
interpolation or fabricated demand is introduced. The objective is in
**quoted-nightly-rate space**, using cost in that same declared basis. Later stay
tax/fee calculation is separate: objective scores are not after-tax accounting
profit, realized income or proof of demand causality.

One `shylock.pricing-decision/v1` retains policy/decision IDs, complete evaluated
forecast/cohort lineage, formula inputs/source/runtime/backend/error, proposal,
formula-adjusted rate, optional demand lineage/optimization, and identified quote.
Failures have explicit `forecast_error`, `formula_error`, `demand_error` or
`quote_error`, exit 2, and a null final recommendation/quote. Earlier forecasts
and calculations remain attached. No silent formula fallback on failed demand.
Malformed/incompatible input and allocation/publication failures may instead
return exit 2 without an artifact. No rate is written to a remote provider.

Forecast and final decision events use the existing opt-in extension lifecycle;
hooks run after domain owners/locks are released and cannot replace core results.
Use `reports/pricing.json` for successful decisions.

## Externally specified demand models and risk

`optimize-model MODEL_POLICY_JSON` uses schema `shylock.model-policy/v1`, explicit
currency/scale, the same constraints, `risk_bp` (0..100000), and 1..16 named models:
`name`, `kind`, `scale_units`, `shape_ppm`, `reference_minor`, `weight_bp`.
Names must be unique. Scales/weights are bounded to 1e6; total weight must be
positive. Weights need not sum to 10000. `shape_ppm` is divided by 1e6:

- linear: max(0, scale - shape * rate), reference must be zero;
- isoelastic: scale * (rate/reference)^(-shape), positive reference/rates;
- logistic: scale/(1+exp((rate-reference)/shape)), positive shape.

The scale is model-specific, not a universal capacity bound: isoelastic demand
can exceed its reference quantity at lower rates. Models are caller-specified,
not fitted from asking quotes. Search evaluates every feasible tick, at most
100000 candidates. MuLIB scores weighted expected profit/revenue minus
`risk_bp/10000 * objective_stddev`, with deterministic lower-rate tie breaking.
Movement, margin and tick guards remain hard constraints, including zero-change
freezes. Zero-weight models are still validated but do not constrain the domain.

`shylock.optimization/v2` preserves the exact model policy, runtime/IDs, effective
grid, selected rate, expected demand, expected quoted-rate revenue/profit,
separate risk-adjusted score and evaluation count. This is model sensitivity
analysis, not an identified causal demand curve or a guarantee of bookings.

## Verification and implementation boundary

On1x modules `forecast.on1x`, `pricing.on1x`, `model.on1x` and `quote.on1x` own
schemas, admission, chronological windows/selection, monetary scope, movement
policy and orchestration. C11 `engine.c` owns copied samples, JSON/fold/lineage
lifetimes and MuLIB numeric calls. It borrows no resource after its session closes.
The native scalar bridge remains trusted; no upstream patch or second MuLIB
implementation translation unit was introduced.

`tests/engine.py` exercises every model family and market method, disjoint
selection/holdout targets, prefix-only replay, coverage failures, quote scales
and rounding, brute-force risk comparison, freezes/ticks, verified cohort-to-price
decisions, replay tampering, failure preservation and sandbox parity. Native and
injected allocator tests cover session teardown and failed output callbacks.
