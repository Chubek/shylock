#define _POSIX_C_SOURCE 200809L
#include "../../src/shylock.h"
#include "mulib-elvi-scrape.h"
#include "mulib-html-jsonld.h"
#include "mulib-html-tokenizer.h"
#include "mulib-money.h"
#include "mulib-uri.h"
#include <strings.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    mulib_json_t *items;
    const char *attribute;
    const char *currency;
    const char *basis;
    const char *rule_hash;
    const char *model;
    unsigned raw_depth;
} extractor;

static int digit(char c) { return c >= '0' && c <= '9'; }

static const char *field(const mulib_json_t *node, const char *key)
{
    size_t n;
    const char *text = mulib_json_text(mulib_json_get(node, key), &n);
    return text && strlen(text) == n ? text : "";
}

static int currency_digits(const char *currency)
{
    static const char *const two[] = { "EUR", "USD", "GBP", "CAD", "AUD", "CHF",
                                       "SEK", "NOK", "DKK", "NZD", "PLN" };
    for (size_t i = 0; i < sizeof two / sizeof *two; ++i)
        if (!strcmp(currency, two[i])) return 2;
    if (!strcmp(currency, "JPY") || !strcmp(currency, "KRW")) return 0;
    if (!strcmp(currency, "KWD") || !strcmp(currency, "BHD")) return 3;
    return -1;
}

static int candidate(extractor *ctx, const char *method, const char *amount,
                     const char *currency, const char *basis, size_t offset,
                     size_t length, const char *path, const char *qualifier)
{
    if (mulib_json_size(ctx->items) >= 256) return 0;
    mulib_money_t money;
    int decimals = currency_digits(currency);
    int valid = decimals >= 0 && mulib_money_parse(amount, currency, (uint8_t)decimals, &money) &&
                money.minor >= 0 && money.minor <= INT64_C(1000000000000);
    mulib_json_t *row = mulib_json_new(MULIB_JSON_OBJECT);
    int ok = shy_add(row, "method", mulib_json_string(method)) &&
             shy_add(row, "amount_text", mulib_json_string(amount)) &&
             shy_add(row, "currency", mulib_json_string(currency)) &&
             shy_add(row, "basis", mulib_json_string(basis)) &&
             shy_add(row, "qualifier", mulib_json_string(qualifier)) &&
             shy_add(row, "byte_offset", mulib_json_number((double)offset)) &&
             shy_add(row, "byte_length", mulib_json_number((double)length)) &&
             shy_add(row, "minor", valid ? mulib_json_number((double)money.minor) :
                                          mulib_json_new(MULIB_JSON_NULL));
    if (ok && path) ok = shy_add(row, "json_pointer", mulib_json_string(path));
    if (ok && !strcmp(method, "site-rule"))
        ok = shy_add(row, "rule_blake3", mulib_json_string(ctx->rule_hash));
    if (ok && ctx->model) ok = shy_add(row, "model", mulib_json_string(ctx->model));
    if (ok && mulib_json_append(ctx->items, row)) return 1;
    mulib_json_destroy(row);
    return 0;
}

static int offer_walk(extractor *ctx, const mulib_json_t *node, const char *path,
                      size_t offset, size_t length, unsigned depth, int offer)
{
    if (depth > 24) return 0;
    char next[1024];
    if (mulib_json_typeof(node) == MULIB_JSON_ARRAY) {
        for (size_t i = 0; i < mulib_json_size(node); ++i) {
            snprintf(next, sizeof next, "%s/%zu", path, i);
            if (!offer_walk(ctx, mulib_json_at(node, i), next, offset, length, depth + 1, offer))
                return 0;
        }
        return 1;
    }
    const char *type = field(node, "@type");
    const mulib_json_t *price = mulib_json_get(node, "price");
    if (price && (offer || !strcmp(type, "Offer") || !strcmp(type, "UnitPriceSpecification"))) {
        const char *currency = field(node, "priceCurrency");
        const char *basis = field(node, "unitCode");
        const char *amount = field(node, "price");
        char numeric[64];
        if (mulib_json_typeof(price) == MULIB_JSON_NUMBER) {
            /* Recover the amount only when it round-trips at the currency scale. */
            double value = mulib_json_numeric(price);
            int digits = currency_digits(currency);
            if (digits >= 0 && value >= 0 && value <= 1e12) {
                snprintf(numeric, sizeof numeric, "%.*f", digits, value);
                if (strtod(numeric, NULL) == value) amount = numeric;
            }
        }
        snprintf(next, sizeof next, "%s/price", path);
        if (*amount && !candidate(ctx, "json-ld", amount, currency,
                                  !strcmp(basis, "DAY") ? "per_day" : "unknown",
                                  offset, length, next, "unspecified")) return 0;
    }
    static const char *const keys[] = { "@graph", "mainEntity", "offers", "priceSpecification" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i) {
        const mulib_json_t *child = mulib_json_get(node, keys[i]);
        if (!child) continue;
        snprintf(next, sizeof next, "%s/%s", path, keys[i]);
        if (!offer_walk(ctx, child, next, offset, length, depth + 1, i >= 2)) return 0;
    }
    return 1;
}

static int jsonld(void *user, mulib_shylock_span_t span, size_t offset)
{
    mulib_json_t *json = mulib_json_parse(span.data, span.len, NULL);
    if (!json) return 1; /* Other script blocks and HTML may still yield evidence. */
    int ok = offer_walk(user, json, "", offset, span.len, 0, 0);
    mulib_json_destroy(json);
    return ok;
}

static int starts(const char *s, size_t n, const char *prefix)
{
    size_t k = strlen(prefix);
    return n >= k && memcmp(s, prefix, k) == 0;
}

static int visible(extractor *ctx, const mulib_html_token_t *token)
{
    const char *text = token->raw;
    size_t n = token->length;
    for (size_t i = 0; i + 5 <= n; ++i) {
        char currency[4] = { text[i], text[i + 1], text[i + 2], 0 };
        if ((i && (unsigned char)text[i - 1] > 32) ||
            currency_digits(currency) < 0 || text[i + 3] != ' ') continue;
        size_t begin = i + 4, end = begin;
        while (end < n && (digit(text[end]) || text[end] == '.')) ++end;
        if (end == begin || end - begin >= 32) continue;
        const char *basis = "unknown";
        size_t suffix = 0;
        if (starts(text + end, n - end, " per night")) { basis = "nightly"; suffix = 10; }
        else if (starts(text + end, n - end, " total")) { basis = "stay_total"; suffix = 6; }
        else if (starts(text + end, n - end, " cleaning fee")) { basis = "cleaning_fee"; suffix = 13; }
        size_t stop = end + suffix;
        if (!suffix || (stop < n && (unsigned char)text[stop] > 32 && !strchr(".,;)]", text[stop])))
            continue;
        char amount[32];
        memcpy(amount, text + begin, end - begin);
        amount[end - begin] = 0;
        const char *qualifier = i >= 5 && !memcmp(text + i - 5, "from ", 5) ? "from" : "unspecified";
        if (!candidate(ctx, "visible-text", amount, currency, basis, token->offset + i,
                       stop - i, NULL, qualifier)) return 0;
        i = end - 1;
    }
    return 1;
}

static int html(void *user, const mulib_html_token_t *token)
{
    extractor *ctx = user;
    int raw = !strcmp(token->name, "script") || !strcmp(token->name, "style") ||
              !strcmp(token->name, "template");
    if (raw && token->kind == MULIB_HTML_STREAM_END && ctx->raw_depth) --ctx->raw_depth;
    if (raw && token->kind == MULIB_HTML_STREAM_START) { ++ctx->raw_depth; return 1; }
    if (ctx->raw_depth) return 1;
    if (token->kind == MULIB_HTML_STREAM_TEXT) return visible(ctx, token);
    if (ctx->attribute && token->kind == MULIB_HTML_STREAM_START) {
        const char *value;
        size_t length;
        if (mulib_html_attribute(token->raw, token->length, ctx->attribute, &value, &length) &&
            length && length < 32) {
            char amount[32];
            memcpy(amount, value, length);
            amount[length] = 0;
            return candidate(ctx, "site-rule", amount, ctx->currency, ctx->basis,
                             token->offset, token->length, NULL, "unspecified");
        }
    }
    return 1;
}

/* Model adapters submit suggestions against the exact snapshot. A matching
   quote proves provenance only, so these rows are explicitly review-only. */
static int model_candidates(extractor *ctx, const shy_capture *capture)
{
    if (!*capture->model_reply_path) return 1;
    size_t length;
    char *source = shy_read(capture->model_reply_path, 65536, &length);
    if (!source) return 0;
    mulib_json_t *reply = mulib_json_parse(source, length, NULL);
    free(source);
    const mulib_json_t *items = mulib_json_get(reply, "suggestions");
    ctx->model = field(reply, "model");
    int ok = *ctx->model && strlen(ctx->model) <= 128 &&
             !strcmp(field(reply, "blake3"), capture->digest) &&
             mulib_json_typeof(items) == MULIB_JSON_ARRAY && mulib_json_size(items) <= 32;
    for (size_t i = 0; ok && i < mulib_json_size(items); ++i) {
        const mulib_json_t *item = mulib_json_at(items, i);
        const mulib_json_t *offset = mulib_json_get(item, "offset");
        double pos = mulib_json_numeric(offset);
        const char *quote = field(item, "quote");
        const char *amount = field(item, "amount");
        const char *currency = field(item, "currency");
        size_t n = strlen(quote);
        ok = mulib_json_typeof(offset) == MULIB_JSON_NUMBER && pos >= 0 &&
             pos <= (double)capture->length && trunc(pos) == pos && n && n <= 512 &&
             n <= capture->length - (size_t)pos &&
             !memcmp(capture->html + (size_t)pos, quote, n) &&
             *amount && strlen(amount) <= 31 && strstr(quote, amount) &&
             currency_digits(currency) >= 0;
        if (ok) ok = candidate(ctx, "model", amount, currency, "unknown",
                               (size_t)pos, n, NULL, "review_required");
    }
    ctx->model = NULL;
    mulib_json_destroy(reply);
    return ok;
}

static int extract(const shy_capture *capture, mulib_json_t *items)
{
    extractor ctx = { .items = items };
    mulib_json_t *rule = NULL;
    char digest[65];
    if (*capture->rules_path) {
        size_t n;
        char *source = shy_read(capture->rules_path, 16384, &n);
        if (!source) return 0;
        shy_digest(source, n, digest);
        rule = mulib_json_parse(source, n, NULL);
        free(source);
        mulib_uri_t url;
        const char *host = field(rule, "host");
        ctx.attribute = field(rule, "attribute");
        ctx.currency = field(rule, "currency");
        ctx.basis = field(rule, "basis");
        ctx.rule_hash = digest;
        if (!mulib_uri_parse(capture->url, strlen(capture->url), &url) ||
            !url.has_authority || url.has_userinfo || strlen(host) != url.host.length ||
            strncasecmp(url.host.data, host, url.host.length) ||
            strncmp(ctx.attribute, "data-", 5) || strlen(ctx.attribute) <= 5 ||
            currency_digits(ctx.currency) < 0 ||
            (strcmp(ctx.basis, "nightly") && strcmp(ctx.basis, "stay_total") &&
             strcmp(ctx.basis, "cleaning_fee"))) {
            mulib_json_destroy(rule);
            return 0;
        }
    }
    int ok = mulib_html_jsonld_scan(capture->html, capture->length, jsonld, &ctx, 64) &&
             mulib_html_tokenizer_scan(capture->html, capture->length, html, &ctx, 100000) &&
             model_candidates(&ctx, capture);
    mulib_json_destroy(rule);
    return ok;
}

const shy_plugin shy_dragnet = { 1, "dragnet/v1", extract };

/* Optional HTTPS transport for the CLI. Curl handles certificate validation.
   ElviScrape still owns query construction, transport policy and SERP parsing. */
static int https(void *unused, const char *url, mulib_shylock_buffer_t *body,
                 unsigned *status, char *type, size_t cap)
{
    (void)unused;
    int fds[2];
    if (pipe(fds)) return 0;
    pid_t pid = fork();
    if (!pid) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) == -1) _exit(126);
        close(fds[1]);
        execlp("curl", "curl", "-q", "--silent", "--show-error", "--globoff",
               "--proto", "=https", "--tlsv1.2", "--max-time", "20",
               "--max-filesize", "2097152", "--write-out", "\n%{http_code}\n%{content_type}\n",
               "--", url, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return 0; }
    size_t limit = SHY_PAGE_LIMIT + 512;
    char *buf = malloc(limit);
    size_t used = 0;
    while (buf && used < limit) {
        ssize_t got = read(fds[0], buf + used, limit - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        used += (size_t)got;
    }
    close(fds[0]);
    int ended;
    pid_t waited;
    do { waited = waitpid(pid, &ended, 0); } while (waited < 0 && errno == EINTR);
    int ok = buf && waited == pid && WIFEXITED(ended) && WEXITSTATUS(ended) == 0 &&
             used >= 6 && used < limit && buf[used-1] == '\n' && !memchr(buf, 0, used);
    if (ok) {
        buf[used-1] = 0;
        char *media = strrchr(buf, '\n');
        ok = media && media >= buf + 4 && media[-4] == '\n' &&
             digit(media[-3]) && digit(media[-2]) && digit(media[-1]) && strlen(media + 1) < cap;
        if (ok) {
            *status = (unsigned)((media[-3] - '0') * 100 + (media[-2] - '0') * 10 + media[-1] - '0');
            strcpy(type, media + 1);
            ok = mulib_shylock_append(body, buf, (size_t)(media - 4 - buf));
        }
    }
    free(buf);
    return ok;
}

int64_t shy_capture_page(const char *url)
{
    mulib_fetch_client_t client = { { 4096, SHY_PAGE_LIMIT, 0, 1, 0 }, https, NULL };
    mulib_shylock_buffer_t body = { 0 };
    char type[128];
    unsigned status;
    int ok = mulib_fetch_get(&client, url, &body, &status, type, sizeof type) &&
             status == 200 && (!strncasecmp(type, "text/html", 9) ||
                               !strncasecmp(type, "application/xhtml+xml", 21)) &&
             body.len && !memchr(body.data, 0, body.len);
    if (ok) ok = fwrite(body.data, 1, body.len, stdout) == body.len && fflush(stdout) == 0;
    mulib_shylock_buffer_free(&body);
    return ok ? 0 : shy_error("expected a successful, bounded HTTPS HTML response");
}

int64_t shy_discover(const char *provider, const char *query, const char *capture, const char *root)
{
    if (!*query || strlen(provider) > 64 || !*provider ||
        strspn(provider, "abcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(provider))
        return shy_error("invalid provider name or query");
    char path[4096], url[4096];
    int n = snprintf(path, sizeof path, "%s/%s.elvs", root, provider);
    mulib_elvi_scrape_program_t program;
    if (n < 0 || (size_t)n >= sizeof path ||
        !mulib_elvi_scrape_load(path, &program, NULL) ||
        !mulib_elvi_scrape_url(&program, query, 10, url, sizeof url))
        return shy_error("cannot load provider or construct query");
    mulib_elvi_scrape_result_t results[64];
    size_t count = 0;
    unsigned status = 0;
    int ok;
    if (*capture) {
        size_t length;
        char *html = shy_read(capture, SHY_PAGE_LIMIT, &length);
        ok = html && mulib_elvi_scrape_parse(&program, html, length, results, 64, &count);
        free(html);
    } else {
        mulib_fetch_client_t client = { { 4096, SHY_PAGE_LIMIT, 0, 1, 0 }, https, NULL };
        ok = mulib_elvi_scrape_search(&program, &client, query, 10, results, 64, &count, &status);
    }
    if (!ok) return shy_error("search failed; check provider response or capture");
    mulib_json_t *out = mulib_json_new(MULIB_JSON_OBJECT);
    mulib_json_t *hits = mulib_json_new(MULIB_JSON_ARRAY);
    ok = hits && shy_add(out, "provider", mulib_json_string(provider)) &&
         shy_add(out, "search_url", mulib_json_string(url)) &&
         shy_add(out, "status", mulib_json_string(count ? "results" : "no_parsed_hits"));
    for (size_t i = 0; ok && i < count; ++i) {
        mulib_json_t *hit = mulib_json_new(MULIB_JSON_OBJECT);
        ok = shy_add(hit, "url", mulib_json_string(results[i].url)) &&
             shy_add(hit, "title", mulib_json_string(results[i].title)) &&
             mulib_json_append(hits, hit);
        if (!ok) mulib_json_destroy(hit);
    }
    if (!ok || !mulib_json_set(out, "hits", hits)) {
        mulib_json_destroy(hits);
        mulib_json_destroy(out);
        return shy_error("could not allocate discovery result");
    }
    return shy_emit(out);
}
