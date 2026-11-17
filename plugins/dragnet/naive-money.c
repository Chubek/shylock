#include "naive.h"
#include "mulib-money.h"
#include <string.h>

int nf_token(const char *text,size_t length,nf_hit *hit)
{
    size_t begin=0;
    while (begin<length && !(text[begin]>='0' && text[begin]<='9')) ++begin;
    size_t end=begin;
    while (end<length) {
        if ((text[end]>='0' && text[end]<='9') || text[end]=='.' || text[end]==',') ++end;
        else if (text[end]==' ' && end+1<length && text[end+1]>='0' && text[end+1]<='9') ++end;
        else break;
    }
    if (begin==end || end-begin>128) return 0;
    /* A custom regex must cover one whole monetary expression, not several
       numbers. Neither partial numeric suffixes nor silent truncation survive. */
    for (size_t i=end;i<length;++i) if (text[i]>='0' && text[i]<='9') return 0;
    memcpy(hit->amount,text+begin,end-begin);
    for (size_t i=0;i<length;++i) {
        if (i+3<=length && text[i]>='A' && text[i]<='Z' && text[i+1]>='A' && text[i+1]<='Z' && text[i+2]>='A' && text[i+2]<='Z') {
            if (*hit->currency) return 0;
            memcpy(hit->currency,text+i,3); i+=2;
        } else if (i+3<=length && !memcmp(text+i,"\xe2\x82\xac",3)) { strcpy(hit->symbol,"euro"); i+=2; }
        else if (text[i]=='$') strcpy(hit->symbol,"dollar");
        else if (i+2<=length && !memcmp(text+i,"\xc2\xa3",2)) { strcpy(hit->symbol,"pound"); ++i; }
    }
    return 1;
}
int64_t shy_naive_money(const char *amount,const char *currency,const char *decimal,const char *group)
{
    int scale=shy_currency_scale(currency);
    if (scale<0 || strlen(amount)>128) return -1;
    char normalized[132]; size_t n=0,group_digits=0; int fraction=0,grouped=0;
    for (const char *p=amount;*p;++p) {
        if (*p>='0' && *p<='9') { normalized[n++]=*p; ++group_digits; }
        else if (*decimal && *p==*decimal && !fraction) {
            if (!group_digits || (grouped && group_digits!=3)) return -1;
            normalized[n++]='.'; fraction=1; group_digits=0;
        } else if (*group && *p==*group && !fraction) {
            if (!group_digits || (!grouped && group_digits>3) || (grouped && group_digits!=3)) return -1;
            grouped=1; group_digits=0;
        } else return -1;
    }
    if (!group_digits || (!fraction && grouped && group_digits!=3)) return -1;
    normalized[n]=0;
    mulib_money_t money;
    if (!mulib_money_parse(normalized,currency,(uint8_t)scale,&money) || money.minor<0 ||
        money.minor>INT64_C(1000000000000)) return -1;
    return money.minor;
}
