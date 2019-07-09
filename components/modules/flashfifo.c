/*
 * Copyright 2015-2019 Dius Computing Pty Ltd. All rights reserved.
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
 * @author Johny Mattsson <jmattsson@dius.com.au>
 */
#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <esp_spi_flash.h>
#include <esp_partition.h>

#define CACHE_WORKAROUND
#ifdef CACHE_WORKAROUND
// Workaround for writes/erase not flushing cache; needed until we use an IDF
// with 2752654043fd14cb8f2b759ee9409c6c5942c157 included.
#include <rom/cache.h>
#include <../cache_utils.h>
static inline void flush_cache(void)
{
  spi_flash_op_lock();
  Cache_Flush(0);
# ifndef CONFIG_FREERTOS_UNICORE
  Cache_Flush(1);
# endif
  spi_flash_op_unlock();
}
#endif

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
#define DICT_ENTRY_SIZE          16
#define DICTIONARY_SHIFT         24
#define DURATION_SHIFT            4

#define INTERNAL // Just for keeping track
#define API      // ditto

typedef struct
{
  uint32_t timestamp;
  uint32_t value;
  uint32_t decimals;
  uint32_t tag;
} sample_t;

typedef struct
{
  uint32_t magic;
  uint32_t sector_size;
  uint32_t head_counter;
  uint32_t tail_counter;
  uint32_t dictionary;
  uint32_t data;
  uint32_t tail_byte_offset;
  uint32_t data_byte_offset;
  uint32_t data_entries_per_sector;
  uint32_t counter_sectors;
  uint32_t data_sectors;

  const esp_partition_t *partition;
  const char *mmap;
} flash_fifo_t;

typedef uint32_t data_sector_t; // These are relative to flash_fifo_t->data
typedef struct
{
  data_sector_t sector;
  uint32_t      index;
} flash_fifo_slot_t;


INTERNAL static const flash_fifo_t* flash_fifo_get_header(void)
{
  static flash_fifo_t hdr={
    .magic=FLASH_FIFO_MAGIC,
    .sector_size=SPI_FLASH_SEC_SIZE,
    .head_counter=0,
    .tail_counter=1,
    .dictionary=2,
    .data=3,
    .tail_byte_offset=32,
    .data_byte_offset=64,
    .data_entries_per_sector=(SPI_FLASH_SEC_SIZE-64)/sizeof(sample_t),
    .counter_sectors=1,
    .data_sectors=0,
    .partition=NULL,
    .mmap=NULL,
  };
  // Look up partition info and establish mmap on first access
  if (!hdr.partition)
  {
    hdr.partition = esp_partition_find_first(
      PLATFORM_PARTITION_TYPE_DIUS,
      PLATFORM_PARTITION_SUBTYPE_DIUS_FLASHFIFO,
      NULL);
    hdr.data_sectors = (hdr.partition->size / hdr.sector_size) - hdr.data;
    spi_flash_mmap_handle_t ignored;
    esp_partition_mmap(hdr.partition, 0, hdr.partition->size,
                       SPI_FLASH_MMAP_DATA, (const void **)&hdr.mmap, &ignored);
  }
  return &hdr;
}


INTERNAL static bool flash_fifo_valid_header(const flash_fifo_t* fifo)
{
  if (fifo->magic!=FLASH_FIFO_MAGIC)
    return false;
  // Any other consistency/sanity checks we should do here?
  return true;
}


// All these functions return TRUE on success, FALSE on failure
INTERNAL static bool flash_fifo_erase_sectors(const flash_fifo_t *fifo, uint32_t first, uint32_t count)
{
  esp_err_t err = esp_partition_erase_range(
    fifo->partition, first * fifo->sector_size, count * fifo->sector_size);
#ifdef CACHE_WORKAROUND
  flush_cache();
#endif
  return err == ESP_OK;
}


INTERNAL static bool flash_fifo_reset_head_sector_counter(const flash_fifo_t* fifo)
{
  return flash_fifo_erase_sectors(fifo, fifo->head_counter,fifo->counter_sectors);
}


INTERNAL static bool flash_fifo_reset_tail_sector_counter(const flash_fifo_t* fifo)
{
  return flash_fifo_erase_sectors(fifo, fifo->tail_counter,fifo->counter_sectors);
}


INTERNAL static bool flash_fifo_erase_data_sector(const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_erase_sectors(fifo, fifo->data+sector,1);
}

INTERNAL static bool flash_fifo_erase_dictionary(const flash_fifo_t* fifo)
{
  return flash_fifo_erase_sectors(fifo, fifo->dictionary,1);
}

INTERNAL static uint32_t flash_fifo_get_dictionary_address(const flash_fifo_t* fifo, int index)
{
   return fifo->dictionary*fifo->sector_size+DICT_ENTRY_SIZE*index;
}

INTERNAL static const uint8_t* flash_fifo_get_dictionary_pointer(const flash_fifo_t* fifo, int index)
{
  uint32_t addr=flash_fifo_get_dictionary_address(fifo,index);
  return (const uint8_t *)(fifo->mmap + addr);
}

INTERNAL static bool flash_fifo_dict_entry_matches(const flash_fifo_t* fifo, int index, const uint8_t* buf)
{
  const uint8_t *entry = flash_fifo_get_dictionary_pointer(fifo,index);
  return memcmp(entry,buf,DICT_ENTRY_SIZE)==0;
}

INTERNAL static bool flash_fifo_dict_entry_valid(const flash_fifo_t* fifo, int index)
{
  const uint8_t *entry = flash_fifo_get_dictionary_pointer(fifo,index);
  return entry[DICT_ENTRY_SIZE-1]==0;
}

INTERNAL static bool flash_fifo_write_dict_entry(const flash_fifo_t* fifo, int index, const uint8_t* buf)
{
  uint32_t addr=flash_fifo_get_dictionary_address(fifo,index);
  esp_err_t err = esp_partition_write(fifo->partition, addr, buf, DICT_ENTRY_SIZE);
#ifdef CACHE_WORKAROUND
  flush_cache();
#endif
  return err == ESP_OK;
}


/*
INTERNAL static bool flash_fifo_erase_all_data_sectors(const flash_fifo_t* fifo)
{
  return flash_fifo_erase_sectors(fifo, fifo->data,fifo->data_sectors);
}
*/


INTERNAL static bool flash_fifo_clear_content(const flash_fifo_t* fifo)
{
  return flash_fifo_reset_head_sector_counter(fifo) &&
    flash_fifo_reset_tail_sector_counter(fifo) &&
    flash_fifo_erase_dictionary(fifo) &&
    // flash_fifo_erase_all_data_sectors(fifo);
    flash_fifo_erase_data_sector(fifo,0); // First sector only. All others will be erased as tail reaches them
}


INTERNAL static bool flash_fifo_get_counter(uint32_t* result, const flash_fifo_t* fifo, uint32_t sector, uint32_t offset)
{
  uint32_t addr=sector*fifo->sector_size+offset;
  const uint32_t *sector_end_addr =
    (const uint32_t *)(fifo->mmap + (sector+1)*fifo->sector_size);
  const uint32_t *bits = (const uint32_t *)(&fifo->mmap[addr]);
  uint32_t response = 0;
  for (; bits < sector_end_addr; ++bits)
  {
    uint32_t n = 32 - __builtin_popcount(*bits);
    response += n;
    if (n != 32)
    {
      *result = response;
      return true;
    }
  }
  return false;
}


INTERNAL static bool flash_fifo_mark_counter(uint32_t value, const flash_fifo_t* fifo, uint32_t sector, uint32_t offset)
{
  uint32_t addr=sector*fifo->sector_size+offset+(value/32)*sizeof(uint32_t);
  uint32_t mask=~(1<<(value&31));

  esp_err_t err = esp_partition_write(
    fifo->partition, addr, &mask, sizeof(mask));
#ifdef CACHE_WORKAROUND
  flush_cache();
#endif
  return err == ESP_OK;
}


INTERNAL static bool flash_fifo_mark_head_index(uint32_t value, const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_mark_counter(value,fifo,fifo->data+sector,0);
}


INTERNAL static bool flash_fifo_mark_tail_index(uint32_t value, const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_mark_counter(value,fifo,fifo->data+sector,fifo->tail_byte_offset);
}


INTERNAL static bool flash_fifo_mark_head_sector(data_sector_t value, const flash_fifo_t* fifo)
{
  return flash_fifo_mark_counter(value,fifo,fifo->head_counter,0);
}


INTERNAL static bool flash_fifo_mark_tail_sector(data_sector_t value, const flash_fifo_t* fifo)
{
  return flash_fifo_mark_counter(value,fifo,fifo->tail_counter,0);
}


INTERNAL static bool flash_fifo_get_head_sector(uint32_t* result, const flash_fifo_t* fifo)
{
  return flash_fifo_get_counter(result,fifo,fifo->head_counter,0);
}


INTERNAL static bool flash_fifo_get_tail_sector(uint32_t* result, const flash_fifo_t* fifo)
{
  return flash_fifo_get_counter(result,fifo,fifo->tail_counter,0);
}


INTERNAL static bool flash_fifo_get_head_index(uint32_t* result, const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_get_counter(result,fifo,fifo->data+sector,0);
}


INTERNAL static bool flash_fifo_get_tail_index(uint32_t* result, const flash_fifo_t* fifo, data_sector_t sector)
{
  return flash_fifo_get_counter(result,fifo,fifo->data+sector,fifo->tail_byte_offset);
}


INTERNAL static bool flash_fifo_read_sample(sample_t* result, const flash_fifo_t* fifo, data_sector_t sector, uint32_t index)
{
  uint32_t addr=fifo->sector_size*(fifo->data+sector)+fifo->data_byte_offset+sizeof(sample_t)*index;
  memcpy(result, fifo->mmap + addr, sizeof(sample_t));
  return true;
}


INTERNAL static bool flash_fifo_write_sample(const sample_t* sample, const flash_fifo_t* fifo, data_sector_t sector, uint32_t index)
{
  uint32_t addr=fifo->sector_size*(sector+fifo->data)+fifo->data_byte_offset+sizeof(sample_t)*index;
  esp_err_t err = esp_partition_write(
    fifo->partition, addr, sample, sizeof(*sample));

#ifdef CACHE_WORKAROUND
  flush_cache();
#endif
  return err == ESP_OK;
}


INTERNAL static data_sector_t flash_fifo_next_data_sector(const flash_fifo_t* fifo, data_sector_t sector)
{
  sector++;
  if (sector>=fifo->data_sectors)
    sector=0;
  return sector;
}


INTERNAL static bool flash_fifo_advance_head_sector(const flash_fifo_t* fifo, data_sector_t head_sector,
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


INTERNAL static bool flash_fifo_advance_tail_sector(const flash_fifo_t* fifo, data_sector_t tail_sector,
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


INTERNAL static bool flash_fifo_get_head(flash_fifo_slot_t* result, const flash_fifo_t* fifo)
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

INTERNAL static bool flash_fifo_get_tail(flash_fifo_slot_t* result, const flash_fifo_t* fifo)
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


INTERNAL static uint32_t flash_fifo_count(void)
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


INTERNAL static bool flash_fifo_drop_one_sample(const flash_fifo_t* fifo)
{
  flash_fifo_slot_t head;
  uint32_t      tail_index;

  if (flash_fifo_get_head(&head,fifo)==false ||
      flash_fifo_get_tail_index(&tail_index,fifo,head.sector)==false)
    return false;
  if (tail_index<=head.index)
    return false;
  if (!flash_fifo_mark_head_index(head.index,fifo,head.sector))
    return false;
  return true;
}


INTERNAL static bool flash_fifo_init()
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (flash_fifo_valid_header(fifo))
    return flash_fifo_clear_content(fifo);
  return false;
}


API static uint32_t flash_fifo_get_count(void)
{
  return flash_fifo_count();
}


API static uint32_t flash_fifo_get_maxval(void)
{
  return 0xffffffff;
}


API static uint32_t flash_fifo_get_size(void)
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


API static uint32_t flash_fifo_get_max_size(void)
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

API static const char* flash_fifo_get_dictionary_by_index(int index)
{
 const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return NULL;
  return (const char*)flash_fifo_get_dictionary_pointer(fifo,index);
}


API static bool flash_fifo_peek_sample(sample_t* dst, uint32_t from_top)
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


API static bool flash_fifo_drop_samples(uint32_t from_top)
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


API static bool flash_fifo_pop_sample(sample_t* dst)
{
  if (flash_fifo_peek_sample(dst,0))
    return flash_fifo_drop_samples(1);
  return false;
}

API static int get_dictionary_index(const flash_fifo_t* fifo, const char* name)
{
  size_t l=strlen(name);
  if (l>DICT_ENTRY_SIZE-1)
    return -1;
  uint8_t buf[DICT_ENTRY_SIZE]={0,};
  memcpy(buf,name,l);

  while (1)
  {
    for (int i=0;i<fifo->sector_size/DICT_ENTRY_SIZE;i++)
    {
      if (flash_fifo_dict_entry_valid(fifo,i))
      {
        if (flash_fifo_dict_entry_matches(fifo,i,buf))
          return i;
      }
      else
      {
        if (flash_fifo_write_dict_entry(fifo,i,buf))
          return i;
        else
          return -1;
      }
    }
    // Last resort. The dictionary is full....
    flash_fifo_clear_content(fifo);
  }
}

API static bool flash_fifo_store_sample(const sample_t* s, const char* mac)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return false;

  int mac_dict=get_dictionary_index(fifo,mac);
  if (mac_dict<0)
    return false;
  sample_t ls=*s;
  ls.decimals|=(mac_dict<<DICTIONARY_SHIFT);

  flash_fifo_slot_t tail;
  if (flash_fifo_get_tail(&tail,fifo)==false)
    return false;
  if (flash_fifo_write_sample(&ls,fifo,tail.sector,tail.index)==false)
    return false;

  flash_fifo_mark_tail_index(tail.index,fifo,tail.sector);
  return true;
}


API static bool flash_fifo_check_magic(void)
{
  const flash_fifo_t* fifo=flash_fifo_get_header();
  if (!flash_fifo_valid_header(fifo))
    return false;
  return true;
}


API static bool flash_fifo_prepare(uint32_t tagcount)
{
  return flash_fifo_init();
}


// --- Lua interface -------------------------------------------------------

// flashfifo.prepare ()
static int flashfifo_prepare (lua_State *L)
{
  flash_fifo_prepare (0); // dummy "tagcount" argument

  return 0;
}


// ready = flashfifo.ready ()
static int flashfifo_ready (lua_State *L)
{
  lua_pushnumber (L, flash_fifo_check_magic ());
  return 1;
}

static void check_fifo_magic (lua_State *L)
{
  if (!flash_fifo_check_magic ())
    luaL_error (L, "flashfifo not prepared!");
}


// flashfifo.put (timestamp, value, decimals, sensor_name)
static int flashfifo_put (lua_State *L)
{
  check_fifo_magic (L);

  sample_t s;
  s.timestamp = luaL_checknumber (L, 1);
  s.value = luaL_checknumber (L, 2);
  unsigned int decimals = luaL_checknumber (L, 3);
  unsigned int duration = 0;
  const char* mac="local";
  size_t maclen=strlen(mac);

  size_t len;
  const char *str = luaL_checklstring (L, 4, &len);

  if (!lua_isnoneornil (L, 5))
    duration=luaL_checknumber (L, 5);
  if (!lua_isnoneornil (L, 6))
    mac=luaL_checklstring (L, 6, &maclen);

  union {
    uint32_t u;
    char s[4];
  } conv = { 0 };
  strncpy (conv.s, str, len > 4 ? 4 : len);
  s.tag = conv.u;

  if (decimals>=(1<<DURATION_SHIFT))
    luaL_error (L, "Decimals too large!");
  if (duration>=(1<<(DICTIONARY_SHIFT-DURATION_SHIFT)))
    luaL_error (L, "Duration too large!");

  s.decimals=decimals|(duration<<4);
  flash_fifo_store_sample (&s,mac);
  return 0;
}


static int extract_sample (lua_State *L, const sample_t *s)
{
  lua_pushnumber (L, s->timestamp);
  lua_pushnumber (L, (int32_t)s->value);
  lua_pushnumber (L, s->decimals&((1<<DURATION_SHIFT)-1));
  union {
    uint32_t u;
    char s[4];
  } conv = { s->tag };
  if (conv.s[3] == 0)
    lua_pushstring (L, conv.s);
  else
    lua_pushlstring (L, conv.s, 4);
  lua_pushnumber (L, (s->decimals>>DURATION_SHIFT)&((1<<(DICTIONARY_SHIFT-DURATION_SHIFT))-1));
  const char* dict_entry=flash_fifo_get_dictionary_by_index(s->decimals>>DICTIONARY_SHIFT);
  lua_pushstring(L,dict_entry);

  return 6;
}


// timestamp, value, decimals, sensor_name = flashfifo.pop ()
static int flashfifo_pop (lua_State *L)
{
  check_fifo_magic (L);

  sample_t s;
  if (!flash_fifo_pop_sample (&s))
    return 0;
  else
    return extract_sample (L, &s);
}


// timestamp, value, decimals, sensor_name = flashfifo.peek ([offset])
static int flashfifo_peek (lua_State *L)
{
  check_fifo_magic (L);

  sample_t s;
  uint32_t offs = 0;
  if (lua_isnumber (L, 1))
    offs = lua_tonumber (L, 1);
  if (!flash_fifo_peek_sample (&s, offs))
    return 0;
  else
    return extract_sample (L, &s);
}


// flashfifo.drop (num)
static int flashfifo_drop (lua_State *L)
{
  check_fifo_magic (L);

  flash_fifo_drop_samples (luaL_checknumber (L, 1));
  return 0;
}


// num = flashfifo.count ()
static int flashfifo_count (lua_State *L)
{
  check_fifo_magic (L);

  lua_pushnumber (L, flash_fifo_get_count ());
  return 1;
}


// The "size" of a fifo cannot necessarily be described by a single number. On overflow, more than one
// old sample may be lost....

// num = flashfifo.size () --- provides guaranteed capacity; Data *may* be lost if more entries
static int flashfifo_size (lua_State *L)
{
  check_fifo_magic (L);

  lua_pushnumber (L, flash_fifo_get_size ());
  return 1;
}


// num = flashfifo.maxsize () --- provides maximum capacity; Data *will* be lost if more entries
static int flashfifo_maxsize (lua_State *L)
{
  check_fifo_magic (L);

  lua_pushnumber (L, flash_fifo_get_max_size ());
  return 1;
}


static int flashfifo_maxval(lua_State *L)
{
  check_fifo_magic (L);

  lua_pushnumber (L, flash_fifo_get_maxval ());
  return 1;
}


LROT_BEGIN(flashfifo)
  LROT_FUNCENTRY(prepare, flashfifo_prepare)
  LROT_FUNCENTRY(ready,   flashfifo_ready)
  LROT_FUNCENTRY(put,     flashfifo_put)
  LROT_FUNCENTRY(pop,     flashfifo_pop)
  LROT_FUNCENTRY(peek,    flashfifo_peek)
  LROT_FUNCENTRY(drop,    flashfifo_drop)
  LROT_FUNCENTRY(count,   flashfifo_count)
  LROT_FUNCENTRY(size,    flashfifo_size)
  LROT_FUNCENTRY(maxsize, flashfifo_maxsize)
  LROT_FUNCENTRY(maxval,  flashfifo_maxval)
LROT_END(flashfifo, NULL, 0)

NODEMCU_MODULE(FLASHFIFO, "flashfifo", flashfifo, NULL);
