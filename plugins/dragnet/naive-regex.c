#include "naive.h"
#include "naive-rules.h"
#include <string.h>

int64_t shy_naive_regex_valid(const char *pattern)
{
    size_t n=strlen(pattern);
    if (!n || n>NF_PATTERN || strchr(pattern,'{')) return 0;
    mulib_regex_error_t error;
    mulib_regex_t *regex=mulib_regex_compile(pattern,n,&error);
    int ok=regex && mulib_regex_full_match(regex,"",0)==0;
    mulib_regex_destroy(regex); return ok;
}
static int numeric(unsigned char c) { return (c>='0' && c<='9') || c=='.' || c==','; }
int nf_scan(const char *pattern)
{
    size_t n=strlen(pattern),length=shy_nf.text.len;
    /* Without counted repetition, emitted NFA size is linear in pattern bytes.
       This conservative product also bounds failed searches/enumeration. */
    if (!n || n>NF_PATTERN || strchr(pattern,'{')) return nf_fail();
    mulib_regex_error_t error;
    mulib_regex_t *regex=mulib_regex_compile(pattern,n,&error);
    if (!regex) return nf_fail();
    size_t start=0; int ok=1;
    while (start<=length) {
        size_t cost=(length-start+1)*(n+1);
        if (cost>NF_REGEX_WORK-shy_nf.work) { ok=0; break; }
        shy_nf.work+=cost;
        mulib_regex_match_t m;
        int found=mulib_regex_search(regex,shy_nf.text.data ? shy_nf.text.data : "",length,start,&m);
        if (found<0) { ok=0; break; }
        if (!found) break;
        if (!m.length || m.length>256 || shy_nf.hits_count==NF_HITS) { ok=0; break; }
        size_t end_at=m.offset+m.length,tail=end_at;
        while (tail<length && (shy_nf.text.data[tail]==' ' || shy_nf.text.data[tail]=='\t')) ++tail;
        if (numeric((unsigned char)shy_nf.text.data[end_at-1]) && tail>end_at && tail<length &&
            shy_nf.text.data[tail]>='0' && shy_nf.text.data[tail]<='9') { ok=0; break; }
        if ((m.offset && (numeric((unsigned char)shy_nf.text.data[m.offset-1]) || shy_nf.text.data[m.offset-1]=='-' || shy_nf.text.data[m.offset-1]=='+')) ||
            (m.offset+m.length<length && (numeric((unsigned char)shy_nf.text.data[m.offset+m.length]) ||
                shy_nf.text.data[m.offset+m.length]=='e' || shy_nf.text.data[m.offset+m.length]=='E'))) { ok=0; break; }
        nf_hit hit={0}; hit.start=m.offset; hit.length=m.length;
        if (!nf_token(shy_nf.text.data+m.offset,m.length,&hit)) { ok=0; break; }
        hit.offset=shy_nf.starts[m.offset]; size_t end=shy_nf.ends[m.offset+m.length-1];
        hit.exact=1;
        for (size_t i=m.offset;i<m.offset+m.length;++i) {
            if (shy_nf.starts[i]==shy_nf.ends[i]) { ok=0; break; }
            if (!shy_nf.exact[i] || (i>m.offset && shy_nf.starts[i]!=shy_nf.ends[i-1])) hit.exact=0;
        }
        if (!ok) break;
        if (end<hit.offset) { ok=0; break; }
        hit.span=end-hit.offset;
        shy_nf.hits[shy_nf.hits_count++]=hit;
        start=m.offset+m.length;
    }
    mulib_regex_destroy(regex); return ok || nf_fail();
}
const char *shy_naive_hit_text(int64_t index,const char *key)
{
    if (index<0 || (size_t)index>=shy_nf.hits_count) { nf_fail(); return ""; }
    nf_hit *h=&shy_nf.hits[index];
    if (!strcmp(key,"amount")) return h->amount;
    if (!strcmp(key,"currency")) return h->currency;
    if (!strcmp(key,"symbol")) return h->symbol;
    if (!strcmp(key,"span_kind")) return h->exact ? "exact" : "covering-text-fragments";
    return "";
}
int64_t shy_naive_hit_number(int64_t index,const char *key)
{
    if (index<0 || (size_t)index>=shy_nf.hits_count) { nf_fail(); return -1; }
    nf_hit *h=&shy_nf.hits[index];
    if (!strcmp(key,"offset")) return (int64_t)h->offset;
    if (!strcmp(key,"span")) return (int64_t)h->span;
    if (!strcmp(key,"start")) return (int64_t)h->start;
    return (int64_t)h->length;
}
