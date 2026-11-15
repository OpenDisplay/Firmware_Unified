#include "od_rtt.h"
#include "SEGGER_RTT.h"
#include <sys/reent.h>

void od_rtt_init(void)
{
  SEGGER_RTT_Init();
}

int _write(int file, const char *ptr, int len)
{
  (void)file;
  if (len <= 0) {
    return 0;
  }
  SEGGER_RTT_Write(0, ptr, (unsigned)len);
  return len;
}

int _write_r(struct _reent *r, int file, const char *ptr, int len)
{
  (void)r;
  return _write(file, ptr, len);
}

#if OD_BG22_DIAG == 1
#include "od_diag.h"
#include <string.h>

void od_diag_line(const char *s)
{
  SEGGER_RTT_Write(0, s, (unsigned)strlen(s));
}

void od_diag_fatal_sc(const char *what, unsigned long sc)
{
  static const char hex[] = "0123456789ABCDEF";
  char code[4];
  unsigned i;

  for (i = 0u; i < 4u; i++) {
    code[i] = hex[(sc >> (12u - 4u * i)) & 0xFu];
  }
  od_diag_line("[OD] FATAL: ");
  od_diag_line(what);
  od_diag_line(" failed sc=0x");
  SEGGER_RTT_Write(0, code, 4u);
  od_diag_line("; entering bootloader\r\n");
}
#endif
