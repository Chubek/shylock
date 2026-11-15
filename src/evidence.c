#include "shylock.h"
#include "mulib-uri.h"
#include "mulib-utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int shy_currency_scale(const char *currency)
{
    static const char *const two[] = { "EUR", "USD", "GBP", "CAD", "AUD", "CHF",
        "SEK", "NOK", "DKK", "NZD", "PLN" };
    for (size_t i = 0; i < sizeof two / sizeof *two; ++i)
        if (!strcmp(currency, two[i])) return 2;
    if (!strcmp(currency, "JPY") || !strcmp(currency, "KRW")) return 0;
    if (!strcmp(currency, "KWD") || !strcmp(currency, "BHD")) return 3;
    return -1;
}

int64_t shy_money_scale(const char *currency) { return shy_currency_scale(currency); }

int64_t shy_date(const char *s)
{
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') return -1;
    for (size_t i = 0; i < 10; ++i)
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return -1;
    int year = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + s[3]-'0';
    int month = (s[5]-'0')*10 + s[6]-'0', day = (s[8]-'0')*10 + s[9]-'0';
    static const int days[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (year < 1 || month < 1 || month > 12) return -1;
    int leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day < 1 || day > days[month-1] + (month == 2 && leap)) return -1;
    return year * 10000 + month * 100 + day;
}

int shy_plugins_init(void)
{
    static int initialized;
    if (!initialized) {
        if (!shy_plugin_register(&shy_dragnet)) return 0;
        initialized = 1;
    }
    return 1;
}

static int identify_candidate(mulib_json_t *row, const char *html, size_t length,
    const char *capture_id, const shy_plugin *plugin)
{
    int64_t offset = shy_integer(row, "byte_offset", SHY_PAGE_LIMIT);
    int64_t span = shy_integer(row, "byte_length", SHY_PAGE_LIMIT);
    if (offset < 0 || span <= 0 || (size_t)offset > length || (size_t)span > length - (size_t)offset)
        return 0;
    char digest[65];
    shy_digest(html + offset, (size_t)span, digest);
    int scale = shy_currency_scale(shy_text(row, "currency"));
    if (!shy_add(row, "capture_id", mulib_json_string(capture_id)) ||
        !shy_add(row, "extractor_version", mulib_json_string(plugin->version)) ||
        !shy_add(row, "span_blake3", mulib_json_string(digest)) ||
        !shy_add(row, "scale", scale < 0 ? mulib_json_new(MULIB_JSON_NULL) : mulib_json_number(scale)) ||
        !shy_add(row, "status", mulib_json_string("needs_review"))) return 0;
    static const char *const unknown[] = { "listing_id", "stay_start", "stay_end", "guests" };
    mulib_json_t *unresolved = mulib_json_new(MULIB_JSON_ARRAY);
    if (!unresolved) return 0;
    for (size_t i = 0; i < sizeof unknown / sizeof *unknown; ++i) {
        if (!shy_add(row, unknown[i], mulib_json_new(MULIB_JSON_NULL)) ||
            !shy_push(unresolved, mulib_json_string(unknown[i]))) goto fail;
    }
    if (!shy_add(row, "taxes", mulib_json_string("unknown")) ||
        !shy_add(row, "fees", mulib_json_string("unknown")) ||
        !shy_push(unresolved, mulib_json_string("taxes")) ||
        !shy_push(unresolved, mulib_json_string("fees"))) goto fail;
    if (!*shy_text(row, "currency") && !shy_push(unresolved, mulib_json_string("currency"))) goto fail;
    if (scale < 0 && !shy_push(unresolved, mulib_json_string("scale"))) goto fail;
    if (shy_integer(row, "minor", INT64_C(1000000000000)) < 0 &&
        !shy_push(unresolved, mulib_json_string("minor"))) goto fail;
    if (!strcmp(shy_text(row, "basis"), "unknown") &&
        !shy_push(unresolved, mulib_json_string("basis"))) goto fail;
    if (!strcmp(shy_text(row, "qualifier"), "review_required") &&
        !shy_push(unresolved, mulib_json_string("qualifier"))) goto fail;
    if (!shy_add(row, "unresolved_fields", unresolved)) return 0;
    return shy_json_digest(row, NULL, digest) && shy_add(row, "evidence_id", mulib_json_string(digest));
fail:
    mulib_json_destroy(unresolved);
    return 0;
}

mulib_json_t *shy_capture_record(const char *html, size_t length, const char *url,
    int64_t timestamp, const char *rules, const char *model, const char *plugin_name)
{
    mulib_uri_t uri;
    if (!html || !length || length > SHY_PAGE_LIMIT || memchr(html, 0, length) ||
        timestamp < 0 || timestamp > INT64_C(253402300799) || strlen(url) > 4096 ||
        (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) ||
        !mulib_uri_parse(url, strlen(url), &uri) || !uri.has_authority ||
        uri.has_userinfo || !uri.host.length || !shy_plugins_init()) return NULL;
    const shy_plugin *plugin = shy_plugin_find(plugin_name);
    if (!plugin) return NULL;
    /* Bad optional model bytes must not make the host's JSON provenance
       allocation fail before deterministic extraction gets a chance to run. */
    size_t model_length = strlen(model);
    if (model_length > 65536 || !mulib_utf8_validate((const unsigned char *)model, model_length))
        model = "invalid model reply encoding or size";
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    mulib_json_t *items = mulib_json_new(MULIB_JSON_ARRAY);
    mulib_json_t *diagnostics = mulib_json_new(MULIB_JSON_ARRAY);
    char digest[65], capture_id[65];
    shy_digest(html, length, digest);
    if (!out || !items || !diagnostics ||
        !shy_add(out, "schema", mulib_json_string("shylock.capture/v2")) ||
        !shy_add(out, "source_url", mulib_json_string(url)) ||
        !shy_add(out, "captured_at", mulib_json_number((double)timestamp)) ||
        !shy_add(out, "blake3", mulib_json_string(digest)) ||
        !shy_json_digest(out, NULL, capture_id) ||
        !shy_add(out, "capture_id", mulib_json_string(capture_id)) ||
        !shy_add(out, "status", mulib_json_string("extracted")) ||
        !shy_add(out, "plugin", mulib_json_string(plugin->name)) ||
        !shy_add(out, "extractor_version", mulib_json_string(plugin->version)) ||
        !shy_add(out, "site_rule_source", mulib_json_string(rules)) ||
        !shy_add(out, "model_reply_source", mulib_json_string(model))) goto fail;
    shy_capture input = { html, length, url, "", "", digest, rules, model, diagnostics };
    if (!plugin->extract(&input, items)) goto fail;
    for (size_t i = 0; i < mulib_json_size(items); ++i)
        if (!identify_candidate(mulib_json_at(items, i), html, length, capture_id, plugin)) goto fail;
    if (!mulib_json_set(out, "candidates", items)) goto fail;
    items = NULL;
    if (!mulib_json_set(out, "diagnostics", diagnostics)) goto fail;
    return out;
fail:
    mulib_json_destroy(items);
    mulib_json_destroy(diagnostics);
    mulib_json_destroy(out);
    return NULL;
}

int shy_verify_capture(const mulib_json_t *record, const char *html, size_t length)
{
    if (strcmp(shy_text(record, "schema"), "shylock.capture/v2")) return 0;
    mulib_json_t *replay = shy_capture_record(html, length, shy_text(record, "source_url"),
        shy_integer(record, "captured_at", INT64_C(253402300799)),
        shy_text(record, "site_rule_source"), shy_text(record, "model_reply_source"),
        shy_text(record, "plugin"));
    int ok = replay && shy_json_equal(record, replay);
    mulib_json_destroy(replay);
    return ok;
}
