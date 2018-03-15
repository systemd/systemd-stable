/* ANSI-C code produced by gperf version 3.1 */
/* Command-line: gperf  */
/* Computed positions: -k'' */


#include <stddef.h>
#include "conf-parser.h"
#include "networkd-conf.h"
#include <string.h>

#define TOTAL_KEYWORDS 2
#define MIN_WORD_LENGTH 13
#define MAX_WORD_LENGTH 16
#define MIN_HASH_VALUE 13
#define MAX_HASH_VALUE 16
/* maximum key range = 4, duplicates = 0 */

#ifdef __GNUC__
__inline
#else
#ifdef __cplusplus
inline
#endif
#endif
/*ARGSUSED*/
static unsigned int
networkd_gperf_hash (register const char *str, register size_t len)
{
  return len;
}

const struct ConfigPerfItem *
networkd_gperf_lookup (register const char *str, register size_t len)
{
  static const struct ConfigPerfItem wordlist[] =
    {
      {(char*)0}, {(char*)0}, {(char*)0}, {(char*)0},
      {(char*)0}, {(char*)0}, {(char*)0}, {(char*)0},
      {(char*)0}, {(char*)0}, {(char*)0}, {(char*)0},
      {(char*)0},
      {"DHCP.DUIDType",              config_parse_duid_type,                 0,          offsetof(Manager, duid.type)},
      {(char*)0}, {(char*)0},
      {"DHCP.DUIDRawData",           config_parse_duid_rawdata,              0,          offsetof(Manager, duid)}
    };

  if (len <= MAX_WORD_LENGTH && len >= MIN_WORD_LENGTH)
    {
      register unsigned int key = networkd_gperf_hash (str, len);

      if (key <= MAX_HASH_VALUE)
        {
          register const char *s = wordlist[key].section_and_lvalue;

          if (s && *str == *s && !strcmp (str + 1, s + 1))
            return &wordlist[key];
        }
    }
  return 0;
}
