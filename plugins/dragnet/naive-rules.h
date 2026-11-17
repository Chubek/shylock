#ifndef SHYLOCK_NAIVE_RULES_H
#define SHYLOCK_NAIVE_RULES_H
/* Native hard ceilings, not rule-controlled privileges. Admission, defaults,
   strategies and semantic interpretation live in plugins/dragnet/dragnet_*.on1x. */
#define NF_QUERY_WORK 2000000u
#define NF_REGEX_WORK 16000000u
#define NF_PATTERN 256u
#endif
