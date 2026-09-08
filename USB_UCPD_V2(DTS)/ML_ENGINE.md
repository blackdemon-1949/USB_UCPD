# ML Engine

`apie_ml.c` implements a small, bounded, evidence-driven embedded ML layer. It
is deliberately conservative: **safety logic always overrides ML**, ML is never
used for deterministic protocol parsing, and there are **no random weights and
no invented training claims**.

## Design

- **Model**: online Naive Bayes with a logistic decision head.
  - Classes: `useful` vs `not-useful` for an informational query.
  - Features: query id, attempt count, has-PPS, hard-identity-known, plus the
    bounded feature vector from `apie_analyzer.c` (`APIE_FEATURE_COUNT=12`).
  - Priors are Laplace-smoothed and start at uniform — the model is driven by
    **online evidence**, not a frozen seed.
- **Decision-tree classifier** (`APIE_Tree_ClassifyUseful`): a compact,
  interpretable rule tree with evidence-justified thresholds (has-PPS → is
  Get_PPS_Status useful, etc.).  Deterministic, no random weights.
- **Anomaly detector** (`APIE_Ml_Anomaly_*`): online Gaussian (Welford) mean/std
  over transaction latency; flags a value deviating by more than *k* sigma.
  Never affects safety decisions.
- **Metadata** (`APIE_ModelMeta_t`): model id, version, feature version, CRC,
  kind, accuracy, training tag. Every model is validated on load/import.
- **Bounded state**: small count tables only; no unbounded history.

## Model validity vs. meaningful training

The task distinguishes two statuses and the firmware does too:

- `MODEL_VALID` — structure and metadata CRC are correct.
- `MODEL_MEANINGFULLY_TRAINED` — the model has seen enough real evidence to
  claim generalisation. A tiny synthetic set does **not** prove this.

The shipped model is `MODEL_VALID` but only `seed-online` trained; its
`accuracy` field is `0` (unknown) until real validation data is collected. The
host pipeline (`tools/train_apie.py`) produces a seed (`apie_model_seed.h/json`)
and the firmware re-learns online from real sources. Nothing is claimed as
`MODEL_MEANINGFULLY_TRAINED`.

## Training pipeline (host)

```
capture → feature extraction → dataset → labelling → training → validation
→ quantisation/package → firmware import
```
`tools/train_apie.py` is the host trainer and emits a model seed with metadata
(id, version, feature version, dataset, accuracy, CRC). Model metadata always
accompanies a model; a model is never shipped without its checksum.

## CLI

- `ml` / `ap ml` / `diag ml` — model id/version/CRC + class counts.
- `predict <q>` / `ap predict <q>` — classify query 0–8 as useful or not.

## Honesty rules

- ML can assist message-family classification, unknown-behaviour
  classification, anomaly detection, query usefulness, source behaviour
  classification, prediction, and experiment-candidate ranking.
- ML **never** auto-transmits unknown/vendor packets (R4 is compile-off).
- When evidence is insufficient the system reports **UNKNOWN** rather than
  inventing an answer.
