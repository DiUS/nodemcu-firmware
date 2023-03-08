/*
 * Copyright 2019 Dius Computing Pty Ltd. All rights reserved.
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
 * @author Johny Mattsson <jmattsson@dius.com.au>
 * @author Bernd Meyer <bmeyer@dius.com.au>
 */

#include "module.h"
#include "lauxlib.h"
#include "lmem.h"
#include "esp_attr.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef enum {
  SYNCING,
  RECV,
  SEND
} state_t;

#define BP_MAGIC   0xa5
#define BP_MAGIC_AES 0xa6
#define BP_MAGIC_AES_WITH_MAC 0xa7
#define BP_PAGESEQ 0xcc // unexpected pageno requested, please resend
#define BP_NOPAGE  0xff

#ifndef BP_PAGESIZE
#define BP_PAGESIZE 4096
#endif


static const uint8_t *fw  = NULL;
static uint32_t fw_len    = 0;
static state_t  state     = SYNCING;
static uint8_t  magic     = BP_MAGIC; // We will choose the actual magic character on SYNC
static uint8_t  exp_page  = 0;
static bool     send_done = false;

RTC_DATA_ATTR uint64_t fw_checksum = 0;

extern const uint8_t nrf_bin[]     asm("_binary_nrf_bin_start");
extern const uint8_t nrf_bin_end[] asm("_binary_nrf_bin_end");

// We might need to find the embedded firmware *outside* this executable
// For that purpose, unique 15 byte markers are embedded before and after,
// which can be detected by simply scanning the binary.
// This module itself has no actual use for them (it can use the symbols
// provided by the linker to find the firmware); It merely needs to
// reference them to make sure they are included in the build
extern const uint8_t nrf_start_marker[]     asm("_binary_nrfboot_start_marker_start");
extern const uint8_t nrf_end_marker[]       asm("_binary_nrfboot_end_marker_start");


#define CRC16_START 0xffff
static uint16_t crc16 (uint16_t crc, const uint8_t *data, uint16_t length)
{
  uint8_t x;
  while (length--)
  {
    uint8_t byte = *data++;
    x = (crc >> 8) ^ byte;
    x ^= x >> 4;
    crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x <<5)) ^ ((uint16_t)x);
  }
  return crc;
}


static void init_fw_checksum()
{
  uint32_t len = nrf_bin_end - nrf_bin;
  const uint8_t *p = nrf_bin;
  uint64_t x = 0;
  while (len--)
  {
    x = ((x<<17) ^ (x>>23)) + *(p++);
  }
  fw_checksum = x;

  // This never happens. It's just a convenient way to mark
  // those symbols as "in use" and thus force the markers to
  // be included in the final output.
  // The volatile makes the compiler assume the (never-done)
  // read has side effects, and cannot be optimised away
  if (nrf_start_marker==nrf_end_marker)
    *((volatile uint8_t*)nrf_end_marker);
}


static bool handleByte(lua_State *L, uint8_t c)
{
  uint8_t pageno;

  if (state == SYNCING)
  {
    if (c != BP_MAGIC)
      return false;

    magic  = BP_MAGIC;
    fw     = nrf_bin;
    fw_len = (nrf_bin_end - nrf_bin);

    if (fw[0]==fw[1] && fw[0]==fw[2] && fw[0]==fw[3])
    {
      magic = fw[0];
      fw += 4;
      fw_len -= 4;
    }

    lua_pushlstring(L, (const char *)&magic, 1); // sync
    state = RECV;
    return true;
  }
  if (state == RECV)
  {
    if (c==BP_MAGIC)
    {
      // acknowledge plus deal with remote restarts
      lua_pushlstring(L, (const char *)&magic, 1);
      return true;
    }

    pageno = c;
    if (pageno == BP_NOPAGE && exp_page == ((fw_len-1)/BP_PAGESIZE + 1)) // remote reports done
    {
      send_done = true;
      state = SYNCING; // We are done!
      return false;
    }

    if (pageno != exp_page && pageno != (exp_page - 1))
    {
      const uint8_t eof[] = { magic, BP_PAGESEQ };
      const size_t len = sizeof(eof) + BP_PAGESIZE + sizeof(uint16_t);
      uint8_t *resp = luaM_malloc(L, len);
      memcpy(resp, eof, sizeof(eof));
      lua_pushlstring(L, (const char *)resp, len);
      luaM_freemem(L, resp, len);
      state = SYNCING;
      return true;
    }

    if ((int)pageno*BP_PAGESIZE >= fw_len)
    {
      struct __attribute__((__packed__))
      {
        uint8_t magic;
        uint8_t index;
        uint64_t fw_csum; // This extra 8 bytes should not cause any harm on the plug ?!?!?!
      } eof = {
        .magic = magic,
        .index = BP_NOPAGE,
        .fw_csum = fw_checksum,
      };
      lua_pushlstring(L, (const char *)&eof, sizeof(eof));
      state = RECV;
      return true;
    }

    exp_page = pageno + 1;
    const uint8_t  hdr[2] = { magic, pageno };
    const uint8_t *page = fw + BP_PAGESIZE*pageno;
    const uint16_t csum = crc16(CRC16_START, page, BP_PAGESIZE);

    const size_t len = sizeof(hdr) + BP_PAGESIZE + sizeof(csum);
    uint8_t *resp = luaM_malloc(L, len);
    memcpy(resp, hdr, sizeof(hdr));
    memcpy(resp + sizeof(hdr), page, BP_PAGESIZE);
    memcpy(resp + sizeof(hdr) + BP_PAGESIZE, &csum, sizeof(csum));

    lua_pushlstring(L, (const char *)resp, len);
    luaM_freemem(L, resp, len);
    state = SYNCING;
    return true;
  }
  return false;
}


static int nrfboot_restart(lua_State *L)
{
  fw     = NULL;
  fw_len = 0;
  state  = SYNCING;
  magic  = BP_MAGIC;
  exp_page = 0;
  return 0;
}


static int nrfboot_handlebytes(lua_State *L)
{
  size_t len;
  const uint8_t *bytes = (const uint8_t *)luaL_checklstring(L, -1, &len);
  lua_checkstack(L, len);
  int n = 0;
  for (; len; --len, ++bytes)
    n += handleByte(L, *bytes);
  if (n > 1)
    lua_concat(L, n);
  if (send_done && state == SYNCING) // got to end, didn't restart
  {
    send_done = false;
    if (n == 0)
      lua_pushnil(L);
    lua_pushinteger(L, 1);
    return 2;
  }
  else
    return n > 0 ? 1 : 0;
}


static int nrfboot_get_checksum(lua_State *L)
{
  char buf[17];
  sprintf(buf, "%llx", fw_checksum);
  lua_pushstring(L, buf);
  return 1;
}


static int nrfboot_init(lua_State *L)
{
  if (fw_checksum == 0) {
    init_fw_checksum();
  }
  return 0;
}


LROT_BEGIN(nrfboot, NULL, 0)
  LROT_FUNCENTRY(restart,      nrfboot_restart)
  LROT_FUNCENTRY(handle_bytes, nrfboot_handlebytes)
  LROT_FUNCENTRY(get_checksum, nrfboot_get_checksum)
LROT_END(nrfboot, NULL, 0)

NODEMCU_MODULE(NRFBOOT, "nrfboot", nrfboot, nrfboot_init);
