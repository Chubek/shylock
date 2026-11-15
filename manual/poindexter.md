# Poindexter review and cohort workflow

Poindexter adds a persistent local ledger backed by MuLIB KVDB. On1x owns
candidate admission and CLI workflow. The C bridge manages borrowed JSON
values and database handles. Writers use an advisory exclusive lock alongside
the database and commit atomic KVDB snapshots.

Run from the Shylock root after placing MuLIB revision
151528b50b316aeea804ce2aebf66ef90b9e7100 at third_party/mulib.

```sh
./build.sh -j4
make test
./shylock extract tests/fixtures/rental.html https://example.org/cottage 1000 > capture.json
./shylock review prices.db capture.json 1 cottages-eur alice
./shylock ledger prices.db cottages-eur
./shylock series prices.db cottages-eur EUR 1000 60 2 > series.json
```

Candidate indexes start at zero. Review explicitly accepts a nightly candidate
whose amount and currency are present and whose qualifier is unspecified.
The reviewer is responsible for checking stay, guest, tax and fee
comparability and assigning a consistent cohort name. Captures are trusted
local extraction records. Import does not re-fetch or re-hash the original
HTML. Keep those HTML files for provenance audits.

The last example requires a second observation in the next 60-second bucket.
Series export rounds each bucket's arithmetic mean to integer minor units.
Every requested bucket must contain observations. Exit 4 means a missing
bucket. Exit 3 means an incompatible currency/basis or conflicting review.
Exit 2 means invalid input, I/O failure, or uncertain commit durability.
Retries with identical capture hash, candidate index and review data succeed
without duplicating a row. A changed review at the same key returns a conflict.
Store separate captures for changing offers. Cross-capture entity matching
and correlated-source deduplication remain future work.

Use the exported array with estimate or recommend. Both commands retain their
existing interfaces. Ledger outputs the reviewed rows as JSON for downstream
table tools. The graphical and terminal spreadsheet interfaces remain open.

Formula states expose deterministic math functions through Mune. The math
random generators are removed, and file, process, module and debug functions
remain unavailable to formulas. Formula authors can use formulas/rounded.lua.
Effectful Lua extension hosting requires a separate lifecycle implementation.

Run tests/poindexter.py through make test. It verifies review retries,
conflicts, rejected quote bases, currency mismatches, missing buckets, and a
complete reviewed-observation to recommendation flow.
