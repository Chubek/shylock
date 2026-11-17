#include "../plugins/dragnet/naive.h"
#include "../plugins/dragnet/workflow.h"
#include <stdlib.h>
#include <string.h>

int64_t shy_native_naive_test(void)
{
    const char html[]="<body><div class='card'><h2>A &amp; B</h2><p class='price'>€<b>180</b>.00 per night</p>"
        "<p hidden class='price'>EUR 1.00</p><meta class='amount' content='180.00'></div></body>";
    mulib_json_t *snapshot=shy_snapshot_record(html,strlen(html),"https://example.org/",1000,"saved",0,"text/html");
    mulib_json_t *record=mulib_json_new(MULIB_JSON_OBJECT);
    int ok=shy_add(record,"schema",mulib_json_string("shylock.naive-find/v1")) &&
        shy_add(record,"rule_source",mulib_json_string("{}"));
    if (ok) { ok=shy_add(record,"snapshot",snapshot); snapshot=NULL; }
    ok=ok && shy_naive_begin_record(record);
    int64_t card=shy_naive_query(0,".card",0),price=shy_naive_query(card,".price",0);
    ok=ok && card>0 && price>0 && shy_naive_query(card,".price",price)==0 &&
        !strcmp(shy_naive_text(price,""),"€180.00 per night") &&
        shy_naive_scan(price,"","€[0-9][0-9.]*")==1 &&
        !strcmp(shy_naive_hit_text(0,"amount"),"180.00") &&
        !strcmp(shy_naive_hit_text(0,"symbol"),"euro") &&
        !strcmp(shy_naive_hit_text(0,"span_kind"),"covering-text-fragments");
    int64_t at=shy_naive_hit_number(0,"offset"),span=shy_naive_hit_number(0,"span");
    ok=ok && at>=0 && span>0 && (size_t)(at+span)<=strlen(html) &&
        strstr(html+at,"<b>180</b>.00") && shy_naive_owner(at,card)>0 && strlen(shy_naive_span_digest(at,span))==64;
    int64_t attr=shy_naive_query(card,".amount",0);
    ok=ok && attr>0 && !strcmp(shy_naive_text(attr,"content"),"180.00") &&
        shy_naive_scan(attr,"content","[0-9][0-9.]*")==1 && !strcmp(shy_naive_hit_text(0,"span_kind"),"exact");
    ok=ok && shy_naive_money("1.250,00","EUR",",",".")==125000 &&
        shy_naive_money("1,250.00","USD",".",",")==125000 &&
        shy_naive_money("1,25.00","USD",".",",")==-1 && shy_naive_money("1,200","EUR",".","")==-1 &&
        shy_naive_money("99999999999999999999999999999","EUR",".","")==-1;
    ok=ok && shy_naive_regex_valid("(")==0 && shy_naive_regex_valid("a*")==0 &&
        shy_naive_regex_valid("[0-9]{999999}")==0 && shy_naive_regex_valid("[0-9]+")==1;
    shy_naive_close(); shy_naive_close();
    ok=ok && shy_naive_begin_record(record) && shy_naive_query(0,".card",0)!=card &&
        shy_naive_query(card,".price",0)==-1 && !shy_naive_verified();
    shy_naive_close();
    /* Regex-work rejection is sticky, and neither failures nor partial results
       can create a verified artifact. */
    ok=ok && shy_naive_begin_record(record);
    shy_nf.work=16000000;
    card=shy_naive_query(0,".card",0);
    ok=ok && shy_naive_scan(card,"","[0-9]+")==-1 && shy_naive_finish("{}")==0;
    shy_naive_close(); mulib_json_destroy(snapshot); mulib_json_destroy(record);
    return ok;
}
