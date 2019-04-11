#ifndef FIFO_H
#define FIFO_H

#ifndef UNIT_TEST
#include <c_types.h>
#else
#include <stdint.h>
#endif

typedef struct
{
  uint32_t timestamp;
  uint32_t value;
  uint32_t decimals;
  uint32_t tag;
} sample_t;

static uint32_t fifo_make_tag(const uint8_t* s)
{
  uint32_t tag=0;
  int i;
  for (i=0;i<4;i++)
  {
    if (!s[i])
      break;
    tag+=((uint32_t)(s[i]&0xff))<<(i*8);
  }
  return tag;
}

static inline void fifo_tag_to_string(uint32_t tag, uint8_t s[5])
{
  int i;
  s[4]=0;
  for (i=0;i<4;i++)
    s[i]=(tag>>(8*i))&0xff;
}

static inline uint32_t fifo_get_divisor(const sample_t* s)
{
  uint8_t decimals=s->decimals;
  uint32_t div=1;
  while (decimals--)
    div*=10;
  return div;
}

#endif
