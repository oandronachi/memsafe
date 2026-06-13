# Spec Decisions Log

Appendable, traceable record of contested-spec issues -- each resolved by reviewer consensus (deliberation) or escalated to a human. The source files cite these decision IDs in comments.

## DEC-0001 -- CPP_MEMSAFE-0240-TEST -- 2026-06-13T08:04:23Z
- artifact: testing/tests/concurrency/test_slotmap_concurrency.cpp:85-92,1095-1150
- clause: CPP_MEMSAFE-0240-TEST acceptance: Relacy exhausts <=2-thread interleavings of the Treiber CAS pop/push pair; Relacy is consumed as vendored test code under testing/tests/concurrency/vendor/ and the test exits 0 on success; F2 Generational Handles case 4
- status: resolved-by-consensus (accept)
- positions:
    - cowork: approved
    - codex: approved
- decision: accept the candidate's implementation as correct as-is. Rationale: (reviewers converged; see ledger deliberation rows)


## DEC-0002 -- CPP_MEMSAFE-0910-TEST -- 2026-06-13T13:30:53Z
- artifact: testing/tests/perf/bench_primitives.cpp:84,135-141,179-184,219-223,259-264,300-304
- clause: CPP_MEMSAFE-0910-TEST acceptance: In the release lane (MEMSAFE_RELEASE_CHECKS=0), geometric-mean slowdown <=5% vs raw/unique_ptr/shared_ptr; exits non-zero if any required ratio exceeds 1.05 or if the geometric mean exceeds 1.05; F1 §NFR Performance
- status: resolved-by-consensus (change)
- positions:
    - codex: rework -- The benchmark gates ratios for workloads dominated by touch_payload(), not for the primitive operations themselves. touch_payload() performs 16 rounds of hash-like arithmetic and is included in both the baseline and candidate timing for Owner/MutRef, SlotMap/Handle, Arc, and Mutex. That common payload can dilute a primitive overhead regression above 5% into an in-budget workload ratio, so the test does not genuinely prove the declared <=5% slowdown criterion for the raw/unique_ptr/shared_ptr comparisons. I also found that the clean default validation outputs do not contain a bench_primitives executable: release uses tests/*.cpp while tests/perf/*.cpp is only listed under the optional perf lane, so the passed release validation did not execute this gate.
- decision: apply the agreed resolution: Revise bench_primitives.cpp so each measured operation isolates the primitive access/copy/lock/lookup cost with only the minimum mutation and optimizer barrier needed for correctness, or otherwise subtract/control the common payload before applying the 1.05 gate. Ensure the required MEMSAFE_RELEASE_. Rationale: codex: The benchmark gates ratios for workloads dominated by touch_payload(), not for the primitive operations themselves. touch_payload() performs 16 rounds of hash-like arithmetic and is included in both timing for Owner/MutRef, SlotMap/Handle, Arc, and Mutex. That common payload can dilute a primitive overhead regression above 5% into an in-budget workload ratio, so the test does not genuinely prove the declared <=5% slowdown criterion for the raw/unique_ptr/shared_ptr comparisons. I also found that the clean default validation outputs do not contain a bench_primitives executable: release uses tests/*.cpp while tests/perf/*.cpp is only listed under the optional perf lane, so the passed release validation did not execute this gate.


## DEC-0003 -- CPP_MEMSAFE-0920-TEST -- 2026-06-13T14:40:24Z
- artifact: 1st attempt/
- clause: Declared artifact destinations: testing/infra_lanes.json; acceptance: This WP edits only the code-side infra_lanes.json, never tools/.
- status: resolved-by-consensus (accept)
- positions:
    - cowork: approved
    - codex: approved
- decision: accept the candidate's implementation as correct as-is. Rationale: (reviewers converged; see ledger deliberation rows)

