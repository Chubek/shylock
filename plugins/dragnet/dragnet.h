#ifndef SHYLOCK_DRAGNET_H
#define SHYLOCK_DRAGNET_H
#include "mulib-fetch.h"
/* Injected transports are trusted synchronous callbacks. MuLIB still checks
   URL policy, response size and HTML type. Live transport pins public DNS. */
int shy_dragnet_https(void *, const char *, mulib_shylock_buffer_t *, unsigned *, char *, size_t);
typedef struct {
    /* Optional trusted resolver for embedding/tests; returns ONE numeric IPv4.
       Production NULL uses deadline-bounded DNS and checks all IPv4 answers.
       This callback must itself be bounded. Returned address is revalidated. */
    int (*resolve)(void *, const char *, char address[16]);
    void *context;
    unsigned deadline_ms; /* 1..20000; zero selects 20000 */
} shy_dragnet_transport;
int shy_dragnet_public_ipv4(const unsigned char address[4]);
int shy_dragnet_url_host(const char *url, char host[256]);
int shy_dragnet_get(const char *, mulib_fetch_transport_fn, void *, mulib_shylock_buffer_t *, unsigned *, char *, size_t);
int shy_dragnet_html_type(const char *);
#endif
