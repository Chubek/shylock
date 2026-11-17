# Extending Dragnet / NaiveFinder

Read the repository's `../../AGENTS.md`, `manual/dragnet.md`, `manual/intelligence.md` and
`manual/dragnet-naive.md` before changing contracts. Work against the actual
`third_party/mulib` checkout; currently verified revision:
`151528b50b316aeea804ce2aebf66ef90b9e7100`. Do not change the submodule for an adapter.

## Ownership of decisions

- Keep rule admission/defaults in `src/dragnet_rules.on1x`, DOM strategy/listing
  association and English classification in `src/dragnet_strategy.on1x`, currency/
  amount/conflict/consent policy in `src/dragnet_policy.on1x`, inspection scores in
  `src/dragnet_rank.on1x` and evidence assembly in `src/dragnet_evidence.on1x`.
- `src/dragnet_naive.on1x` owns command, replay, review and fallback orchestration.
  Do not move these workflows into C or introduce a thin-wrapper architecture.
  Keep substantive new extraction code approximately 40% C11 / 60% On1x; report
  the measurement scope, excluding tests, generated code, docs and dependencies.
- C is for MuLIB calls, checked money, regexes, Unicode/entity/source mapping,
  structural facts, generation handles and lifetimes. Declare scalar ABI functions
  in `naive.h` and `src/dragnet_native.on1x`. Never expose a borrowed tree pointer.
- Use upstream standard library and kernels first. Include declarations only in
  adapters; add kernels to Makefile's normal **and allocation-test** helper calls.
  Preserve its single implementation translation unit and GNU Make build.

## Safe changes

1. Add an offline HTML fixture demonstrating the actual markup. Do not add generic
   unscoped proximity associations. Currency/labels/identities must belong to the
   selected container. Global structured offers without a reliable association
   stay separate in the legacy artifact.
2. For a new selector feature, validate its entire grammar in On1x and compose
   bounded native facts. MuLIB DOM query is simple-selector-only; never pass a
   compound selector hoping it will work partially. Reject unknown schema keys.
3. For a new locale or token form, separate recognition from interpretation. Test
   missing currency, ambiguous grouping, complete-token boundaries and overflow.
   Never infer USD from `$`, nightly from `DAY`, or independent corroboration from
   two representations on one page.
4. Keep normalized offsets distinct from original byte spans. Map every emitted
   byte or report a covering range; synthetic block separators cannot ground a
   monetary expression. Preserve entity single-decoding and raw snapshots.
5. New diagnostics/scores are review aids, not verification. Retain conflicting
   alternatives. Version changed semantics and require full recomputation on replay.
   Review and stored replay must re-enter On1x, not trust submitted hashes/booleans.
6. Native allocation/query/regex failures must be sticky through publication.
   Close all partial trees, scratch, selector records and regexes. Add allocation
   injection coverage to `tests/document_alloc.c` and stale-handle tests to
   `tests/naive.c`. Keep session strings copied before scratch invalidation.
7. Keep model modes explicit. Rules/pages cannot authorize calls or override TLS
   or endpoint policy. Preserve deterministic artifacts separately on all model
   outcomes. No network or paid calls belong in tests.

## Verification

Run `./build.sh -j4` and `TMPDIR=/tmp/opencode make -j4 test`. The suite includes
`tests/naive.py`, `tests/naive.c` and `tests/naive_policy.on1x`. Run strict native
diagnostics (`cc -std=c11 -Wall -Wextra -Wpedantic -Werror -Ithird_party/mulib/include
-fsyntax-only plugins/dragnet/naive*.c`) and a clean alternate BUILD directory.
Use Clang ASan/UBSan in another BUILD with `PPX_TIMEOUT_MS=60000`; do not modify or
discard the user's checkout to make a clean build. Report any live checks separately.

The local fields PPX relies on the pinned compiler's mandatory input/output schema
validation in `on1x/src/ppx.cpp` instead of repeating two slow full-tree schema walks.
Keep its protocol check, hygienic fresh names, and `tests/metaprogramming.py` coverage.
Revisit this assumption when upgrading the compiler; do not bypass host validation.
