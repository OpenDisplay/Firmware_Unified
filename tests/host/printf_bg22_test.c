/* Checks targets/efr32bg22-slc/od_printf.c against the host C library for every supported
 * conversion/flag/width/precision combination (tests/host/CMakeLists.txt renames its entry points). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
int od_t_snprintf(char *, size_t, const char *, ...);
int od_t_printf(const char *, ...);
int od_t_puts(const char *);
static char wbuf[4096]; static size_t wlen;
int _write(int f, const char *p, int n) { (void)f; memcpy(wbuf + wlen, p, (size_t)n); wlen += (size_t)n; return n; }
static int fails, checks;
static void cmp(const char *fmt, const char *a, int ra, const char *b, int rb) {
  checks++;
  if (ra != rb || strcmp(a, b) != 0) { if (fails++ < 20) fprintf(stderr, "FAIL fmt=[%s] od=[%s]%d libc=[%s]%d\n", fmt, a, ra, b, rb); }
}
#define CHECK1(fmt, ...) do { char a[256], b[256]; int ra = od_t_snprintf(a, sizeof a, fmt, __VA_ARGS__); \
  int rb = snprintf(b, sizeof b, fmt, __VA_ARGS__); cmp(fmt, a, ra, b, rb); \
  wlen = 0; int rp = od_t_printf(fmt, __VA_ARGS__); wbuf[wlen] = 0; cmp(fmt, wbuf, rp, b, rb); } while (0)
int main(void) {
  static const int ivals[] = { 0, 1, -1, 7, 9, 10, 255, -255, 4096, 65535, 123456789, INT_MAX, INT_MIN };
  static const unsigned uvals[] = { 0u, 1u, 9u, 10u, 15u, 16u, 255u, 4096u, 65535u, 0xDEADBEEFu, UINT_MAX };
  static const char *flags[] = { "", "-", "0", "+", " ", "-0", "+0", " 0", "-+" };
  static const char *widths[] = { "", "1", "2", "4", "6", "8", "12" };
  static const char *precs[] = { "", ".0", ".1", ".3", ".8" };
  static const char *convs_i[] = { "d", "i", "ld" };
  static const char *convs_u[] = { "u", "x", "X", "lu", "lx", "lX" };
  char fmt[64];
  for (size_t f = 0; f < sizeof flags / sizeof *flags; f++)
  for (size_t w = 0; w < sizeof widths / sizeof *widths; w++)
  for (size_t p = 0; p < sizeof precs / sizeof *precs; p++) {
    for (size_t c = 0; c < 3; c++) for (size_t v = 0; v < sizeof ivals / sizeof *ivals; v++) {
      snprintf(fmt, sizeof fmt, "<%%%s%s%s%s>", flags[f], widths[w], precs[p], convs_i[c]);
      if (convs_i[c][0] == 'l') CHECK1(fmt, (long)ivals[v]); else CHECK1(fmt, ivals[v]); }
    if (strchr(flags[f], '+') || strchr(flags[f], ' ')) continue; /* + and space apply to signed conversions only */
    for (size_t c = 0; c < 6; c++) for (size_t v = 0; v < sizeof uvals / sizeof *uvals; v++) {
      snprintf(fmt, sizeof fmt, "<%%%s%s%s%s>", flags[f], widths[w], precs[p], convs_u[c]);
      if (convs_u[c][0] == 'l') CHECK1(fmt, (unsigned long)uvals[v]); else CHECK1(fmt, uvals[v]); }
    if (!strchr(flags[f], '0') && !strchr(flags[f], '+') && !strchr(flags[f], ' ')) {
      static const char *svals[] = { "", "a", "abc", "hello world" };
      for (size_t v = 0; v < 4; v++) { snprintf(fmt, sizeof fmt, "<%%%s%s%ss>", flags[f], widths[w], precs[p]); CHECK1(fmt, svals[v]); }
      if (precs[p][0] == 0) { snprintf(fmt, sizeof fmt, "<%%%s%sc>", flags[f], widths[w]); CHECK1(fmt, 'Q'); }
    }
  }
  /* star width / precision, %%, %.*s, mixed real-format shapes from the firmware */
  for (int w = -6; w <= 6; w++) for (int p = -2; p <= 5; p++) { CHECK1("[%*.*d]", w, p, -42); CHECK1("[%*.*s]", w, p, "xyzzy"); CHECK1("[%.*s]", p, "xyzzy"); }
  CHECK1("100%% %u%%", 5u);
  CHECK1("[OD] pipe notify permanent failure sc=0x%04lX len=%u\r\n", 0x1234ul, 20u);
  CHECK1("%02X:%02X:%02X %06lX %08lX %04lu %03lu %01u %02u %c", 0xAu, 0xBCu, 0u, 0xABCDEul, 0x1ul, 7ul, 1234ul, 0u, 3u, 'z');
  /* truncation and size 0 */
  { char a[8]; int r = od_t_snprintf(a, sizeof a, "%s-%u", "abcdef", 12345u); checks++; if (r != 12 || strcmp(a, "abcdef-")) { fails++; fprintf(stderr, "trunc [%s] %d\n", a, r); } }
  { char a[1] = { 'x' }; int r = od_t_snprintf(a, 0, "%u", 99u); checks++; if (r != 2 || a[0] != 'x') { fails++; fprintf(stderr, "size0\n"); } }
  /* long output through the 32 B chunking */
  { char b[300]; memset(b, 'k', 299); b[299] = 0; wlen = 0; int r = od_t_printf("%s|%d", b, -7); wbuf[wlen] = 0; char e[320]; snprintf(e, sizeof e, "%s|%d", b, -7); cmp("long", wbuf, r, e, (int)strlen(e)); }
  { wlen = 0; int r = od_t_puts("line"); wbuf[wlen] = 0; cmp("puts", wbuf, r, "line\n", 5); }
  /* unsupported conversions are copied verbatim */
  { char a[64]; od_t_snprintf(a, sizeof a, "a%pb%5.2fc", 1); checks++; if (strcmp(a, "a%pb%5.2fc")) { fails++; fprintf(stderr, "verbatim [%s]\n", a); } }
  printf("od_printf: %d checks, %d failures\n", checks, fails);
  return fails != 0;
}
