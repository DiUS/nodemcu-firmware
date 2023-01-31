#include <stdint.h>
#include <stdbool.h>
#include "esp_rom_sys.h"

static char space[]=" ";
static char blank[]=".. ";
static char brk[] = " | ";
static char addrfmt[]="\n%08x: ";
static char fmt[]="%02x ";
static char eol[]="\n";
static char onechar[]="%c";

void hexdump (char *src, uint32_t len, uint32_t base_addr)
{
  bool nl = true;
  char *p = (char *)base_addr;
  while (len)
  {
    uint32_t line_addr = (unsigned)p & ~0xf;
    union {
      uint32_t dw[4];
      char     b[16];
    } cache;
    for (int i = 0; i < 4; ++i)
      cache.dw[i] = ((uint32_t*)(p + (uint32_t)src - base_addr))[i];
    if (nl)
    {
      esp_rom_printf(addrfmt, line_addr);
      nl = false;
    }
    uint32_t skips = ((unsigned)p > line_addr) ?  ((unsigned)p - line_addr) : 0;
    uint32_t skipe = (len < 16) ? 16-len : 16;
    for (int i = 0; i < 16; ++i)
    {
      if (i < skips || i >= skipe)
        esp_rom_printf(blank);
      else
      {
        esp_rom_printf(fmt, cache.b[i]);
        ++p;
        --len;
      }
      if (i == 7)
        esp_rom_printf(space);
    }
    esp_rom_printf (brk);
    for (int i = 0; i < 16; ++i)
    {
      char c = ' ';
      if (i >= skips && i < skipe)
      {
        if (cache.b[i] >= 0x20 && cache.b[i] < 0x80)
          c = cache.b[i];
        else
          c = '.';
      }
      esp_rom_printf(onechar, c);
      if (i == 7)
        esp_rom_printf(space);
    }

    nl = true;
  }
  esp_rom_printf(eol);
}
