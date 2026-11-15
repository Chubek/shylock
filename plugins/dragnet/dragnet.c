#define _POSIX_C_SOURCE 200809L
#include "../../src/shylock.h"
#include "mulib-elvi-scrape.h"
#include "mulib-html-jsonld.h"
#include "mulib-html-tokenizer.h"
#include "mulib-money.h"
#include "mulib-uri.h"
#include "dragnet.h"
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
    const char *container_tag, *container_attribute, *container_value;
    unsigned container_depth, containers_seen;
    char text[4096];
    size_t positions[4096], ends[4096], text_length;
    int mapped;
    char decimal_separator, group_separator;
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
    return shy_currency_scale(currency);
}

static int normalize_amount(const extractor *ctx,const char *source,char out[128])
{
    if (!ctx->decimal_separator || strlen(source)>=128) return 0;
    size_t n=0,group=0; int grouped=0,fraction=0;
    for (const char *s=source;*s;++s) {
        if (digit(*s)) { out[n++]=*s; if (!fraction) ++group; }
        else if (*s==ctx->decimal_separator && !fraction && group && (!grouped || group==3)) { out[n++]='.'; fraction=1; }
        else if (*s==ctx->group_separator && !fraction && group && (grouped ? group==3 : group<=3)) { grouped=1; group=0; }
        else return 0;
    }
    if ((!fraction && grouped && group!=3) || !group || !n || out[n-1]=='.') return 0;
    out[n]=0; return 1;
}
static int candidate(extractor *ctx, const char *method, const char *amount,
                     const char *currency, const char *basis, size_t offset,
                     size_t length, const char *path, const char *qualifier)
{
    if (mulib_json_size(ctx->items) >= 256) return 0;
    mulib_money_t money;
    char normalized[128];
    int localized=strcmp(method,"json-ld") && strcmp(method,"model") && normalize_amount(ctx,amount,normalized);
    int decimals = currency_digits(currency);
    int valid = decimals >= 0 && mulib_money_parse(localized ? normalized : amount, currency, (uint8_t)decimals, &money) &&
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
    if (ok && localized && strcmp(normalized,amount)) ok=shy_add(row,"normalized_amount",mulib_json_string(normalized));
    if (ok && (!strcmp(method, "site-rule") || localized))
        ok = shy_add(row, "rule_blake3", mulib_json_string(ctx->rule_hash));
    if (ok && ctx->model) ok = shy_add(row, "model", mulib_json_string(ctx->model));
    if (ok && mulib_json_append(ctx->items, row)) return 1;
    mulib_json_destroy(row);
    return 0;
}

static int has_type(const mulib_json_t *node,const char *wanted)
{
    const mulib_json_t *type=mulib_json_get(node,"@type");
    if (!strcmp(field(node,"@type"),wanted)) return 1;
    if (mulib_json_typeof(type)==MULIB_JSON_ARRAY)
        for (size_t i=0;i<mulib_json_size(type);++i) {
            const char *s=mulib_json_text(mulib_json_at(type,i),NULL);
            if (s && !strcmp(s,wanted)) return 1;
        }
    return 0;
}
static int offer_walk(extractor *ctx, const mulib_json_t *node, const char *path,
                      size_t offset, size_t length, unsigned depth, int offer,const char *inherited_currency)
{
    if (depth > 24) return 0;
    char next[1024];
    if (mulib_json_typeof(node) == MULIB_JSON_ARRAY) {
        for (size_t i = 0; i < mulib_json_size(node); ++i) {
            int n=snprintf(next, sizeof next, "%s/%zu", path, i);
            if (n<0 || (size_t)n>=sizeof next || !offer_walk(ctx, mulib_json_at(node, i), next, offset, length, depth + 1, offer,inherited_currency))
                return 0;
        }
        return 1;
    }
    const char *currency=field(node,"priceCurrency");
    if (!*currency && offer) currency=inherited_currency;
    int is_offer=offer || has_type(node,"Offer") || has_type(node,"UnitPriceSpecification") || has_type(node,"AggregateOffer");
    const mulib_json_t *price = mulib_json_get(node, "price");
    const char *price_key="price",*qualifier="unspecified";
    if (!price && has_type(node,"AggregateOffer")) { price=mulib_json_get(node,"lowPrice"); price_key="lowPrice"; qualifier="from"; }
    if (price && is_offer) {
        const char *basis = field(node, "unitCode");
        const char *amount = field(node, price_key);
        char numeric[64];
        if (mulib_json_typeof(price) == MULIB_JSON_NUMBER) {
            /* Recover the amount only when it round-trips at the currency scale. */
            double value = mulib_json_numeric(price);
            int digits = currency_digits(currency);
            if (digits >= 0 && value >= 0 && value <= 1e12) {
                snprintf(numeric, sizeof numeric, "%.*f", digits, value);
                if (strtod(numeric, NULL) == value) amount = numeric;
            } else if (digits < 0 && value >= 0 && value <= 1e12) {
                /* Preserve the claim without inventing a currency or scale. */
                snprintf(numeric, sizeof numeric, "%.17g", value);
                amount = numeric;
            }
        }
        int n=snprintf(next, sizeof next, "%s/%s", path,price_key);
        if (n<0 || (size_t)n>=sizeof next) return 0;
        if (*amount && !candidate(ctx, "json-ld", amount, currency,
                                  !strcmp(basis, "DAY") ? "per_day" : "unknown",
                                  offset, length, next, qualifier)) return 0;
    }
    static const char *const keys[] = { "@graph", "mainEntity", "offers", "priceSpecification", "itemListElement", "item", "hasPart" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i) {
        const mulib_json_t *child = mulib_json_get(node, keys[i]);
        if (!child) continue;
        int n=snprintf(next, sizeof next, "%s/%s", path, keys[i]);
        int child_offer=i==2 || i==3;
        if (n<0 || (size_t)n>=sizeof next || !offer_walk(ctx, child, next, offset, length, depth + 1, child_offer,child_offer ? currency : "")) return 0;
    }
    return 1;
}

static int jsonld(void *user, mulib_shylock_span_t span, size_t offset)
{
    mulib_json_t *json = mulib_json_parse(span.data, span.len, NULL);
    if (!json) return 1; /* Other script blocks and HTML may still yield evidence. */
    int ok = offer_walk(user, json, "", offset, span.len, 0, 0, "");
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
    for (size_t i = 0; i + 2 <= n; ++i) {
        char currency[4]=""; size_t begin=i;
        if (i && (unsigned char)text[i-1]>32) continue;
        if (i+4<=n && text[i]>='A' && text[i]<='Z' && text[i+1]>='A' && text[i+1]<='Z' &&
            text[i+2]>='A' && text[i+2]<='Z' && text[i+3]==' ') {
            memcpy(currency,text+i,3); begin=i+4;
        } else if (starts(text+i,n-i,"\xe2\x82\xac")) { strcpy(currency,"EUR"); begin=i+3; }
        else if (text[i]=='$') begin=i+1; /* Currency intentionally unresolved. */
        else continue;
        while (begin<n && text[begin]==' ') ++begin;
        size_t end=begin;
        while (end < n && (digit(text[end]) || text[end] == '.' || text[end]==',' ||
            (ctx->group_separator==' ' && text[end]==' ' && end+1<n && digit(text[end+1])))) ++end;
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
        size_t offset=ctx->mapped ? ctx->positions[i] : token->offset+i;
        size_t length=ctx->mapped ? ctx->ends[stop-1]-offset : stop-i;
        if (!candidate(ctx, "visible-text", amount, currency, basis, offset,
                       length, NULL, qualifier)) return 0;
        i = end - 1;
    }
    return 1;
}

static int flush_text(extractor *ctx)
{
    if (!ctx->text_length) return 1;
    mulib_html_token_t token={0}; token.raw=ctx->text; token.length=ctx->text_length;
    ctx->mapped=1; int ok=visible(ctx,&token); ctx->mapped=0; ctx->text_length=0; return ok;
}
static int inline_tag(const char *name)
{
    static const char *const names[]={"span","b","strong","em","i","small","a","u","s","sup","sub"};
    for (size_t i=0;i<sizeof names/sizeof *names;++i) if (!strcmp(name,names[i])) return 1;
    return 0;
}
static int append_text(extractor *ctx,const mulib_html_token_t *token)
{
    for (size_t i=0;i<token->length;++i) {
        if (ctx->text_length==sizeof ctx->text) return 0; /* Never split a price/label at a limit boundary. */
        size_t size=1; char c=token->raw[i];
        if (starts(token->raw+i,token->length-i,"&nbsp;")) { c=' '; size=6; }
        else if (starts(token->raw+i,token->length-i,"&#160;")) { c=' '; size=6; }
        else if (starts(token->raw+i,token->length-i,"\xc2\xa0")) { c=' '; size=2; }
        ctx->text[ctx->text_length]=c;
        ctx->positions[ctx->text_length]=token->offset+i;
        ctx->ends[ctx->text_length++]=token->offset+i+size;
        i+=size-1;
    }
    return 1;
}
static int html(void *user, const mulib_html_token_t *token)
{
    extractor *ctx = user;
    if (token->kind!=MULIB_HTML_STREAM_TEXT && !inline_tag(token->name) && !flush_text(ctx)) return 0;
    int raw = !strcmp(token->name, "script") || !strcmp(token->name, "style") ||
              !strcmp(token->name, "template");
    if (raw && token->kind == MULIB_HTML_STREAM_END && ctx->raw_depth) --ctx->raw_depth;
    if (raw && token->kind == MULIB_HTML_STREAM_START) { ++ctx->raw_depth; return 1; }
    if (ctx->raw_depth) return 1;
    if (ctx->container_tag && !strcmp(token->name, ctx->container_tag)) {
        if (token->kind == MULIB_HTML_STREAM_END && ctx->container_depth) --ctx->container_depth;
        if (token->kind == MULIB_HTML_STREAM_START) {
            if (ctx->container_depth) ++ctx->container_depth;
            else {
                const char *value; size_t length;
                if (mulib_html_attribute(token->raw, token->length, ctx->container_attribute, &value, &length) &&
                    length == strlen(ctx->container_value) && !memcmp(value, ctx->container_value, length)) {
                    ctx->container_depth = 1;
                    ++ctx->containers_seen;
                }
            }
        }
    }
    if (token->kind == MULIB_HTML_STREAM_TEXT) return append_text(ctx, token);
    if (token->kind==MULIB_HTML_STREAM_START) {
        const char *prop,*amount; size_t prop_length,amount_length;
        if (mulib_html_attribute(token->raw,token->length,"itemprop",&prop,&prop_length) &&
            prop_length==5 && !memcmp(prop,"price",5) &&
            mulib_html_attribute(token->raw,token->length,"content",&amount,&amount_length) && amount_length && amount_length<32) {
            char value[32],currency[4]=""; const char *code; size_t code_length;
            memcpy(value,amount,amount_length); value[amount_length]=0;
            if (mulib_html_attribute(token->raw,token->length,"data-currency",&code,&code_length) && code_length==3) {
                memcpy(currency,code,3); currency[3]=0;
            }
            if (!candidate(ctx,"microdata",value,currency,"unknown",token->offset,token->length,NULL,"unspecified")) return 0;
        }
    }
    if (ctx->attribute && token->kind == MULIB_HTML_STREAM_START &&
        (!ctx->container_tag || ctx->container_depth)) {
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
    if (!*capture->model_source) return 1;
    mulib_json_t *reply = mulib_json_parse(capture->model_source, strlen(capture->model_source), NULL);
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
    if (*capture->rules_source) {
        size_t n = strlen(capture->rules_source);
        const char *source = capture->rules_source;
        if (n > 16384) return 0;
        shy_digest(source, n, digest);
        rule = mulib_json_parse(source, n, NULL);
        mulib_uri_t url;
        const char *host = field(rule, "host");
        ctx.attribute = field(rule, "attribute");
        ctx.currency = field(rule, "currency");
        ctx.basis = field(rule, "basis");
        ctx.rule_hash = digest;
        const char *decimal=field(rule,"decimal_separator"),*group=field(rule,"group_separator");
        if (*decimal || *group) {
            if (strlen(decimal)!=1 || !strchr(".,",*decimal) || strlen(group)>1 ||
                (*group && (!strchr("., ",*group) || *group==*decimal))) { mulib_json_destroy(rule); return 0; }
            ctx.decimal_separator=*decimal; ctx.group_separator=*group;
        }
        int64_t version = mulib_json_get(rule, "version") ? shy_integer(rule, "version", 2) : 1;
        if (version == 2) {
            const mulib_json_t *container = mulib_json_get(rule, "container");
            ctx.container_tag = field(container, "tag");
            ctx.container_attribute = field(container, "attribute");
            ctx.container_value = field(container, "value");
            if ((strcmp(ctx.container_tag, "article") && strcmp(ctx.container_tag, "div") &&
                 strcmp(ctx.container_tag, "section") && strcmp(ctx.container_tag, "li")) ||
                strncmp(ctx.container_attribute, "data-", 5) || strlen(ctx.container_attribute) <= 5 ||
                !*ctx.container_value || strlen(ctx.container_value) > 256) {
                mulib_json_destroy(rule); return 0;
            }
        }
        if (!mulib_uri_parse(capture->url, strlen(capture->url), &url) ||
            version < 1 ||
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
              flush_text(&ctx) && !ctx.container_depth;
    if (ok && ctx.container_tag && !ctx.containers_seen) {
        mulib_json_t *diagnostic = mulib_json_new(MULIB_JSON_OBJECT);
        ok = shy_add(diagnostic, "code", mulib_json_string("site_rule_no_container")) &&
            shy_add(diagnostic, "rule_blake3", mulib_json_string(ctx.rule_hash));
        if (ok) ok = shy_push(capture->diagnostics, diagnostic);
        else mulib_json_destroy(diagnostic);
    }
    /* Model output is transactional and optional. A bad suggestion must never
       discard deterministic evidence or leak partially validated suggestions. */
    if (ok && *capture->model_source) {
        mulib_json_t *suggestions = mulib_json_new(MULIB_JSON_ARRAY);
        ctx.items = suggestions;
        if (!suggestions) ok = 0;
        else if (strlen(capture->model_source) > 65536 || !model_candidates(&ctx, capture)) {
            mulib_json_t *diagnostic = mulib_json_new(MULIB_JSON_OBJECT);
            ok = shy_add(diagnostic, "code", mulib_json_string("model_reply_invalid")) &&
                 shy_add(diagnostic, "message", mulib_json_string("model suggestions rejected; deterministic evidence retained"));
            if (ok) ok = shy_push(capture->diagnostics, diagnostic);
            else mulib_json_destroy(diagnostic);
        } else {
            for (size_t i = 0; ok && i < mulib_json_size(suggestions); ++i)
                ok = mulib_json_size(items) < 256 && shy_push(items, shy_clone(mulib_json_at(suggestions, i)));
        }
        mulib_json_destroy(suggestions);
    }
    mulib_json_destroy(rule);
    return ok;
}

const shy_plugin shy_dragnet = { SHY_EXTRACTOR_ABI, sizeof(shy_plugin), "dragnet", "dragnet/v3", extract };

int64_t shy_capture_page(const char *url)
{
    mulib_shylock_buffer_t body = { 0 };
    char type[128];
    unsigned status;
    int ok = shy_dragnet_get(url, shy_dragnet_https, NULL, &body, &status, type, sizeof type) &&
             status == 200 && shy_dragnet_html_type(type) &&
             body.len && !memchr(body.data, 0, body.len);
    if (ok) ok = fwrite(body.data, 1, body.len, stdout) == body.len && fflush(stdout) == 0;
    mulib_shylock_buffer_free(&body);
    return ok ? 0 : shy_error("expected a successful, bounded HTTPS HTML response");
}
