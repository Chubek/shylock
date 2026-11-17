#include "shylock.h"
#include "mulib-uri.h"
#include "mulib-utf8.h"
#include "mulib-money.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t shy_extract_plugin(const char *plugin, const char *path, const char *url, int64_t timestamp, const char *rules,
                    const char *model_reply)
{
    size_t length;
    char *data = shy_read(path, SHY_PAGE_LIMIT, &length);
    if (!data) return shy_error("cannot read bounded HTML capture");
    size_t ignored;
    char *rule = *rules ? shy_read(rules, 16384, &ignored) : NULL;
    char *model = *model_reply ? shy_read(model_reply, 65536, &ignored) : NULL;
    /* A missing optional model file is represented as invalid model input so
       extraction can retain its deterministic results and a diagnostic. */
    mulib_json_t *record = (!*rules || rule) ? shy_capture_record(data, length, url, timestamp,
        rule ? rule : "", model ? model : (*model_reply ? "unreadable model reply" : ""), plugin) : NULL;
    free(data);
    free(rule);
    free(model);
    if (!record) return shy_error("extractor failed; check URL, capture time, site rules and snapshot limits");
    return shy_emit(record);
}

static int identify_candidate(mulib_json_t *row, const char *html, size_t length,
    const char *capture_id, const shy_plugin *plugin)
{
    static const char *const fields[]={"method","amount_text","currency","basis","qualifier"};
    for (size_t i=0;i<sizeof fields/sizeof *fields;++i)
        if (mulib_json_typeof(mulib_json_get(row,fields[i]))!=MULIB_JSON_STRING) return 0;
    if (mulib_json_typeof(row)!=MULIB_JSON_OBJECT || !*shy_text(row,"method") ||
        strlen(shy_text(row,"method"))>64 || !*shy_text(row,"amount_text") || strlen(shy_text(row,"amount_text"))>128 ||
        strlen(shy_text(row,"currency"))>3 || !*shy_text(row,"basis") || strlen(shy_text(row,"basis"))>32 ||
        !*shy_text(row,"qualifier") || strlen(shy_text(row,"qualifier"))>32) return 0;
    int64_t offset = shy_integer(row, "byte_offset", SHY_PAGE_LIMIT);
    int64_t span = shy_integer(row, "byte_length", SHY_PAGE_LIMIT);
    if (offset < 0 || span <= 0 || (size_t)offset > length || (size_t)span > length - (size_t)offset)
        return 0;
    char digest[65];
    shy_digest(html + offset, (size_t)span, digest);
    int scale = shy_currency_scale(shy_text(row, "currency"));
    const mulib_json_t *minor=mulib_json_get(row,"minor");
    if (!minor) return 0;
    if (mulib_json_typeof(minor)!=MULIB_JSON_NULL) {
        const char *amount=shy_text(row,"normalized_amount");
        if (!*amount) amount=shy_text(row,"amount_text");
        mulib_money_t money;
        if (scale<0 || shy_integer(row,"minor",INT64_C(1000000000000))<0 ||
            !mulib_money_parse(amount,shy_text(row,"currency"),(uint8_t)scale,&money) ||
            money.minor!=shy_integer(row,"minor",INT64_C(1000000000000))) return 0;
    }
    if (!shy_add(row, "capture_id", mulib_json_string(capture_id)) ||
        !shy_add(row, "extractor_version", mulib_json_string(plugin->version)) ||
        !shy_add(row, "span_blake3", mulib_json_string(digest)) ||
        !shy_add(row, "scale", scale < 0 ? mulib_json_new(MULIB_JSON_NULL) : mulib_json_number(scale)) ||
         !shy_add(row, "status", mulib_json_string("needs_review"))) return 0;
    const char *configuration=shy_plugin_configuration(plugin);
    if (configuration) {
        shy_digest(configuration,strlen(configuration),digest);
        if (!shy_add(row,"plugin_configuration_blake3",mulib_json_string(digest))) return 0;
    }
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
    const char *configuration=shy_plugin_configuration(plugin);
    if (configuration && !shy_add(out,"plugin_configuration",mulib_json_string(configuration))) goto fail;
    if (!shy_plugin_extract(plugin, &input, items) || mulib_json_size(items)>256 || mulib_json_size(diagnostics)>64) goto fail;
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
