#ifndef RTCMEM_H
#define RTCMEM_H

#include <c_types.h>

#define DIUS_MAGIC 0x44695553


// Layout of the RTC storage space for DiUS sensor applications:
//
// 0: Magic. If set to DIUS_MAGIC, the rest is valid. If not, continue to proper boot
//
// 1: time_of_day, seconds.
// 2: time_of_day, microseconds.
// 3: frc counter for timestamp given in (1:2), bottom 32 bits
// 4: frc counter for timestamp given in (1:2), top 32 bits
// 5: bottom 32 bits of frc at last read
// 6: top 32 bits of frc at last read (maintained by software)
//
// 7: cached result of sleep clock calibration. Has the format of system_rtc_clock_cali_proc(),
//    or 0 if not available
// (see 16/17 below)
//
// (1:2) set to 0 if no time information is available.
//
// 8: measurement alignment, in microseconds
// 9: timestamp for next sample (seconds). For sensors which sense during the sleep phase. Set to
//    0 to indicate no sample waiting. Simply do not use for sensors which deliver values prior to
//    deep sleep.

// 10: Number of samples to take before doing a "real" boot. Decremented as samples are obtained
// 11: Reload value for (10). Needs to be applied by the firmware in the real boot (rtc_restart_samples_to_take())
//
// 12: FIFO location. First FIFO address in bits 0:7, first non-FIFO address in bits 8:15. Total must
//     be a multiple of 3!
// 13: Number of samples in FIFO.
// 14: FIFO tail (where next sample will be written. Increments by 3 for each sample)
// 15: FIFO head (where next sample will be read. Increments by 3 for each sample)
//
// 16: Number of microseconds we tried to sleep, or 0 if we didn't sleep since last calibration, ffffffff if invalid
// 17: Number of RTC cycles we decided to sleep, or 0 if we didn't sleep since last calibration, ffffffff if invalid
// 18: Number of microseconds which we add to (1/2) to avoid time going backwards
// 19: microsecond value returned in the last gettimeofday() to "user space".
//
//     Entries 16 to 18 are needed because the RTC cycles/second appears quite temperature dependent,
//     and thus is heavily influenced by what else the chip is doing. As such, any calibration against
//     the crystal-provided clock (which necessarily would have to happen while the chip is active and
//     burning a few milliwatts) will be significantly different from the actual frequency during deep
//     sleep.
//     Thus, in order to calibrate for deep sleep conditions, we keep track of total sleep microseconds
//     and total sleep clock cycles between settimeofday() calls (which presumably are NTP driven), and
//     adjust the calibration accordingly on each settimeofday(). This will also track frequency changes
//     due to ambient temperature changes.
//     18/19 get used when a settimeofday() would result in turning back time. As that can cause all sorts
//     of ugly issues, we *do* adjust (1/2), but compensate by making the same adjustment to (18). Then each
//     time gettimeofday() is called, we inspect (19) and determine how much time has passed since the last
//     call (yes, this gets it wrong if more than a second has passed, but not in a way that causes issues)
//     and try to take up to 6% of that time away from (18) until (18) reaches 0. Also, whenever we go to
//     deep sleep, we try to take (18) out of the sleep time.
//     Note that for calculating the next sample-aligned wakeup, we need to use the post-adjustment
//     timeofday(), but for calculating actual sleep time, we use the pre-adjustment one, thus bringing
//     things back into line.
//
// 20-25: Debugging data
//
// 32-127: FIFO space. Each FIFO entry uses three slots:
//     n+0: timestamp (in seconds UTC)
//     n+1: value
//     n+2: 4 byte ASCII tag. bits 0:6 first char, 8:14 second char, etc. 4 chars max, or use 0 for unused
//          Note that the top bits of each byte are used to encode the number of desired decimals instead,
//          i.e. if decimals=2 and value=366184, then reported value should be 3661.84
#define RTC_MAGIC_POS     0
#define RTC_TODS_POS      1
#define RTC_TODUS_POS     2
#define RTC_COUNTL_POS    3
#define RTC_COUNTH_POS    4
#define RTC_LASTREADL_POS 5
#define RTC_LASTREADH_POS 6
#define RTC_CALIBRATION_POS 7

#define RTC_ALIGNMENT_POS 8
#define RTC_TIMESTAMP_POS 9

#define RTC_SAMPLESTOTAKE_POS 10
#define RTC_SAMPLESPERBOOT_POS 11

#define RTC_FIFOLOC_POS   12
#define RTC_FIFOCOUNT_POS 13
#define RTC_FIFOTAIL_POS  14
#define RTC_FIFOHEAD_POS  15

#define RTC_SLEEPTOTALUS_POS     16
#define RTC_SLEEPTOTALCYCLES_POS 17
#define RTC_TODOFFSETUS_POS      18
#define RTC_LASTTODUS_POS        19

#define RTC_DEFAULT_FIFO_START 32
#define RTC_DEFAULT_FIFO_END  128
#define RTC_DEFAULT_FIFO_LOC (RTC_DEFAULT_FIFO_START + (RTC_DEFAULT_FIFO_END<<8))

typedef struct
{
  uint32_t timestamp;
  uint32_t value;
  uint32_t tag;
} sample_t;



static inline uint32_t rtc_mem_read(uint32_t addr)
{
  return ((uint32_t*)0x60001200)[addr];
}

static inline void rtc_mem_write(uint32_t addr, uint32_t val)
{
  ((uint32_t*)0x60001200)[addr]=val;
}

static inline uint64_t rtc_make64(uint32_t high, uint32_t low)
{
  return (((uint64_t)high)<<32)|low;
}

static inline uint64_t rtc_mem_read64(uint32_t addr)
{
  return rtc_make64(rtc_mem_read(addr+1),rtc_mem_read(addr));
}

static inline void rtc_mem_write64(uint32_t addr, uint64_t val)
{
  rtc_mem_write(addr+1,val>>32);
  rtc_mem_write(addr,val&0xffffffff);
}

static inline uint32_t rtc_fifo_get_tail(void)
{
  return rtc_mem_read(RTC_FIFOTAIL_POS);
}

static inline void rtc_fifo_put_tail(uint32_t val)
{
  rtc_mem_write(RTC_FIFOTAIL_POS,val);
}

static inline uint32_t rtc_fifo_get_head(void)
{
  return rtc_mem_read(RTC_FIFOHEAD_POS);
}

static inline void rtc_fifo_put_head(uint32_t val)
{
  rtc_mem_write(RTC_FIFOHEAD_POS,val);
}

static inline uint32_t rtc_fifo_get_count(void)
{
  return rtc_mem_read(RTC_FIFOCOUNT_POS);
}

static inline void rtc_fifo_put_count(uint32_t val)
{
  rtc_mem_write(RTC_FIFOCOUNT_POS,val);
}

static inline uint32_t rtc_fifo_get_last(void)
{
  return (rtc_mem_read(RTC_FIFOLOC_POS)>>8)&0xff;
}

static inline uint32_t rtc_fifo_get_first(void)
{
  return (rtc_mem_read(RTC_FIFOLOC_POS)>>0)&0xff;
}

static inline void rtc_fifo_put_loc(uint32_t first, uint32_t last)
{
  rtc_mem_write(RTC_FIFOLOC_POS,first+(last<<8));
}

static inline uint32_t rtc_fifo_normalise_index(uint32_t index)
{
  if (index>=rtc_fifo_get_last())
    index=rtc_fifo_get_first();
  return index;
}

static inline void rtc_fifo_increment_count(void)
{
  rtc_fifo_put_count(rtc_fifo_get_count()+1);
}

static inline void rtc_fifo_decrement_count(void)
{
  rtc_fifo_put_count(rtc_fifo_get_count()-1);
}



static inline uint32_t rtc_get_samples_to_take(void)
{
  return rtc_mem_read(RTC_SAMPLESTOTAKE_POS);
}

static inline void rtc_put_samples_to_take(uint32_t val)
{
  rtc_mem_write(RTC_SAMPLESTOTAKE_POS,val);
}

static inline void rtc_decrement_samples_to_take(void)
{
  uint32_t stt=rtc_get_samples_to_take();
  if (stt)
    rtc_put_samples_to_take(stt-1);
}

static inline void rtc_restart_samples_to_take(void)
{
  rtc_put_samples_to_take(rtc_mem_read(RTC_SAMPLESPERBOOT_POS));
}

// returns 1 if sample popped, 0 if not
static inline int8_t rtc_fifo_pop_sample(sample_t* dst)
{
  if (rtc_fifo_get_count()==0)
    return 0;
  uint32_t head=rtc_fifo_get_head();
  dst->timestamp=rtc_mem_read(head++);
  dst->value=rtc_mem_read(head++);
  dst->tag=rtc_mem_read(head++);
  rtc_fifo_put_head(rtc_fifo_normalise_index(head));
  rtc_fifo_decrement_count();
  return 1;
}

// returns 1 if sample is available, 0 if not
static inline int8_t rtc_fifo_peek_sample(sample_t* dst, uint32_t from_top)
{
  if (rtc_fifo_get_count()<=from_top)
    return 0;
  uint32_t head=rtc_fifo_get_head();
  while (from_top--)
    head=rtc_fifo_normalise_index(head+3);
  dst->timestamp=rtc_mem_read(head++);
  dst->value=rtc_mem_read(head++);
  dst->tag=rtc_mem_read(head++);
  return 1;
}

static inline void rtc_fifo_drop_samples(uint32_t from_top)
{
  if (rtc_fifo_get_count()<=from_top)
    from_top=rtc_fifo_get_count();
  uint32_t head=rtc_fifo_get_head();
  while (from_top--)
  {
    head=rtc_fifo_normalise_index(head+3);
    rtc_fifo_decrement_count();
  }
  rtc_fifo_put_head(head);
}


static inline void rtc_fifo_store_sample(const sample_t* s)
{
  uint32_t head=rtc_fifo_get_head();
  uint32_t tail=rtc_fifo_get_tail();

  if (head==tail && rtc_fifo_get_count()>0)
  { // Full! Need to remove a sample
    sample_t dummy;
    rtc_fifo_pop_sample(&dummy);
  }
  rtc_mem_write(tail++,s->timestamp);
  rtc_mem_write(tail++,s->value);
  rtc_mem_write(tail++,s->tag);
  rtc_fifo_put_tail(rtc_fifo_normalise_index(tail));
  rtc_fifo_increment_count();
}

static uint32_t rtc_fifo_make_tag(const uint8_t* s, uint8_t decimals)
{
  uint32_t tag=0;
  int i;
  for (i=0;i<4;i++)
  {
    if (!s[i])
      break;
    tag+=((uint32_t)(s[i]&0x7f))<<(i*8);
  }
  for (i=0;i<4;i++)
    if (decimals&(1<<i))
      tag+=(128<<(8*i));

  return tag;
}

static void rtc_tag_to_string(uint32_t tag, uint8_t s[5])
{
  int i;
  s[4]=0;
  for (i=0;i<4;i++)
    s[i]=(tag>>(8*i))&0x7f;
}

static uint8_t rtc_tag_to_decimals(uint32_t tag)
{
  int i;
  uint8_t decimals=0;

  for (i=0;i<4;i++)
    if ((tag>>(8*i))&0x80)
      decimals+=(1<<i);
  return decimals;
}

static uint32_t rtc_tag_to_divisor(uint32_t tag)
{
  uint8_t decimals=rtc_tag_to_decimals(tag);
  uint32_t div=1;
  while (decimals--)
    div*=10;
  return div;
}

static inline void rtc_fifo_init(uint32_t first, uint32_t last)
{
  rtc_fifo_put_loc(first,last);
  rtc_fifo_put_tail(first);
  rtc_fifo_put_head(first);
  rtc_fifo_put_count(0);
}

static inline void rtc_fifo_init_default(void)
{
  rtc_fifo_init(RTC_DEFAULT_FIFO_START,RTC_DEFAULT_FIFO_END);
}


static inline uint8_t rtc_check_magic(void)
{
  if (rtc_mem_read(RTC_MAGIC_POS)==DIUS_MAGIC)
    return 1;
  return 0;
}

static inline void rtc_set_magic(void)
{
  rtc_mem_write(RTC_MAGIC_POS,DIUS_MAGIC);
}

static inline void rtc_unset_magic(void)
{
  rtc_mem_write(RTC_MAGIC_POS,0);
}

static inline void rtc_reset_timekeeping(bool clear_cali)
{
  rtc_mem_write(RTC_TODS_POS,0);
  rtc_mem_write(RTC_TODUS_POS,0);
  rtc_mem_write64(RTC_COUNTL_POS,0);
  rtc_mem_write64(RTC_LASTREADL_POS,0);
  rtc_mem_write(RTC_SLEEPTOTALUS_POS,0);
  rtc_mem_write(RTC_SLEEPTOTALCYCLES_POS,0);
  rtc_mem_write(RTC_TODOFFSETUS_POS,0);
  rtc_mem_write(RTC_LASTTODUS_POS,0);
  if (clear_cali)
    rtc_mem_write(RTC_CALIBRATION_POS,0);
}

static inline uint8_t rtc_have_time(void)
{
  return (rtc_check_magic() && rtc_mem_read(RTC_TODS_POS)!=0);
}

static inline void rtc_dius_prepare(uint32_t samples_per_boot, uint32_t us_per_sample)
{
  rtc_mem_write(RTC_SAMPLESPERBOOT_POS,samples_per_boot);
  rtc_mem_write(RTC_ALIGNMENT_POS,us_per_sample);

  rtc_put_samples_to_take(0);
  rtc_fifo_init_default();
  rtc_reset_timekeeping(true);
  rtc_set_magic();
}

static inline void rtc_dius_disprepare(void)
{
  rtc_unset_magic();
}


#endif
