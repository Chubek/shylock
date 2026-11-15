#include "shylock.h"
#include <string.h>

/* Shared scalar context adapters, also linkable by isolated numeric tests. */
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
