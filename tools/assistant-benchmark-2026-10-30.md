# Studio assistant model benchmark — October 30, 2026

Requested configurations: **GPT-6 Luna / Max**, **GPT-6.1 Terra / High**, and
**GPT-6.1 Sol / Medium**. These levels are explicitly sent in every model request;
no default reasoning level or substituted model is used.

## Task and method

Each fresh trial starts with two synthetic models on one 180 mm plate: an
earring rotated 30 degrees, with a connected loop, and a separate overhanging
stand. The same prompt asks the assistant to replace the loop with a round
2 mm through-hole centered across the body and 3 mm below its new top edge,
paint manual supports only beneath the stand's top, preserve both models'
dimensions/pose, slice, and verify the hole and generated supports.

There are two runs per configuration, executed sequentially in reverse-balanced
order: Sol, Terra, Luna, Luna, Terra, Sol. Each run uses the same fresh scene,
instructions, tool schemas and grader, with a five-minute limit. No feedback,
prompt changes, manual corrections or model-specific hints are supplied between
runs. Source and executable hashes are frozen and checked by the runner.

The main time is **the entire assistant turn**, including model/network requests,
native tool execution and its final response. Login, catalog checks, fixture
creation, independent grading and artifact export are excluded. CLI wall time
and model/network versus local-tool time are recorded separately. Model/network
time is not a measurement of reasoning compute alone.

The executable uses Studio's actual assistant instructions and session loop,
native geometry/painting helpers, and native slicer, with an isolated headless
dispatcher. It does not exercise the GUI event loop or every UI-only operation.
Any unsupported fixture operation makes a trial inconclusive for comparison,
rather than a model capability failure. Shared projects and printers are unused.

## Independent checks

- Exactly two objects remain; both reference-instance poses are preserved.
- The earring's retained body is 20×30×3 mm with its rectangular section intact.
- A round 2 mm hole is centered correctly and passes through three checked depths.
- The stand's mesh, part transforms and instance pose are unchanged.
- Support enforcement covers its 396 mm² underside and no other surface.
- Manual support settings are enabled, actual stand support layers exist, and
  the slice is fresh.

Before real requests, a scripted solution passed all checks. Negative controls
confirmed that the grader rejects a filled hole and disabled support settings.
An initial pilot exposed an overly narrow settings check: valid object-only
manual supports were compared against global settings. The harness now exposes
object overrides and grades effective per-object settings, with regression
checks for both valid scopes. The pilot and interrupted next trial are preserved
separately under `build/assistant-comparison-20261030-pilot` and excluded from
the six frozen comparison runs. No model instructions or task hints changed.
This uses outcome-based checks rather than asking another model to grade the
answer, consistent with [OpenAI's evaluation guidance](https://developers.openai.com/api/docs/guides/evaluation-best-practices).

## Results

All six requests used their exact requested model and effort. All six initial
contexts and assistant instructions matched by hash; no trial encountered an
unimplemented fixture operation.

| Configuration | Run 1 | Run 2 | Finished correctly within five minutes | Native tool calls |
| --- | ---: | ---: | ---: | --- |
| GPT-6.1 Terra High | 82.3 s | 78.7 s | 2/2 | 19, 18 |
| GPT-6.1 Sol Medium | 105.5 s | 90.3 s | 2/2 | 20, 18 |
| GPT-6 Luna Max | ≥300 s, stopped | ≥300 s, stopped | 0/2 | 25, 20 before cancellation |

Terra's mean completed-turn time was **80.5 seconds**, versus **97.9 seconds**
for Sol (about 18% less elapsed time). Both passed every physical/settings check
with no tool errors. Local native-tool execution took only 0.17–0.23 seconds per
successful trial; virtually all elapsed time was model/API/network time.

Luna's first run had correct geometry, supports and a fresh slice, with zero tool
errors, but did not finish its final answer before the deadline. Its second run
finished the earring repair but had not enabled/painted/generated the stand's
supports when canceled; it also encountered two native cut errors. The recorded
300.7/300.1 seconds include cancellation latency and **are not completed-answer
times**. A mean completion time for Luna cannot be estimated from these trials.

**Interpretation:** Terra High was the strongest configuration for this workflow
in this small test. Sol Medium also completed it reliably in both repetitions.
Luna Max showed greater latency here; the second failure is additionally
confounded by a real native-tool defect, not simply poor geometric reasoning.

Raw reports, event logs, views and geometry artifacts are under
`build/assistant-comparison-20261030`; the combined machine-readable record is
`summary.json` in that directory. No failed trial was corrected or silently
retried as part of its score.

To repeat the full comparison, first close Studio (the diagnostic refuses to
share an active account session with it), then use a new output directory:

```sh
python3 tools/run_studio_assistant_model_checks.py \
  --test-source tools/assistant_live_client_check.cpp \
  --check-name assistant-benchmark-20261030 --build-only
python3 tools/sign_macos_app.py sign \
  build/arm64/assistant-benchmark-20261030/model-checks \
  --identifier com.bambulab.bambu-studio
python3 tools/run_assistant_comparison.py \
  build/arm64/assistant-benchmark-20261030/model-checks \
  build/assistant-comparison-new
```

## Native cut defect found during interpretation

An offline check reproduced a rejection of Luna's mathematically valid cut.
Two points describing the same rotated plane (offset difference only
1.8×10⁻¹⁵ mm) produced different results: one removed the intended 108 mm³ loop,
while the other raised “This plane removes no material.” This was reproduced
directly against `prepare_plane_cut`, outside the model and benchmark dispatcher.
It establishes numerical sensitivity in the shared native tool. The model did
not invent an invalid plane in that attempt. The existing implementation was
left unchanged throughout all six timed trials.

Reproduce without an account or model request:

```sh
python3 tools/run_studio_assistant_model_checks.py \
  --test-source tools/assistant_cut_tangent_checks.cpp \
  --check-name assistant-cut-tangent-checks
```

The diagnostic reports both outcomes as JSON; it intentionally returns normally
so either observation can be inspected. `all_passed: false` is the reproduced
defect, not a passing regression claim. Output is recorded in
`build/arm64/assistant-cut-tangent-checks/results.log`.

Two repetitions of one task are a practical spot check, not an estimate of
general model reliability. Request timing includes variable service/network
latency. No assistant default or installed app is changed by this benchmark.
