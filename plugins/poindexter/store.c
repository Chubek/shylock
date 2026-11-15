#define _POSIX_C_SOURCE 200809L
#include "../../src/shylock.h"
#include "mulib-kvdb.h"
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static mulib_json_t *capture;
static mulib_kvdb_t *db;
static int lock_fd = -1;
static const char *text(const mulib_json_t *v, const char *key)
{
    size_t n;
    const char *s = mulib_json_text(mulib_json_get(v,key), &n);
    return s && strlen(s)==n ? s : "";
}
int64_t shy_store_close(void)
{
    mulib_json_destroy(capture); capture=NULL;
    mulib_kvdb_close(db); db=NULL;
    if(lock_fd>=0) { close(lock_fd); lock_fd=-1; }
    return 0;
}
int64_t shy_store_open(const char *path)
{
    shy_store_close();
    size_t n=strlen(path);
    char *lock=malloc(n+6);
    if(!lock) return 0;
    memcpy(lock,path,n); memcpy(lock+n,".lock",6);
    lock_fd=open(lock,O_CREAT|O_RDWR,0600); free(lock);
    if(lock_fd<0 || flock(lock_fd,LOCK_EX) ||
       mulib_kvdb_open(path,1,&db)!=MULIB_KVDB_OK) { shy_store_close(); return 0; }
    return 1;
}
int64_t shy_review_load(const char *path)
{
    size_t n;
    char *data=shy_read(path,SHY_PAGE_LIMIT,&n);
    if(!data) return 0;
    capture=mulib_json_parse(data,n,NULL); free(data);
    const char *hash=text(capture,"blake3");
    const mulib_json_t *stamp=mulib_json_get(capture,"captured_at");
    double t=mulib_json_numeric(stamp);
    return capture && strlen(hash)==64 && strspn(hash,"0123456789abcdef")==64 &&
        *text(capture,"source_url") && mulib_json_typeof(stamp)==MULIB_JSON_NUMBER &&
        t>=0 && t<=253402300799.0 && floor(t)==t &&
        mulib_json_typeof(mulib_json_get(capture,"candidates"))==MULIB_JSON_ARRAY;
}
static mulib_json_t *candidate(int64_t index)
{
    return index<0 ? NULL : mulib_json_at(mulib_json_get(capture,"candidates"),(size_t)index);
}
int64_t shy_review_amount(int64_t index)
{
    const mulib_json_t *v=mulib_json_get(candidate(index),"minor");
    double n=mulib_json_numeric(v);
    return mulib_json_typeof(v)==MULIB_JSON_NUMBER && n>=0 && n<=1e12 && floor(n)==n ? (int64_t)n : -1;
}
const char *shy_review_field(int64_t index,const char *key) { return text(candidate(index),key); }
int64_t shy_review_save(int64_t index,const char *cohort,const char *basis,const char *reviewer)
{
    if(!db || !candidate(index) || !*cohort || strlen(cohort)>128 ||
       !*reviewer || strlen(reviewer)>128) return 2;
    mulib_json_t *row=mulib_json_new(MULIB_JSON_OBJECT);
    char key[128];
    snprintf(key,sizeof key,"%s/%lld",text(capture,"blake3"),(long long)index);
    int ok=shy_add(row,"cohort",mulib_json_string(cohort)) &&
        shy_add(row,"reviewer",mulib_json_string(reviewer)) &&
        shy_add(row,"basis",mulib_json_string(basis)) &&
        shy_add(row,"currency",mulib_json_string(text(candidate(index),"currency"))) &&
        shy_add(row,"minor",mulib_json_number((double)shy_review_amount(index))) &&
        shy_add(row,"captured_at",mulib_json_number(mulib_json_numeric(mulib_json_get(capture,"captured_at")))) &&
        shy_add(row,"source_url",mulib_json_string(text(capture,"source_url"))) &&
        shy_add(row,"capture_blake3",mulib_json_string(text(capture,"blake3"))) &&
        shy_add(row,"candidate_index",mulib_json_number((double)index));
    char *encoded=ok ? mulib_json_stringify(row,0) : NULL;
    mulib_json_destroy(row);
    if(!encoded) return 2;
    const void *old; size_t old_n;
    int found=mulib_kvdb_get(db,key,strlen(key),&old,&old_n);
    if(found==MULIB_KVDB_OK) {
        int same=old_n==strlen(encoded) && !memcmp(old,encoded,old_n);
        free(encoded);
        return same ? 0 : 3; /* explicit conflict, never silently rewrite a review */
    }
    int rc=mulib_kvdb_set(db,key,strlen(key),encoded,strlen(encoded)); free(encoded);
    if(rc!=MULIB_KVDB_OK) return 2;
    return mulib_kvdb_commit(db)==MULIB_KVDB_OK ? 0 : 2;
}
int64_t shy_store_count(void) { return (int64_t)mulib_kvdb_size(db); }
static mulib_json_t *stored(int64_t index)
{
    const void *key,*value; size_t kn,vn;
    if(index<0 || mulib_kvdb_at(db,(size_t)index,&key,&kn,&value,&vn)!=MULIB_KVDB_OK) return NULL;
    return mulib_json_parse(value,vn,NULL);
}
int64_t shy_store_list(const char *cohort)
{
    mulib_json_t *rows=mulib_json_new(MULIB_JSON_ARRAY);
    for(size_t i=0;i<mulib_kvdb_size(db);++i) {
        mulib_json_t *row=stored((int64_t)i);
        if(!row) { mulib_json_destroy(rows); return 2; }
        if(*cohort && strcmp(text(row,"cohort"),cohort)) { mulib_json_destroy(row); continue; }
        if(!mulib_json_append(rows,row)) { mulib_json_destroy(row); mulib_json_destroy(rows); return 2; }
    }
    return shy_emit(rows);
}
int64_t shy_store_series(const char *cohort,const char *currency,int64_t start,int64_t step,int64_t count)
{
    if(!*cohort || strlen(currency)!=3 || start<0 || step<1 || count<2 || count>4096 ||
       step>(INT64_MAX-start)/count) return 2;
    double sum[4096]={0}; size_t totals[4096]={0};
    for(size_t i=0;i<mulib_kvdb_size(db);++i) {
        mulib_json_t *row=stored((int64_t)i);
        if(!row) return 2;
        if(!strcmp(text(row,"cohort"),cohort)) {
            if(strcmp(text(row,"currency"),currency) || strcmp(text(row,"basis"),"nightly")) {
                mulib_json_destroy(row); return 3;
            }
            int64_t time=(int64_t)mulib_json_numeric(mulib_json_get(row,"captured_at"));
            if(time>=start && time<start+step*count) {
                size_t bucket=(size_t)((time-start)/step);
                sum[bucket]+=mulib_json_numeric(mulib_json_get(row,"minor")); ++totals[bucket];
            }
        }
        mulib_json_destroy(row);
    }
    mulib_json_t *values=mulib_json_new(MULIB_JSON_ARRAY);
    for(int64_t i=0;i<count;++i) {
        if(!totals[i]) { mulib_json_destroy(values); return 4; }
        mulib_json_t *value=mulib_json_number(round(sum[i]/totals[i]));
        if(!value || !mulib_json_append(values,value)) {
            mulib_json_destroy(value); mulib_json_destroy(values); return 2;
        }
    }
    return shy_emit(values);
}
