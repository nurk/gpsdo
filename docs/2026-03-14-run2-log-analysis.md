# Log Analysis — 2026-03-14-run2.log

## Summary

Your observation is correct: **LOCKED is declared** (T1178), **but the DAC and `iAccumulator` are still drifting downward** and the TIC sawtooth is still full-width. The loop has not converged — it has merely not been *kicked out* of the lock window recently. This is a sign of a loop gain imbalance, not a successful lock.

---

## Phase 1 — WARMUP (T12 → T603)

- DAC fixed at 32767 / 2.5000 V. `iAccumulator` frozen at 32767.5. ✅
- TIC sawtooth is clean, full ±500 count swing (free-running OCXO at ~170 ppb offset). ✅
- EMA filter converging from −145 at T12 toward ~−50 by T603. ✅

---

## Phase 2 — RUN begins (T604 onward)

### iAccumulator drift (I-term working)

| Time | iAccumulator | DAC Value (base) |
|------|-------------|------------------|
| T604 | ~32767 | 32767 |
| T680 | ~31899 | ~31900 |
| T850 | ~29611 | ~29611 |
| T1000 | ~26973 | ~26973 |
| T1178 | ~24282 | ~24282 |
| T1228 | ~24250 | ~24250 |

- Drift rate: **~−11 to −13 counts/tick** from T604–T900.
- Drift rate **slows markedly** from ~T900 onward (~−7 to −8 counts/tick).
- Drift rate nearly stops around T1100–T1180 (~−3 to −5 counts/tick).
- `iAccumulator` appears to be **converging toward ~24,000–24,500**, which corresponds to **~1.87–1.88 V EFC**. This is the OCXO's actual null-frequency operating point.

### DAC output behaviour (P-term dominating)

Every tick the reported DAC Value alternates dramatically:

```
T693: DAC Value 33713  (iAcc=31713 + pTerm ≈ +2000)
T694: DAC Value 29697  (iAcc=31697 + pTerm ≈ −2000)
T695: DAC Value 33684  (iAcc=31684 + pTerm ≈ +2000)
```

The **P-term clamp is hitting ±2000 counts every single tick** because `ticFrequencyError` alternates ±500–900 counts with the sawtooth. The base `iAccumulator` is the "true" converging value — the reported `DAC Value` is `iAccumulator ± 2000`, swinging visibly.

This swing is **expected while the sawtooth is still wide** (170 ppb offset → full TIC range), but means the OCXO EFC is being kicked ±0.15 V every second — which makes it harder to converge.

---

## Phase 3 — Apparent convergence / TIC sawtooth compression (T900–T1178)

From ~T900 onward, the TIC sawtooth starts **compressing noticeably**:

| Time range | TIC peak values | Sawtooth width |
|------------|----------------|----------------|
| T604–T850  | 0–850 (full range) | ~850 counts |
| T900–T1000 | ~0–800 | ~800 counts |
| T1000–T1077 | ~0–800 (ramps continuing) | still wide |
| T1078+ | sawtooth wraps at lower peak (~800 → ~400 → wraps) | **compressing** |

By ~T1050–T1180, individual TIC values are **no longer reaching 800–900** on the high side; they run from ~15 to ~800, then from ~200 to ~800, suggesting the OCXO is pulling toward the correct frequency. The sawtooth period appears longer (less drift per second).

### The filtered value

`ticCorrectedNetValueFiltered` trends:
- T604: ~−90 (below zero — OCXO slow/high EFC needed)
- T850: ~−155
- T1000: ~−163
- T1050: ~−400 (filter still lagging badly — EMA too slow relative to how far off the OCXO was)
- T1100: ~−88 (recovering — OCXO has pulled in)
- T1140: ~−42
- T1178 (**LOCKED declared**): `filtered = −37.24`, `lockCount = 32`
- T1200–T1228: filtered oscillates between −35 and +60 — **crossing zero!**

---

## What "LOCKED but DAC not settled" actually means

### The real issue: LOCKED is declared too early / on wrong metric

At T1178 the filter is at −37 (inside ±50 LOCK_THRESHOLD for 32 consecutive seconds = 2×16 = 32). Lock is **correctly declared by the algorithm**, but:

1. **`iAccumulator` is still drifting** — from T1100 (~24560) to T1228 (~24250), it has moved another ~310 counts downward. Rate has slowed to ~−3 counts/tick, but has not stopped.

2. **The DAC output is still oscillating ±2000 counts every tick** due to the P-term. Even though `iAccumulator ≈ 24250`, the reported DAC swings between ~22250 and ~26250 each second, because `ticFrequencyError` is still ±500–900 (sawtooth still present).

3. **The sawtooth is compressing but not gone.** Near end of log (~T1200–T1228), TIC values run ~250–800. The sawtooth is still ~500–600 counts wide — compared to ~850 at the start of RUN mode. Progress, but not converged.

---

## Root Cause Analysis

### Why is convergence so slow?

**Estimated loop setpoint: ~24,000 DAC counts (≈1.83 V EFC)**

The OCXO started at 32767 (2.5 V). The I-term has to wind down ~8700 counts at ~10 counts/tick → **requires ~870 seconds** just in the I-term alone, with no other disturbances. The log runs to T1228 = ~624 seconds into RUN mode — so the I-term has **not yet fully converged** in this run. It needs roughly 250 more seconds.

### Why does the filter oscillate ±50 even at lock?

The sawtooth is still running (OCXO slightly off-frequency) so every tick the TIC jumps by ~70–90 counts. The EMA with `filterConst=16` smooths this but can't fully eliminate it. The filtered value swings between roughly −50 and +60 near the end of the log.

### Is the lock declaration valid?

**Technically yes** (algorithm criteria met), but it's a **premature lock**. The loop has not converged — it has entered the lock window while the filter is near zero, but the I-term is still moving and the OCXO is not yet at the correct frequency. If the run continued, the lock would likely hold (lockCount stays at 32 throughout the rest of the log) while the I-term continues its slow approach to the true setpoint.

---

## Key Numbers for the Report

| Metric | Value |
|--------|-------|
| RUN start | T604 |
| LOCKED declared | T1178 |
| `iAccumulator` at LOCKED | 24,282 |
| `iAccumulator` at end of log (T1228) | ~24,250 |
| Expected final setpoint | ~23,500–24,000 (still converging) |
| DAC swing at lock due to P-term | ±2000 counts (±0.153 V) |
| Filtered phase error at lock | −37 counts |
| Sawtooth width at end of log | ~500–600 counts (still present) |
| Time to full I-term convergence (estimated) | ~250–300 more seconds needed |

---

## Conclusions and Next Steps

1. **The loop is working correctly** — iAccumulator is converging, the OCXO is pulling in, lock was detected. ✅

2. **The DAC appears "unsettled" because the P-term alternates ±2000 every tick** while the TIC sawtooth is still running. This is expected and correct — P-term is damping, not setting the frequency. The *actual* EFC control is the I-term base.

3. **Extend the run** — another 300 seconds should see the sawtooth compress to near-zero, P-term swings shrink dramatically, and the DAC settle to ~1.83 V. This would be the true locked state.

4. **Consider reducing `PTERM_MAX_COUNTS`** from 2000 to something smaller (e.g. 500) once the I-term is converging well. A ±2000 count kick every second is fighting the I-term while the sawtooth is wide.

5. **The `ticFilterConst=16` combined with `lockCount≥32` is appropriate** — lock was declared after 32 consecutive in-range seconds, which is correct. It just happens that the I-term is still converging at that point.
