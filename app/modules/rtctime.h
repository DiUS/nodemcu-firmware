#ifndef RTCTIME_H
#define RTCTIME_H

#include "rtcmem.h"

#define RTC_MMIO_BASE 0x60000700
#define RTC_TARGET_ADDR 0x04
#define RTC_COUNTER_ADDR 0x1c

static inline uint64_t rtc_get_now_us_adjusted(void);

struct rtc_timeval
{
  uint32_t tv_sec;
  uint32_t tv_usec;
};


static inline void rtc_memw(void)
{
  asm volatile ("memw");
}

static inline void rtc_reg_write(uint32_t addr, uint32_t val)
{
  rtc_memw();
  addr+=RTC_MMIO_BASE;
  *((volatile uint32_t*)addr)=val;
  rtc_memw();
}

static inline uint32_t rtc_reg_read(uint32_t addr)
{
  addr+=RTC_MMIO_BASE;
  rtc_memw();
  return *((volatile uint32_t*)addr);
}

static inline uint32_t rtc_read_raw(void)
{
  return rtc_reg_read(RTC_COUNTER_ADDR);
}

static inline uint32_t rtc_read_frc(void)
{
  return *((volatile uint32_t*)0x60000624);
}


static inline uint32_t dius_rtc_cali(void)
{
  uint32_t frc_start=rtc_read_frc();
  uint32_t rtc_start=rtc_read_raw();

  uint32_t frc_end;
  uint32_t rtc_end;

  while (1)
  {
    frc_end=rtc_read_frc();
    rtc_end=rtc_read_raw();
    if (frc_end-frc_start>600)
      break;
  }

  return 4096*32*(frc_end-frc_start)/(10*(rtc_end-rtc_start));
}


static inline uint64_t rtc_frc_get_current(void)
{
  uint32_t low_bits=rtc_read_frc();
  uint32_t prev_low_bits=rtc_mem_read(RTC_LASTREADL_POS);
  uint32_t high_bits=rtc_mem_read(RTC_LASTREADH_POS);
  if (low_bits<prev_low_bits)
  {
    high_bits++;
    rtc_mem_write(RTC_LASTREADH_POS,high_bits);
  }
  rtc_mem_write(RTC_LASTREADL_POS,low_bits);
  return rtc_make64(high_bits,low_bits);
}

static inline void rtc_register_time_reached(uint32_t s, uint32_t us)
{
  rtc_mem_write(RTC_LASTTODUS_POS,us);
}

static inline uint32_t rtc_us_since_time_reached(uint32_t s, uint32_t us)
{
  uint32_t lastus=rtc_mem_read(RTC_LASTTODUS_POS);
  if (us<lastus)
    us+=1000000;
  return us-lastus;
}

static inline void rtc_settimeofday(const struct rtc_timeval* tv)
{
  if (!rtc_check_magic())
    return;

  uint32_t sleep_us=rtc_mem_read(RTC_SLEEPTOTALUS_POS);
  uint32_t sleep_cycles=rtc_mem_read(RTC_SLEEPTOTALCYCLES_POS);
  uint64_t now_esp_us=rtc_get_now_us_adjusted();
  uint64_t now_ntp_us=((uint64_t)tv->tv_sec)*1000000+tv->tv_usec;
  int64_t  diff_us=now_esp_us-now_ntp_us;

  // Store the *actual* time
  uint64_t now=rtc_frc_get_current();
  rtc_mem_write(RTC_TODS_POS,tv->tv_sec);
  rtc_mem_write(RTC_TODUS_POS,tv->tv_usec);
  rtc_mem_write64(RTC_COUNTL_POS,now);

  rtc_mem_write(27,sleep_us);
  rtc_mem_write(28,sleep_cycles);

  // calibrate sleep period based on difference between expected time and actual time
  if (sleep_us>0 && sleep_us<0xffffffff &&
      sleep_cycles>0 && sleep_cycles<0xffffffff)
  {
    uint64_t actual_sleep_us=sleep_us-diff_us;
    rtc_mem_write(29,actual_sleep_us);
    uint32_t cali=(actual_sleep_us<<12)/sleep_cycles;
    rtc_mem_write(RTC_CALIBRATION_POS,cali);
  }
  else
    rtc_mem_write(29,0);

  rtc_mem_write(RTC_SLEEPTOTALUS_POS,0);
  rtc_mem_write(RTC_SLEEPTOTALCYCLES_POS,0);

  // Deal with time adjustment if necessary
  if (diff_us>0) // Time went backwards. Avoid that....
  {
    if (diff_us>0xffffffffULL)
      diff_us=0xffffffffULL;
    now_ntp_us+=diff_us;
  }
  else
    diff_us=0;
  rtc_mem_write(RTC_TODOFFSETUS_POS,diff_us);

  uint32_t now_s=now_ntp_us/1000000;
  uint32_t now_us=now_ntp_us%1000000;
  rtc_register_time_reached(now_s,now_us);
}

static inline uint32_t rtc_get_calibration(void)
{
  uint32_t cal=rtc_mem_read(RTC_CALIBRATION_POS);
  if (!cal)
  {
    // Make a first guess, most likely to be rather bad, but better then nothing.
#ifndef BOOTLOADER_CODE // This will pull in way too much of the system for the bootloader to handle.
    ets_delay_us(200);
    cal=system_rtc_clock_cali_proc();
    rtc_mem_write(RTC_CALIBRATION_POS,cal);
#else
    cal=6<<12;
#endif
  }
  return cal;
}

// Call this before going to sleep from proper firmware, and a brand new calibration will be done and stored
static inline void rtc_invalidate_calibration(void)
{
  rtc_mem_write(RTC_CALIBRATION_POS,0);
}

static inline uint64_t rtc_us_to_ticks(uint64_t us)
{
  uint32_t cal=rtc_get_calibration();

  rtc_mem_write(20,cal);

  return (us<<12)/cal;
}


// frc ticks are exactly 3.2us long (80MHz clock, 256 clock cycles per tick)
static inline uint64_t rtc_frc_ticks_to_us(uint64_t ticks)
{
  return ticks*32/10; //
}

static inline uint64_t rtc_us_to_frc_ticks(uint64_t us)
{
  return us*10/32;
}

static inline uint64_t rtc_get_todcount(void)
{
  return rtc_mem_read64(RTC_COUNTL_POS);
}

static inline uint64_t rtc_get_todus(void)
{
  return ((uint64_t)rtc_mem_read(RTC_TODS_POS))*1000000+rtc_mem_read(RTC_TODUS_POS);
}

static inline uint64_t rtc_get_now_us_raw(void)
{
  if (!rtc_check_magic())
    return 0;

  uint64_t ref_tod_us=rtc_get_todus();
  if (ref_tod_us==0) // Now time info available
    return 0;

  uint64_t ref_rtc=rtc_get_todcount();
  uint64_t now_rtc=rtc_frc_get_current();
  uint64_t diff_us=rtc_frc_ticks_to_us(now_rtc-ref_rtc);
  return ref_tod_us+diff_us;
}

static inline uint64_t rtc_get_now_us_adjusted(void)
{
  uint64_t raw=rtc_get_now_us_raw();
  if (!raw)
    return 0;
  return raw+rtc_mem_read(RTC_TODOFFSETUS_POS);
}

static inline void rtc_gettimeofday(struct rtc_timeval* tv)
{

  uint64_t now=rtc_get_now_us_adjusted();
  uint32_t sec=now/1000000;
  uint32_t usec=now%1000000;
  uint32_t to_adjust=rtc_mem_read(RTC_TODOFFSETUS_POS);
  if (to_adjust)
  {
    uint32_t us_passed=rtc_us_since_time_reached(sec,usec);
    uint32_t adjust=us_passed>>4;
    if (adjust)
    {
      if (adjust>to_adjust)
        adjust=to_adjust;
      to_adjust-=adjust;
      now-=adjust;
      now/1000000;
      now%1000000;
      rtc_mem_write(RTC_TODOFFSETUS_POS,to_adjust);
    }
  }
  tv->tv_sec=sec;
  tv->tv_usec=usec;
  rtc_register_time_reached(sec,usec);
}

static inline void rtc_add_sleep_tracking(uint32_t us, uint32_t cycles)
{
  // us is the one that will grow faster...
  uint32_t us_before=rtc_mem_read(RTC_SLEEPTOTALUS_POS);
  uint32_t us_after=us_before+us;
  uint32_t cycles_after=rtc_mem_read(RTC_SLEEPTOTALCYCLES_POS)+cycles;

  if (us_after<us_before) // Give up if it would cause an overflow
  {
    us_after=cycles_after=0xffffffff;
  }
  rtc_mem_write(RTC_SLEEPTOTALUS_POS,    us_after);
  rtc_mem_write(RTC_SLEEPTOTALCYCLES_POS,cycles_after);
}

static void rtc_enter_deep_sleep_us(uint32_t us)
{
  rtc_reg_write(0,0);
  rtc_reg_write(0,rtc_reg_read(0)&0xffffbfff);
  rtc_reg_write(0,rtc_reg_read(0)|0x30);

  rtc_reg_write(0x44,4);
  rtc_reg_write(0x0c,0x00010010);

  rtc_reg_write(0x48,(rtc_reg_read(0x48)&0xffff01ff)|0x0000fc00);
  rtc_reg_write(0x48,(rtc_reg_read(0x48)&0xfffffe00)|0x00000080);

  rtc_reg_write(RTC_TARGET_ADDR,rtc_read_raw()+136);
  rtc_reg_write(0x18,8);
  rtc_reg_write(0x08,0x00100010);

  ets_delay_us(20);

  rtc_reg_write(0x9c,17);
  rtc_reg_write(0xa0,3);

  rtc_reg_write(0x0c,0x640c8);
  rtc_reg_write(0,rtc_reg_read(0)&0xffffffcf);

  uint32_t cycles=rtc_us_to_ticks(us);
  rtc_add_sleep_tracking(us,cycles);
  // Debug logging
  rtc_mem_write(21,cycles);
  rtc_mem_write(22,us);
  rtc_mem_write(23,rtc_read_raw());

  rtc_reg_write(RTC_TARGET_ADDR,rtc_read_raw()+cycles);
  rtc_reg_write(0x9c,17);
  rtc_reg_write(0xa0,3);

  // Clear bit 0 of DPORT 0x04. Doesn't seem to be necessary
  // wm(0x3fff0004,bitrm(0x3fff0004),0xfffffffe));
  rtc_reg_write(0x40,-1);
  rtc_reg_write(0x44,32);
  rtc_reg_write(0x10,0);

  rtc_reg_write(0x18,8);
  rtc_reg_write(0x08,0x00100000); //  go to sleep
}

static inline void rtc_deep_sleep_us(uint32_t us)
{
  uint32_t to_adjust=rtc_mem_read(RTC_TODOFFSETUS_POS);
  if (to_adjust)
  {
    us+=to_adjust;
    rtc_mem_write(RTC_TODOFFSETUS_POS,0);
  }
  uint64_t now=rtc_get_now_us_raw(); // Now the same as _adjusted()
  if (now)
  { // Need to maintain the clock first. When we wake up, counter will be 0
    uint64_t wakeup=now+us;
    rtc_mem_write(RTC_TODS_POS,wakeup/1000000);
    rtc_mem_write(RTC_TODUS_POS,wakeup%1000000);
    rtc_mem_write64(RTC_COUNTL_POS,0);
    rtc_mem_write64(RTC_LASTREADL_POS,0);
  }

  rtc_mem_write(24,now%1000000000);
  rtc_enter_deep_sleep_us(us);
}

static inline void rtc_deep_sleep_until_sample(uint32_t min_sleep_us)
{
  uint64_t now=rtc_get_now_us_adjusted();
  uint64_t then=now+min_sleep_us;
  uint32_t align=rtc_mem_read(RTC_ALIGNMENT_POS);

  if (align)
  {
    then+=align-1;
    then-=(then%align);
  }
  rtc_deep_sleep_us(then-now);
}


static inline void rtc_time_register_bootup(void)
{
  uint64_t count=rtc_mem_read64(RTC_COUNTL_POS);
  uint64_t lastread=rtc_mem_read64(RTC_LASTREADL_POS);
  uint32_t reset_reason=rtc_get_reset_reason();

  if (count!=0 || lastread!=0 || reset_reason!=2)
  { // This was *not* a proper wakeup from a deep sleep. All our time keeping is f*cked!
    rtc_reset_timekeeping(false); // Keep the calibration, it should still be good
  }
  rtc_mem_write(30,reset_reason);
}

#endif
