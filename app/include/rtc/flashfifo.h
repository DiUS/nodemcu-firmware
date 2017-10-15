/*
 * Copyright 2015 Dius Computing Pty Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the
 *   distribution.
 * - Neither the name of the copyright holders nor the names of
 *   its contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @author Bernd Meyer <bmeyer@dius.com.au>
 */
#ifndef _FLASHFIFO_H_
#define _FLASHFIFO_H_

// The flash fifo consists of a number N of sectors. The first three sectors are special:
//
//   * sector 0: Header, containing static information about the fifo
//   * sector 1: "counter" for the current head sector
//   * sector 2: "counter" for the current tail sector
//   * sector 3...N-1: data sectors
//
// The "counter" sectors are viewed as a collection of 32768 bits, each of which corresponds to
// one (data) sector. The counter value is simply the index of the first bit which is a 1.
// Thus, a freshly erased "counter" sector has the value 0, and the counter can be incremented
// by successively clearing bits. The counter cannot be decremented (can't write a '1' to flash),
// but can be reset to 0 (by erasing).
//
// Data sectors consist of two parts --- a counter part, and a data part. The counter part
// is similar to the counter sectors described above, but smaller. 32 bytes (256 bits) each
// are used for head and tail counters, with the rest of the sector used for fifo entries.
// Fifo entries are self-contained (unlike in the rtc fifo), and thus take 16 bytes each. This
// gives the following layout for data sectors:
//
//   * Bytes    0-31:   head counter within the sector
//   * Bytes   32-63:   tail counter within the sector
//   * Bytes   64-4095: 252 fifo data entries, 16 bytes each
//
// Each data entry has the following structure (which is the same as a "sample_t" in rtc_fifo)
//   * Bytes  0-3:  timestamp, in unix UTC seconds
//   * Bytes  4-7:  raw data value
//   * Bytes  8-11: decimals
//   * Bytes 12-15: tag (up to 4 ASCII characters, zero-filled if shorter)
//
// Both counter sectors and in-data-sector counters shall never reach a state of being all-zeroes.
// This is pretty much a given for the counter sectors (they can count to 32767 before overflowing,
// or 128MB of fifo space), and also holds for the in-sector counters (at 16 byte per sample, we
// can store 252 entries in the 4032 data bytes of the data sectors, so the counters can never reach
// 253, yet they only overflow at 255).
//
// The header sector is used to identify a fifo, and provide its basic parameters (some of which are
// given as concrete numbers above, for the sake of understanding):
//
//    * Bytes 0-3: FLASH_FIFO_MAGIC
//    * Bytes 4-7: sector size   (ESP8266: 4096)
//    * Bytes 8-11: sector number of "head sector counter" (typically: this sector's number, plus one)
//    * Bytes 12-15: sector number of "tail sector counter" (typically: this sector's number, plus two)
//    * Bytes 16-19: sector number of first data sector (typically: this sector's number, plus three)
//    * Bytes 20-23: byte number of tail counter in data sector (ESP8266: 32)
//    * Bytes 24-27: byte number of first data entry in data sector (ESP8266: 64)
//    * Bytes 28-31: number of data entries in data sector (ESP8266: 252)
//    * Bytes 32-35: number of sectors in each sector counter
//    * Bytes 36-39: number of data sectors
// Note that the header sector does not necessarily need to exist as a physical sector. All that matters
// is that a function flash_fifo_get_header() exists which returns a pointer to a header structure.
// This may be a pointer to a const structure, rather than something that reads a sector from flash.
//
//
// Writing an entry works as follows:
//  1) Obtain current "tail" sector from sector counter
//  2) Obtain current "tail" index in sector from in-sector counter
//  3) If tail_index+1==data_entries_per_sector (i.e. if this entry would complete the sector), then
//     3a) Obtain current "head" sector from sector counter
//     3b) if next(tail_sector)==head_sector   (i.e. the logically next page is still in use), then
//       3b.1) advance head_sector (free up the page, losing the data stored in it)
//     3c) erase sector next(tail_sector)
//  4) write entry to spot tail_index in the current tail_sector.
//  5) mark bit tail_index in the current tail_sector's tail_counter as used (i.e. set to zero)
//  6) If tail_index+1==data_entries_per_sector, then
//     6a) If next(tail_sector)==0 then
//        6a.1) Erase the sectors making up the tail sector counter, else
//        6a.2) Mark bit tail_sector in the tail sector counter as complete.
//
// Reading (without consuming) an entry at offset "offset" works as follows:
//  1) Obtain current head_sector from sector counter
//  2) Obtain current head_index from in-sector counter
//  3) do repeat
//       3a) obtain tail_index from in-sector tail counter of head_sector
//       3b) head_index+=offset, offset=0
//       3c) if (head_index>=data_entries_per_sector)
//          3c.1) if tail_index<data_entries_per_sector then fail
//          3c.2) offset=head_index-data_entries_per_sector
//          3c.3) head_index=0, head_sector=next(head_sector)
//     until offset==0
//  4) if (tail_index<=head_index)
//      4a) fail (no data available)
//  5) return data entry at index head_index from head_sector
//
// Consuming (up to) "count" entries (without reading them)
//
//  Repeat "count" times:
//  1) Obtain current head_sector from sector counter
//  2) Obtain current head_index from in-sector counter
//  3) Obtain tail_index from in-sector tail counter of head_sector
//  4) if (tail_index<=head_index)  finish
//  5) Mark bit "head_index" in head in-sector counter of sector "head_sector" as used
//  6) if (next(head_index)==data_entries_per_sector)
//     6a) If next(head_sector)==0 then
//        6a.1) Erase the sectors making up the head sector counter, else
//        6a.2) Mark bit tail_sector in the head sector counter as complete.
// (Yeah, this could be made more efficient. But that would also introduce a whole lot more
//  corner cases, which is a Bad Idea[tm], at least until we find that we *need* it to be
//  more efficient)

#define FLASH_FIFO_MAGIC         0x64695573

#define INTERNAL // Just for keeping track
#define API      // ditto
#define UNITTEST // ditto

#include "fifo.h" // This gets us the sample_t structure
#include <stdbool.h>

typedef struct
{
  uint32_t magic;
  uint32_t sector_size;
  uint32_t head_counter;
  uint32_t tail_counter;
  uint32_t data;
  uint32_t tail_byte_offset;
  uint32_t data_byte_offset;
  uint32_t data_entries_per_sector;
  uint32_t counter_sectors;
  uint32_t data_sectors;
} flash_fifo_t;

typedef uint32_t data_sector_t; // These are relative to flash_fifo_t->data
typedef struct
{
  data_sector_t sector;
  uint32_t      index;
} flash_fifo_slot_t;


#define ESP8266_SECTOR_SIZE SPI_FLASH_SEC_SIZE
#ifndef ESP8266_FLASH_FIFO_START
#define ESP8266_FLASH_FIFO_START 0xa0000
#endif
#ifndef ESP8266_FLASH_FIFO_SIZE
#define ESP8266_FLASH_FIFO_SIZE  0x40000
#endif

#ifdef UNIT_TEST
#define FAKE_FLASH
typedef enum {
  SPI_FLASH_RESULT_OK=0,
}SpiFlashOpResult;
#define SPI_FLASH_SEC_SIZE 4096
static SpiFlashOpResult fake_spi_flash_erase_sector(uint16_t sector);
static SpiFlashOpResult fake_spi_flash_write(uint32_t addr, const uint32_t* data, uint32_t len);
static SpiFlashOpResult fake_spi_flash_read(uint32_t addr, uint32_t* data, uint32_t len);
#define spi_flash_erase_sector fake_spi_flash_erase_sector
#define spi_flash_write        fake_spi_flash_write
#define spi_flash_read         fake_spi_flash_read
#else
#include <spi_flash.h>
#endif

#ifdef BOOTLOADER_CODE
// We can't use the full spi_flash_* routines, because they pull in too much of the SDK.
#define spi_flash_erase_sector bootloader_spi_flash_erase_sector
#define spi_flash_write        bootloader_spi_flash_write
#define spi_flash_read         bootloader_spi_flash_read
extern int booted_flash_page;

static SpiFlashOpResult IN_RAM_ATTR bootloader_spi_flash_erase_sector(uint16_t sector)
{
  ets_intr_lock();
  Cache_Read_Disable();
  SpiFlashOpResult res=SPIEraseSector(sector);
  Cache_Read_Enable(booted_flash_page,0,0);
  ets_intr_unlock();
  return res;
}

static SpiFlashOpResult IN_RAM_ATTR bootloader_spi_flash_write(uint32_t addr, const uint32_t* data, uint32_t len)
{
  ets_intr_lock();
  Cache_Read_Disable();
  SpiFlashOpResult res=SPIWrite(addr,data,len);
  Cache_Read_Enable(booted_flash_page,0,0);
  ets_intr_unlock();
  return res;
}

static SpiFlashOpResult IN_RAM_ATTR bootloader_spi_flash_read(uint32_t addr, uint32_t* data, uint32_t len)
{
  ets_intr_lock();
  Cache_Read_Disable();
  SpiFlashOpResult res=SPIRead(addr,data,len);
  Cache_Read_Enable(booted_flash_page,0,0);
  ets_intr_unlock();
  return res;
}
#endif

INTERNAL static inline const flash_fifo_t* flash_fifo_get_header(void)
{
  static const flash_fifo_t esp8266_fifo={
    .magic=FLASH_FIFO_MAGIC,
    .sector_size=ESP8266_SECTOR_SIZE,
    .head_counter=(ESP8266_FLASH_FIFO_START/ESP8266_SECTOR_SIZE),
    .tail_counter=(ESP8266_FLASH_FIFO_START/ESP8266_SECTOR_SIZE)+1,
    .data=(ESP8266_FLASH_FIFO_START/ESP8266_SECTOR_SIZE)+2,
    .tail_byte_offset=32,
    .data_byte_offset=64,
    .data_entries_per_sector=(ESP8266_SECTOR_SIZE-64)/sizeof(sample_t),
    .counter_sectors=1,
    .data_sectors=(ESP8266_FLASH_FIFO_SIZE/ESP8266_SECTOR_SIZE)-2
  };
  return &esp8266_fifo;
}

INTERNAL static inline bool flash_fifo_valid_header(const flash_fifo_t* fifo)
{
  if (fifo->magic!=FLASH_FIFO_MAGIC)
    return false;
  // Any other consistency/sanity checks we should do here?
  return true;
}

#ifdef BOOTLOADER_CODE
// We can't use the system function, because we don't have the whole system available
// On the other hand, we don't have to worry about the system's software watchdog, either,
// just the hardware one. So let's do it the bare-metal way...
static void flash_fifo_tickle_watchdog(void)
{
  WRITE_PERI_REG(0x60000914, 0x73);
}
#else
extern void system_soft_wdt_feed();
static void flash_fifo_tickle_watchdog(void)
{
  system_soft_wdt_feed();
}
#endif

// All these functions return TRUE on success, FALSE on failure
INTERNAL static inline bool flash_fifo_erase_sectors(uint32_t first, uint32_t count)
{
  while (count--)
  {
    flash_fifo_tickle_watchdog();
    if (spi_flash_erase_sector(first)!=SPI_FLASH_RESULT_OK)
      return false;
    first++;
  }
  return true;
}

INTERNAL static inline bool flash_fifo_reset_head_sector_counter(const flash_fifo_t* fifo)
{
  return flash_fifo_erase_sectors(fifo->head_counter,fifo->counter_sectors);
}

INTERNAL static inline bool flash_fifo_reset_tail_sector_counter(const flash_fifo_t* fifo)
{
  return flash_fifo_erase_sectors(fifo->tail_counter,fifo->counter_sectors);
}

INTERNAL static inline bool flash_fifo_erase_data_sector(const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_erase_sectors(fifo->data+sector,1);
}

INTERNAL static inline bool flash_fifo_erase_all_data_sectors(const flash_fifo_t* fifo)
{
  return flash_fifo_erase_sectors(fifo->data,fifo->data_sectors);
}

INTERNAL static inline bool flash_fifo_clear_content(const flash_fifo_t* fifo)
{
  return flash_fifo_reset_head_sector_counter(fifo) &&
    flash_fifo_reset_tail_sector_counter(fifo) &&
    flash_fifo_erase_all_data_sectors(fifo);
}

#define FLASH_FIFO_LONGS_PER_READ 8
INTERNAL static inline bool flash_fifo_get_counter(uint32_t* result, const flash_fifo_t* fifo, uint32_t sector, uint32_t offset)
{
  uint32_t addr=sector*fifo->sector_size+offset;
  uint32_t response=0;
  while (1)
  {
    uint32_t buffer[FLASH_FIFO_LONGS_PER_READ];

    if (spi_flash_read(addr,(void*)buffer,sizeof(buffer))!=SPI_FLASH_RESULT_OK)
      return false;
    uint32_t ind=0;
    while (ind<FLASH_FIFO_LONGS_PER_READ && buffer[ind]==0)
    {
      ind++;
      response+=32;
    }
    if (ind<FLASH_FIFO_LONGS_PER_READ)
    { // Found a 1 bit.
      uint32_t v=buffer[ind];
      if ((v&0xffff)==0)
      {
        v>>=16;
        response+=16;
      }
      if ((v&0xff)==0)
      {
        v>>=8;
        response+=8;
      }
      if ((v&0x0f)==0)
      {
        v>>=4;
        response+=4;
      }
      if ((v&0x03)==0)
      {
        v>>=2;
        response+=2;
      }
      if ((v&0x01)==0)
      {
        v>>=1;
        response+=1;
      }
      *result=response;
      return true;
    }
    addr+=sizeof(uint32_t)*FLASH_FIFO_LONGS_PER_READ;
  }
}

INTERNAL static inline bool flash_fifo_mark_counter(uint32_t value, const flash_fifo_t* fifo, uint32_t sector, uint32_t offset)
{
  uint32_t addr=sector*fifo->sector_size+offset+(value/32)*sizeof(uint32_t);
  uint32_t mask=~(1<<(value&31));

  return spi_flash_write(addr,(void*)&mask,sizeof(mask))==SPI_FLASH_RESULT_OK;
}

INTERNAL static inline bool flash_fifo_mark_head_index(uint32_t value, const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_mark_counter(value,fifo,fifo->data+sector,0);
}

INTERNAL static inline bool flash_fifo_mark_tail_index(uint32_t value, const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_mark_counter(value,fifo,fifo->data+sector,fifo->tail_byte_offset);
}

INTERNAL static inline bool flash_fifo_mark_head_sector(data_sector_t value, const flash_fifo_t* fifo)
{
  return flash_fifo_mark_counter(value,fifo,fifo->head_counter,0);
}

INTERNAL static inline bool flash_fifo_mark_tail_sector(data_sector_t value, const flash_fifo_t* fifo)
{
  return flash_fifo_mark_counter(value,fifo,fifo->tail_counter,0);
}


INTERNAL static inline bool flash_fifo_get_head_sector(uint32_t* result, const flash_fifo_t* fifo)
{
  return flash_fifo_get_counter(result,fifo,fifo->head_counter,0);
}

INTERNAL static inline bool flash_fifo_get_tail_sector(uint32_t* result, const flash_fifo_t* fifo)
{
  return flash_fifo_get_counter(result,fifo,fifo->tail_counter,0);
}

INTERNAL static inline bool flash_fifo_get_head_index(uint32_t* result, const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_get_counter(result,fifo,fifo->data+sector,0);
}

INTERNAL static inline bool flash_fifo_get_tail_index(uint32_t* result, const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_get_counter(result,fifo,fifo->data+sector,fifo->tail_byte_offset);
}

INTERNAL static inline bool flash_fifo_read_sample(sample_t* result, const flash_fifo_t* fifo, data_sector_t sector, uint32_t index)
{
  uint32_t addr=fifo->sector_size*(fifo->data+sector)+fifo->data_byte_offset+sizeof(sample_t)*index;
  return spi_flash_read(addr,(void*)result,sizeof(sample_t))==SPI_FLASH_RESULT_OK;
}

INTERNAL static inline bool flash_fifo_write_sample(const sample_t* sample, const flash_fifo_t* fifo, data_sector_t sector, uint32_t index)
{
  uint32_t addr=fifo->sector_size*(sector+fifo->data)+fifo->data_byte_offset+sizeof(sample_t)*index;
  return spi_flash_write(addr,(void*)sample,sizeof(sample_t))==SPI_FLASH_RESULT_OK;
}

INTERNAL static inline data_sector_t flash_fifo_next_data_sector(const flash_fifo_t* fifo, data_sector_t sector)
{
  sector++;
  if (sector>=fifo->data_sectors)
    sector=0;
  return sector;
}

INTERNAL static inline bool flash_fifo_advance_head_sector(const flash_fifo_t* fifo, data_sector_t head_sector,
                                                           data_sector_t* result)
{
  data_sector_t next_head_sector=flash_fifo_next_data_sector(fifo,head_sector);
  if (result)
    *result=next_head_sector;
  if (next_head_sector==0)
    return flash_fifo_reset_head_sector_counter(fifo);
  else
    return flash_fifo_mark_head_sector(head_sector,fifo);
}

INTERNAL static inline bool flash_fifo_advance_tail_sector(const flash_fifo_t* fifo, data_sector_t tail_sector,
                                                           data_sector_t* result)
{
  data_sector_t next_tail_sector=flash_fifo_next_data_sector(fifo,tail_sector);
  if (result)
    *result=next_tail_sector;
  if (next_tail_sector==0)
    return flash_fifo_reset_tail_sector_counter(fifo);
  else
    return flash_fifo_mark_tail_sector(tail_sector,fifo);
}

INTERNAL static inline bool flash_fifo_get_head(flash_fifo_slot_t* result, const flash_fifo_t* fifo)
{
  if (flash_fifo_get_head_sector(&result->sector,fifo)==false ||
      flash_fifo_get_head_index(&result->index,fifo,result->sector)==false)
    return false;
  if (result->index>=fifo->data_entries_per_sector)
  {
    if (flash_fifo_advance_head_sector(fifo,result->sector,&result->sector)==false)
      return false;
    result->index=0;
  }
  return true;
}

INTERNAL static inline bool flash_fifo_get_tail(flash_fifo_slot_t* result, const flash_fifo_t* fifo)
{
  if (flash_fifo_get_tail_sector(&result->sector,fifo)==false ||
      flash_fifo_get_tail_index(&result->index,fifo,result->sector)==false)
    return false;
  if (result->index>=fifo->data_entries_per_sector)
  {
    data_sector_t next_tail_sector=flash_fifo_next_data_sector(fifo,result->sector);
    data_sector_t head_sector;
    if (flash_fifo_get_head_sector(&head_sector,fifo)==false)
      return false;
    if (next_tail_sector==head_sector)
    {
      if (flash_fifo_advance_head_sector(fifo,head_sector,NULL)==false)
        return false;
    }
    if (flash_fifo_erase_data_sector(fifo,next_tail_sector)==false)
      return false;
    if (flash_fifo_advance_tail_sector(fifo,result->sector,&result->sector)==false)
      return false;
    result->index=0;
  }
  return true;
}

INTERNAL static inline uint32_t flash_fifo_count(void)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return 0;

  flash_fifo_slot_t head,tail;
  uint32_t      eps=fifo->data_entries_per_sector;

  if (flash_fifo_get_tail(&tail,fifo)==false ||
      flash_fifo_get_head(&head,fifo)==false)
    return 0;
  uint32_t head_pos=head.sector*eps+head.index;
  uint32_t tail_pos=tail.sector*eps+tail.index;

  if (tail_pos>=head_pos)
    return tail_pos-head_pos;
  uint32_t total_entries=fifo->data_sectors*eps;
  return tail_pos+total_entries-head_pos;
}


INTERNAL static inline bool flash_fifo_drop_one_sample(const flash_fifo_t* fifo)
{
  flash_fifo_slot_t head;
  uint32_t      tail_index;
  uint32_t      eps=fifo->data_entries_per_sector;

  if (flash_fifo_get_head(&head,fifo)==false ||
      flash_fifo_get_tail_index(&tail_index,fifo,head.sector)==false)
    return false;
  if (tail_index<=head.index)
    return false;
  if (!flash_fifo_mark_head_index(head.index,fifo,head.sector))
    return false;
  return true;
}

INTERNAL static inline bool flash_fifo_init()
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (flash_fifo_valid_header(fifo))
    return flash_fifo_clear_content(fifo);
  return false;
}

API static inline uint32_t flash_fifo_get_count(void)
{
  return flash_fifo_count();
}

API static inline uint32_t flash_fifo_get_maxval(void)
{
  return 0xffffffff;
}

API static inline uint32_t flash_fifo_get_size(void)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return 0;

  uint32_t eps=fifo->data_entries_per_sector;
  uint32_t total_entries=fifo->data_sectors*eps;
  // The maximum we can hold at any one time is total_entries-1.
  // However, when we *do* need to discard old data to make room,
  // we discard down to total_entries-eps. So as a promise of "it
  // can hold this much", we should return that smaller number
  return total_entries-eps;
}

API static inline uint32_t flash_fifo_get_max_size(void)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return 0;

  uint32_t eps=fifo->data_entries_per_sector;
  uint32_t total_entries=fifo->data_sectors*eps;
  // The maximum we can hold at any one time is total_entries-1.
  // However, when we *do* need to discard old data to make room,
  // we discard down to total_entries-eps. So as a promise of "it
  // can never hold more than this much", we should return the larger number
  return total_entries-1;
}


API static inline bool flash_fifo_peek_sample(sample_t* dst, uint32_t from_top)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return false;

  flash_fifo_slot_t head,tail;
  uint32_t          eps=fifo->data_entries_per_sector;

  if (flash_fifo_get_tail(&tail,fifo)==false ||
      flash_fifo_get_head(&head,fifo)==false)
    return false;
  do
  {
    head.index+=from_top;
    from_top=0;
    if (head.sector==tail.sector && head.index>=tail.index) // Gone over the end
      return false;
    if (head.index>=eps)
    {
      from_top=head.index-eps;
      head.index=0;
      head.sector=flash_fifo_next_data_sector(fifo,head.sector);
      continue; // ensure check for overrun even if from_top==0
    }
    break;
  } while (true);
  return flash_fifo_read_sample(dst,fifo,head.sector,head.index);
}

API static inline bool flash_fifo_drop_samples(uint32_t from_top)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return false;

  while (from_top--)
  {
    if (!flash_fifo_drop_one_sample(fifo))
      return false; // Uh-oh...
  }
  return true;
}

API static inline bool flash_fifo_pop_sample(sample_t* dst)
{
  if (flash_fifo_peek_sample(dst,0))
    return flash_fifo_drop_samples(1);
  return false;
}

API static inline bool flash_fifo_store_sample(const sample_t* s)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return false;

  flash_fifo_slot_t tail;
  uint32_t      eps=fifo->data_entries_per_sector;

  if (flash_fifo_get_tail(&tail,fifo)==false)
    return false;
  if (flash_fifo_write_sample(s,fifo,tail.sector,tail.index)==false)
    return false;

  flash_fifo_mark_tail_index(tail.index,fifo,tail.sector);
  return true;
}

API static inline bool flash_fifo_check_magic(void)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return false;
  return true;
}

API static inline bool flash_fifo_prepare(uint32_t tagcount)
{
  return flash_fifo_init();
}

#ifdef FAKE_FLASH
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FLASH_MAX (ESP8266_FLASH_FIFO_START+ESP8266_FLASH_FIFO_SIZE)
static uint8_t flash[FLASH_MAX];

void die(void)
{
  FILE* f=fopen("flash_at_death.bin","w");
  if (f)
  {
    for (int i=0;i<FLASH_MAX;i++)
      fputc(flash[i],f);
    fclose(f);
  }
  abort();
}

UNITTEST SpiFlashOpResult fake_spi_flash_erase_sector(uint16_t sector)
{
  uint32_t addr=SPI_FLASH_SEC_SIZE*(uint32_t)sector;
  if (addr>=FLASH_MAX || addr+SPI_FLASH_SEC_SIZE>FLASH_MAX)
    die();

  memset(flash+addr,0xff,SPI_FLASH_SEC_SIZE);
  return SPI_FLASH_RESULT_OK;
}

UNITTEST SpiFlashOpResult fake_spi_flash_write(uint32_t addr, const uint32_t* data, uint32_t len)
{
  if (addr>=FLASH_MAX || addr+len>FLASH_MAX)
    die();
  if ((addr&3)!=0 || (len&3)!=0)
    die();

  const uint8_t* bdata=(const uint8_t*)data;
  uint32_t i;

  for (i=0;i<len;i++)
    flash[addr+i]&=bdata[i];
  return SPI_FLASH_RESULT_OK;
}

UNITTEST SpiFlashOpResult fake_spi_flash_read(uint32_t addr, uint32_t* data, uint32_t len)
{
  if (addr>=FLASH_MAX || addr+len>FLASH_MAX)
    die();
  if ((addr&3)!=0 || (len&3)!=0)
    die();
  uint8_t* bdata=(uint8_t*)data;
  uint32_t i;

  for (i=0;i<len;i++)
    bdata[i]=flash[addr+i];
  return SPI_FLASH_RESULT_OK;
}
#endif

#endif
