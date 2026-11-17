"""Offline DOM rules, On1x reconciliation, provenance and verified review."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RULE = ROOT / "plugins/dragnet/rules/rental.json"
PROFILE = ROOT / "plugins/dragnet/openai-profile.json"


def run(*args, code=0, extra=None):
    env = dict(os.environ)
    for name in ("OPENAI_API_KEY", "OPENAI_BASE_URL", "SHY_NATIVE_PLUGINS", "SHY_EXTENSIONS"):
        env.pop(name, None)
    if extra:
        env.update(extra)
    result = subprocess.run([str(ROOT / "shylock"), *map(str, args)], cwd=ROOT,
                            env=env, text=True, capture_output=True, timeout=40)
    assert result.returncode == code, (args, result.returncode, result.stdout[:1000], result.stderr)
    return json.loads(result.stdout) if result.stdout else None


with tempfile.TemporaryDirectory(prefix="shylock-naive-", dir=os.environ.get("TMPDIR")) as directory:
    folder = Path(directory)

    def save(name, value):
        path = folder / name
        path.write_text(json.dumps(value, ensure_ascii=False))
        return path

    snapshot_sequence = 0

    def snapshot(html, name=None, url="https://example.org/cottages"):
        global snapshot_sequence
        snapshot_sequence += 1
        name = name or f"page-{snapshot_sequence}"
        path = folder / (name + ".html")
        path.write_text(html)
        result = run("snapshot", url, 1760000000, path)
        return save(name + ".json", result)

    def rules(**changes):
        value = json.loads(RULE.read_text())
        value.update(changes)
        return save("rule.json", value)

    def field(selector=".price", kind="auto", attribute="", **changes):
        result = dict(selector=selector, attribute=attribute, kind=kind, regex="", priority=80,
                      currency_selector="", currency_attribute="")
        result.update(changes)
        return result

    def card(text, title="Lake Cottage"):
        return f'<body><div class="listing-card"><h2 class="listing-title">{title}</h2>{text}</div></body>'

    source = snapshot((ROOT / "tests/fixtures/naive-rentals.html").read_text(), "rentals")
    found = run("find-rule", source, RULE, "rental_price")
    artifact = save("find.json", found)
    assert found == run("find-rule", source, RULE, "rental_price")
    assert found == run("find-rule-replay", artifact)
    assert found["schema"] == "shylock.naive-find/v1" and found["mode"] == "deterministic"
    rows = found["candidates"]
    lake = [row for row in rows if row["listing_id"] == "Lake & Cottage"]
    hill = [row for row in rows if row["listing_id"] == "Hill Cottage"]
    assert len(lake) == 5 and len(hill) == 3, [(r["listing_id"], r["method"], r["amount_text"]) for r in rows]
    assert {(r["basis"], r["minor"]) for r in lake} == {
        ("nightly", 18000), ("cleaning_fee", 7500), ("refundable_deposit", 20000), ("stay_total", 54000)}
    assert any(r["qualifier"] == "from" and r["minor"] == 12000 for r in hill)
    assert all(r["minor"] not in (100, 200, 999900) for r in rows)
    assert all(r["status"] == "needs_review" and r["independent_corroboration"] is False for r in rows)
    assert any(g["same_page_representations"] == 2 and g["independent_sources"] == 1 for g in found["groups"])
    split = lake[0]
    assert split["source_span_kind"] == "covering-text-fragments"
    raw = found["snapshot"]["html"].encode()
    assert b"<b>180</b>.00" in raw[split["byte_offset"]:split["byte_offset"] + split["byte_length"]]
    assert all(len(r["source_span_blake3"]) == 64 and len(r["evidence_id"]) == 64 for r in rows)
    assert run("flatten", source)["snapshot"] == found["snapshot"]
    assert run("extract-snapshot", source) == found["legacy_extraction"]
    assert run("find", source, "prices")["extraction"] == found["legacy_extraction"]

    # Same-basis contradictions retain both alternatives, regardless of score.
    conflict_source = snapshot(card('<p class="price">EUR 180.00 per night</p>'
                                    '<script type="application/ld+json">'
                                    '{"@type":"Offer","price":"190.00","priceCurrency":"EUR","unitText":"per night"}'
                                    '</script>'), "conflict")
    conflict = run("find-rule", conflict_source, RULE, "rental_price")
    assert len(conflict["candidates"]) == 2 and all(r["conflict"] for r in conflict["candidates"])
    assert {r["minor"] for r in conflict["candidates"]} == {18000, 19000}
    assert all(g["reconciliation"] == "material_conflict_requires_review" for g in conflict["groups"])
    global_structured = snapshot('<body><script type="application/ld+json">'
                                 '{"@type":"Offer","price":"999.00","priceCurrency":"EUR"}</script>'
                                 + card('<p class="price">EUR 180.00 per night</p>').replace('<body>', '').replace('</body>', '')
                                 + '</body>')
    unscoped = run("find-rule", global_structured, RULE, "rental_price")
    assert [r["minor"] for r in unscoped["candidates"]] == [18000]
    assert any(r["minor"] == 99900 for r in unscoped["legacy_extraction"]["candidates"])
    micro_source = snapshot(card('<meta itemprop="price" content="180.00" data-currency="EUR">'
                                 '<meta hidden itemprop="price" content="1.00" data-currency="EUR">'))
    micro = run("find-rule", micro_source, RULE, "rental_price")
    assert [(r["method"], r["minor"]) for r in micro["candidates"]] == [("existing-microdata", 18000)]

    examples = [
        ("EUR 180.00 per night", 18000, "EUR", "nightly", "unspecified"),
        ("180.00 EUR per week", 18000, "EUR", "weekly", "unspecified"),
        ("€180 per month", 18000, "EUR", "monthly", "unspecified"),
        ("180 € per day", 18000, "EUR", "per_day", "unspecified"),
        ("$200 per night", None, "", "nightly", "unspecified"),
        ("USD 200 per night", 20000, "USD", "nightly", "unspecified"),
        ("From €120 per night", 12000, "EUR", "nightly", "from"),
        ("€540 total for three nights", 54000, "EUR", "stay_total", "unspecified"),
        ("€75 cleaning fee", 7500, "EUR", "cleaning_fee", "unspecified"),
        ("EUR 50 service fee", 5000, "EUR", "service_fee", "unspecified"),
        ("EUR 12 tax", 1200, "EUR", "tax", "unspecified"),
        ("EUR 200 refundable deposit", 20000, "EUR", "refundable_deposit", "unspecified"),
        ("Discounted €100 per night", 10000, "EUR", "nightly", "discounted"),
        ("Original €150 per night", 15000, "EUR", "nightly", "original"),
        ("€90", 9000, "EUR", "unknown", "unknown"),
        ("180,50 € per night", None, "EUR", "nightly", "unspecified"),
        ("1.250,00 EUR per night", None, "EUR", "nightly", "unspecified"),
        ("EUR 1,200 per night", None, "EUR", "nightly", "unspecified"),
    ]
    auto_rule = rules(fields=[field()], include_existing=False)
    for text, minor, currency, basis, qualifier in examples:
        page = snapshot(card(f'<p class="price">{text}</p>'))
        result = run("find-rule", page, auto_rule, "rental_price")
        assert len(result["candidates"]) == 1, (text, result)
        candidate = result["candidates"][0]
        assert (candidate["minor"], candidate["currency"], candidate["basis"], candidate["qualifier"]) == (
            minor, currency, basis, qualifier), (text, candidate)
    localized = snapshot(card('<p class="price">1.250,00 EUR per night</p>'))
    locale_rule = rules(fields=[field()], decimal_separator=",", group_separator=".", include_existing=False)
    assert run("find-rule", localized, locale_rule, "rental_price")["candidates"][0]["minor"] == 125000
    malformed_group = snapshot(card('<p class="price">12.50,00 EUR per night</p>'))
    assert run("find-rule", malformed_group, locale_rule, "rental_price")["candidates"][0]["minor"] is None

    # Attribute amounts and metadata currencies are scoped independently per card.
    attr_source = snapshot('<body>' + ''.join(
        f'<div class="listing-card"><h2 class="listing-title">{title}</h2>'
        f'<meta class="currency" content="{currency}"><span class="price" data-price="180.00"></span></div>'
        for title, currency in (("A", "EUR"), ("B", "USD"))) + '</body>')
    attr_rule = rules(fields=[field(kind="nightly_rate", attribute="data-price",
                                   currency_selector=".currency", currency_attribute="content")], include_existing=False)
    attr_rows = run("find-rule", attr_source, attr_rule, "rental_price")["candidates"]
    assert [(r["listing_id"], r["currency"], r["minor"]) for r in attr_rows] == [("A", "EUR", 18000), ("B", "USD", 18000)]
    assert all(r["source_span_kind"] == "exact" for r in attr_rows)
    metadata_conflict = snapshot(card('<meta class="currency" content="EUR"><meta class="currency" content="USD">'
                                      '<span class="price" data-price="180.00"></span>'))
    ambiguous_metadata = run("find-rule", metadata_conflict, attr_rule, "rental_price")["candidates"][0]
    assert ambiguous_metadata["minor"] is None and ambiguous_metadata["currency"] == ""
    assert ambiguous_metadata["diagnostic"] == "invalid_scoped_currency"
    trusted = rules(host="example.org", currency="USD", currency_source="trusted_site_rule", fields=[field()], include_existing=False)
    ambiguous = snapshot(card('<p class="price">$200 per night</p>'))
    assert run("find-rule", ambiguous, trusted, "rental_price")["candidates"][0]["minor"] == 20000
    mismatched = snapshot(card('<p class="price">€200 per night</p>'))
    assert run("find-rule", mismatched, trusted, "rental_price")["candidates"][0]["diagnostic"] == "currency_conflict"
    cottage = snapshot('<body><div class="listing-card" data-listing-id="cottage-1">'
                       '<span class="price" data-price="1.250,00"></span></div></body>')
    assert run("find-rule", cottage, ROOT / "plugins/dragnet/rules/examples/cottage.json", "rental_price")["candidates"][0]["minor"] == 125000
    generic_source = snapshot('<body><h1>House</h1><p>EUR <b>180</b>.00 per night</p></body>')
    assert run("find-rule", generic_source, ROOT / "plugins/dragnet/rules/generic.json", "prices")["candidates"][0]["minor"] == 18000
    separated = snapshot(card('<span class="price">&#x20ac;<b>180</b><i>.50</i></span><span class="basis">per night</span>'))
    context_rule = rules(fields=[field(context_selector=".basis")], include_existing=False)
    contextual = run("find-rule", separated, context_rule, "rental_price")["candidates"][0]
    assert contextual["minor"] == 18050 and contextual["basis"] == "nightly"
    assert contextual["classification_context"] == "per night" and contextual["context_node_offset"] >= 0
    assert contextual["source_span_kind"] == "covering-text-fragments"
    entity = snapshot(card('<p class="price">&euro;180&#46;50&#160;per night</p>'))
    assert run("find-rule", entity, rules(fields=[field()], include_existing=False), "rental_price")["candidates"][0]["minor"] == 18050
    spaced = snapshot(card('<p class="price">EUR 1\u202f250,00 per night</p>'))
    assert run("find-rule", spaced, rules(fields=[field()], decimal_separator=",", group_separator=" ", include_existing=False), "rental_price")["candidates"][0]["minor"] == 125000
    assert run("find-rule", spaced, rules(fields=[field()], include_existing=False), "rental_price")["candidates"][0]["minor"] is None
    double_encoded = snapshot(card('<p class="price">&amp;#8364;180.00</p>'))
    assert run("find-rule", double_encoded, rules(fields=[field()], include_existing=False), "rental_price")["candidates"] == []

    # Malformed HTML recovery stays bounded; unsupported selectors and schemas fail closed.
    malformed = snapshot('<body><div class="listing-card"><h2 class="listing-title">A</h2><p class="price">€180 per night')
    assert run("find-rule", malformed, RULE, "rental_price")["candidates"][0]["minor"] == 18000
    base = json.loads(RULE.read_text())
    for key, value in (("schema", "unknown"), ("host", "other.example"), ("target", "other"),
                       ("container", ".card .price"), ("container", ".price:hover"), ("maximum_candidates", 0), ("maximum_candidates", 257),
                       ("maximum_containers", 129), ("fallback", "execute"), ("network", True),
                       ("currency", "USD"), ("group_separator", ".")):
        bad = copy.deepcopy(base); bad[key] = value
        run("find-rule", source, save("bad-rule.json", bad), "rental_price", code=2)
    for pattern in ("(", "a*", "[0-9]{999999}", "x" * 257):
        run("find-rule", source, rules(fields=[field(regex=pattern)]), "rental_price", code=2)
    invalid_file = folder / "invalid-rule.json"; invalid_file.write_text('{"schema":')
    run("find-rule", source, invalid_file, "rental_price", code=2)
    duplicate = folder / "duplicate-rule.json"
    for spelling in ('"host"', '"ho\\u0073t"'):
        duplicate.write_text(RULE.read_text().replace('"host": "*",', f'"host": "other.example", {spelling}: "*",'))
        run("find-rule", source, duplicate, "rental_price", code=2)
    run("find-rule", source, rules(maximum_candidates=1), "rental_price", code=2)
    run("find-rule", source, rules(maximum_containers=1), "rental_price", code=2)
    nested = snapshot(card('<div class="listing-card"><h2 class="listing-title">B</h2><p class="price">EUR 180</p></div>'))
    run("find-rule", nested, RULE, "rental_price", code=2)
    too_deep = snapshot('<body>' + '<div>' * 100 + 'EUR 180 per night' + '</div>' * 100 + '</body>')
    run("find-rule", too_deep, RULE, "rental_price", code=2)
    too_many_nodes = snapshot('<body>' + '<span>x</span>' * 4200 + '</body>')
    run("find-rule", too_many_nodes, RULE, "rental_price", code=2)
    huge_token = snapshot(card('<p class="price">EUR ' + '1' * 129 + ' per night</p>'))
    run("find-rule", huge_token, rules(fields=[field()], include_existing=False), "rental_price", code=2)
    partial = snapshot(card('<p class="price">EUR 180.50 per night</p>'))
    run("find-rule", partial, rules(fields=[field(regex="EUR [0-9]+")], include_existing=False), "rental_price", code=2)
    partial_space = snapshot(card('<p class="price">EUR 1 250 per night</p>'))
    run("find-rule", partial_space, rules(fields=[field(regex="EUR [0-9]+")], include_existing=False), "rental_price", code=2)
    work = snapshot(card('<p class="price">' + 'x' * 7500 + '</p>'))
    # Each no-match scan consumes real remaining-text × pattern work. Repeated
    # valid strategies exceed the shared budget, rather than failing tokenization.
    run("find-rule", work, rules(fields=[field(regex="z" * 250)] * 10, include_existing=False), "rental_price", code=2)
    legacy_limit = snapshot(card('<p class="price">EUR 180.00 per night</p><p>' + 'x' * 5000 + '</p>'))
    preserved = run("find-rule", legacy_limit, RULE, "rental_price", code=2)
    assert preserved["legacy_extraction"] is None and preserved["legacy_status"] == "legacy_extraction_unavailable"
    assert preserved["candidates"][0]["minor"] == 18000
    assert run("find-rule-replay", save("partial-find.json", preserved), code=2) == preserved

    for mutate in (lambda d: d["candidates"][0].update(minor=1),
                   lambda d: d["snapshot"].update(html=d["snapshot"]["html"] + " "),
                   lambda d: d.update(rule_source=d["rule_source"] + " "),
                   lambda d: d["groups"][0].update(independent_sources=2),
                   lambda d: d["candidates"].reverse()):
        tampered = copy.deepcopy(found); mutate(tampered)
        bad_path = save("tampered.json", tampered)
        run("find-rule-replay", bad_path, code=2)
        run("review", folder / "bad.db", bad_path, 0, "market", "reviewer", code=2)
    bad_snapshot = json.loads(source.read_text()); bad_snapshot["html"] += " "
    run("find-rule", save("bad-snapshot.json", bad_snapshot), RULE, "rental_price", code=2)

    # Review reruns On1x and uses embedded immutable bytes. Acceptance still needs
    # complete, comparable stay context; no private "verified=true" input exists.
    context = save("context.json", dict(stay_start="2026-12-01", stay_end="2026-12-04", guests=2,
                                        taxes="included", fees="included"))
    db = folder / "ledger.db"
    run("review", db, artifact, 0, "market", "reviewer", code=2)
    run("review", db, artifact, 0, "market", "reviewer", "", context)
    run("review", db, artifact, 0, "market", "reviewer", "", context)  # idempotent
    ledger = run("ledger", db, "market")
    assert len(ledger) == 1 and ledger[0]["provenance"] == "verified-replay"
    assert ledger[0]["observation_id"] == rows[0]["evidence_id"]
    assert run("replay", db, ledger[0]["extraction_id"]) == found
    second_snapshot = save("second-snapshot.json", run("snapshot", found["source_url"], 1760000001, folder / "rentals.html"))
    second_find = save("second-find.json", run("find-rule", second_snapshot, RULE, "rental_price"))
    run("review", db, second_find, 0, "market", "reviewer", "", context)
    cohort = run("cohort", db, "market", "EUR", 1760000000, 1, 2)
    assert cohort["values"] == [18000, 18000]
    decision = run("recommend", save("cohort.json", cohort), 1, ROOT / "formulas/weekend.lua", 1, 1000, 10000, 30000, 100)
    assert decision["adjusted_minor"] == 19800 and decision["cohort_lineage"] == cohort
    bad_context = save("wrong-context.json", dict(json.loads(context.read_text()), listing_id="Other house"))
    run("review", db, artifact, 0, "wrong", "reviewer", "", bad_context, code=2)
    conflict_path = save("conflict-find.json", conflict)
    run("review", db, conflict_path, 0, "conflict", "reviewer", "", context, code=2)
    resolution = save("resolution.json", dict(json.loads(context.read_text()), conflict_resolution="DOM quote selected after source inspection"))
    run("review", db, conflict_path, 0, "conflict", "reviewer", "", resolution)
    reviewed_conflict = run("ledger", db, "conflict")[0]
    assert reviewed_conflict["reason"].startswith("DOM quote selected")
    run("revise", db, reviewed_conflict["observation_id"], reviewed_conflict["review_id"], "rejected", "reviewer", "pending further inspection")
    rejected_conflict = run("ledger", db, "conflict")[0]
    run("revise", db, rejected_conflict["observation_id"], rejected_conflict["review_id"], "accepted", "reviewer", "cannot silently resolve alternatives", code=2)

    # Explicit model modes cannot turn empty results or conflicts into consent.
    run("find-rule", source, RULE, "rental_price", "hybrid", code=2)
    run("find-rule", source, RULE, "rental_price", "model", PROFILE, "--LIVE", code=2)
    deterministic = run("find-rule", source, RULE, "rental_price", extra={"OPENAI_API_KEY": "not-used"})
    assert deterministic == found
    hybrid_rule = rules(fields=[field()], fallback="model_review", include_existing=False)
    hybrid = run("find-rule", ambiguous, hybrid_rule, "rental_price", "hybrid", PROFILE, "--live", code=2)
    assert hybrid["status"] == "deterministic_fallback"
    assert hybrid["deterministic"]["candidates"][0]["currency"] == ""
    assert hybrid["model_result"]["model_audit"]["outcome"] == "credentials_unavailable"
    skipped = run("find-rule", source, RULE, "rental_price", "hybrid", PROFILE, "--live")
    assert skipped["status"] == "model_not_needed" and skipped["deterministic"] == found
    model = run("find-rule", source, RULE, "rental_price", "model", PROFILE, "--live", code=2)
    assert model["status"] == "deterministic_fallback" and model["deterministic"] == found

print("NaiveFinder DOM rules, classification, reconciliation, limits, replay, review and explicit fallback tests passed.")
