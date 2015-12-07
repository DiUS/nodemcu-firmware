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
 * @author Johny Mattsson <jmattsson@dius.com.au>
 */
#include "lauxlib.h"
#include "platform.h"
#include "user_modules.h"
#include "flash_api.h"

/* Ensure we don't override Cache_Read_Enable() unintentionally, as that would
 * be Rather Bad(tm) */
#ifdef LUA_USE_MODULES_OTAUPGRADE

#if !defined(SPIFFS_FIXED_LOCATION)
# error "SPIFFS location must be explicitly set when using OTA upgrade!"
#endif

/* ---------- Supporting internals --------------------------------------- */

#include "otaupgrade.h"

#define OTA_IRAM_CODE_SECTION __attribute__((section(".ota.iram0.text")))
#define OTA_IRAM_DATA_SECTION __attribute__((section(".ota.iram0.data")))

extern void ROM_Cache_Read_Enable (uint32_t b0, uint32_t b1, uint32_t full32k);

/* To keep track of which firmware slot is the currently booted one, we keep
 * a single uint32_t in memory. Due to the point at which the slot needs to
 * be saved (before .bss and .data init has been done), it cannot reside in
 * either of those sections. We therefore stash it away into IRAM, next to
 * the overridden Cache_Read_Enable() function. This allows said function
 * to access it as soon as the function itself is loaded, and avoids
 * contention with regular .bss and .data operation.
 */
static uint32_t saved_slot OTA_IRAM_DATA_SECTION;

void OTA_IRAM_CODE_SECTION Cache_Read_Enable (uint32_t b0, uint32_t b1, uint32_t full32k)
{
  (void)b0; (void)b1; /* "We know better, trust us!" */
  ROM_Cache_Read_Enable (saved_slot, 0, full32k);
}

void OTA_IRAM_CODE_SECTION otaupgrade_save_booted_slot (uint8_t which)
{
  if (which > 1)
    which = 0;

  saved_slot = which;
}


/* ----------otaupgrade Lua API ------------------------------------------ */

#define FW_OFFS    0x1000  /* The first 4k in each slot is reserved */
#define MEGABYTE 0x100000  /* Each slot is a meg in size */

#define OTA_HDR_MAGIC 0x4d4a

#define BOOT_STATUS_INVALID   0x10 /* No valid image in this slot */
#define BOOT_STATUS_PREFERRED 0x20 /* If two images available, use this one */
#define BOOT_STATUS_IN_TEST   0x40 /* Boot possible, provided bootbits free */
#define NUM_SECTIONS_MASK     0x0f

typedef struct
{
  uint16_t magic;
  uint8_t  boot_bits;
  uint8_t  flags_num_sections;
  uint32_t entry;
} ota_header;

typedef enum { IN_TEST_DISALLOWED, IN_TEST_ALLOWED } intest_policy_t;

static bool ota_flashing_in_progress;

static ota_header get_ota_header (lua_State *L, uint8_t slot, bool raise_error)
{
  uint32_t addr = (slot * MEGABYTE + FW_OFFS);
  ota_header hdr;
  platform_flash_read (&hdr, addr, sizeof (hdr));
  if (raise_error && hdr.magic != OTA_HDR_MAGIC)
    luaL_error (L, "slot %c firmware is not an OTA image", 'A' + slot);
  return hdr;
}


static void ensure_possible (lua_State *L, intest_policy_t policy)
{
  if (flash_detect_size_byte () < 2*MEGABYTE)
    luaL_error (L, "unsupported, flash chip is too small");

  ota_header hdr = get_ota_header (L, saved_slot, true);

  if ((hdr.flags_num_sections & BOOT_STATUS_IN_TEST) &&
      (policy == IN_TEST_DISALLOWED))
    luaL_error (L, "not permitted while testing firmware");
}


static inline uint32_t get_flash_write_addr_for_offset (uint8_t slot, uint32_t offs)
{
  return slot * MEGABYTE +  FW_OFFS + offs;
}


static void ota_flash_write (lua_State *L, const void *src, uint32_t dst_addr, uint32_t len)
{
  if (!platform_flash_write (src, dst_addr, len))
    luaL_error (L, "flash write error");
}


static inline void optional_reboot (lua_State *L)
{
  if (luaL_optint (L, 1, 0))
    system_restart ();
}


/* Lua: otaupgrade.commence() -- wipes the inactive slot and enables .write() */
static int lotaupgrade_commence (lua_State* L)
{
  ensure_possible (L, IN_TEST_DISALLOWED);

  uint8_t spare_slot = !saved_slot;
  uint32_t page_addr = FW_OFFS + (spare_slot * MEGABYTE);
  const uint32_t end_addr = (spare_slot + 1) * MEGABYTE;
  for (; page_addr < end_addr; page_addr += SPI_FLASH_SEC_SIZE)
  {
    system_soft_wdt_feed ();
    platform_flash_erase_sector (page_addr / SPI_FLASH_SEC_SIZE);
  }
  ota_flashing_in_progress = true;
  return 0;
}


/* Lua: otaupgrade.write(offset, data) -- writes the data block to flash */
static int lotaupgrade_write (lua_State *L)
{
  if (!ota_flashing_in_progress)
    return luaL_error(L, "write not possible, use otaupgrade.commence() first");
  ensure_possible (L, IN_TEST_DISALLOWED);

  uint32_t dst_addr = get_flash_write_addr_for_offset (!saved_slot, luaL_checkint (L, 1));
  const char *bytes = luaL_checkstring (L, 2);
  size_t len = lua_objlen (L, 2);

  /* Pad to 4-byte align */
  if (len % 4)
  {
    char pad[] = { 0xff, 0xff, 0xff };
    lua_settop (L, 2); /* discard any excess arguments */
    lua_pushlstring (L, pad, 4 - (len % 4));
    lua_concat (L, 2);
    bytes = lua_tolstring (L, 2, &len);
  }

  ota_flash_write (L, bytes, dst_addr, len);

  return 0;
}

/* Lua: otaupgrade.complete(optional_reboot) */
static int lotaupgrade_complete (lua_State *L)
{
  if (!ota_flashing_in_progress)
    return luaL_error(L, "upgrade not in progress");

  ensure_possible (L, IN_TEST_DISALLOWED);

  ota_flashing_in_progress = false;
  /* Clear the 'invalid' bit to mark this image as available to boot */
  ota_header hdr = get_ota_header (L, !saved_slot, true);
  hdr.flags_num_sections &= ~BOOT_STATUS_INVALID;

  uint32_t dst_addr = get_flash_write_addr_for_offset (!saved_slot, 0);
  ota_flash_write (L, &hdr, dst_addr, sizeof (hdr));

  optional_reboot (L);
  return 0;
}

/* Lua: otaupgrade.accept(optional_reboot) */
static int lotaupgrade_accept (lua_State *L)
{
  ensure_possible (L, IN_TEST_ALLOWED);

  ota_header hdr = get_ota_header (L, saved_slot, true);
  if (!(hdr.flags_num_sections & BOOT_STATUS_IN_TEST))
    return 0;

  /* First, clear the preferred bit in the old slot */
  ota_header old_hdr = get_ota_header (L, !saved_slot, true);
  old_hdr.flags_num_sections &= ~BOOT_STATUS_PREFERRED;
  uint32_t dst_addr = get_flash_write_addr_for_offset (!saved_slot, 0);
  ota_flash_write (L, &old_hdr, dst_addr, sizeof (old_hdr));

  /* If we crash/reboot before the next write, we will use up another
   * test boot bit. Que sera, sera. */

  /* Clear the testing bit in the current slot */
  hdr.flags_num_sections &= ~BOOT_STATUS_IN_TEST;

  dst_addr = get_flash_write_addr_for_offset (saved_slot, 0);
  ota_flash_write (L, &hdr, dst_addr, sizeof (hdr));

  optional_reboot (L);
  return 0;
}

/* Lua: otaupgrade.reject(optional_reboot) */
static int lotaupgrade_reject (lua_State *L)
{
  ensure_possible (L, IN_TEST_ALLOWED);

  ota_header hdr = get_ota_header (L, saved_slot, true);
  if (!(hdr.flags_num_sections & BOOT_STATUS_IN_TEST))
    return 0;

  /* Zero the test attempts, to make this image unavailable for booting */
  hdr.boot_bits = 0;

  uint32_t dst_addr = get_flash_write_addr_for_offset (saved_slot, 0);
  ota_flash_write (L, &hdr, dst_addr, sizeof (hdr));

  optional_reboot (L);
  return 0;
}

/* Lua: t = otaupgrade.info ()
 * -- { .running = X,
 *      .X = { preferred = n, in_test = n, attempts_left = only_if_test },
 *      .Y = { preferred = n, in_test = n, attempts_left = only_if_test },
 *    }
 */
static int lotaupgrade_info (lua_State *L)
{
  ensure_possible (L, IN_TEST_ALLOWED);

  lua_createtable (L, 0, 3);

  lua_pushliteral (L, "running");
  char slot[2] = { 'A' + saved_slot, '\0' };
  lua_pushstring (L, slot);
  lua_settable (L, -3);

  uint8_t i;
  for (i = 0; i < 2; ++i)
  {
    ota_header hdr = get_ota_header (L, i, false);
    if (hdr.magic == OTA_HDR_MAGIC)
    {
      slot[0] = 'A' + i;
      lua_pushstring (L, slot);
      lua_createtable (L, 0, 3);

      /* Sigh, no proper stdbool.h, have to ternary to get a proper 0 vs 1 */
      bool preferred = hdr.flags_num_sections & BOOT_STATUS_PREFERRED ? 1 : 0;
      bool in_test = hdr.flags_num_sections & BOOT_STATUS_IN_TEST ? 1 : 0;

      lua_pushliteral (L, "preferred");
      lua_pushnumber (L, preferred);
      lua_settable (L, -3);

      lua_pushliteral (L, "in_test");
      lua_pushnumber (L, in_test);
      lua_settable (L, -3);

      if (in_test)
      {
        int count = 0;
        while (hdr.boot_bits)
        {
          hdr.boot_bits &= hdr.boot_bits - 1;
          ++count;
        }
        lua_pushliteral (L, "attempts_left");
        lua_pushnumber (L, count);
        lua_settable (L, -3);
      }

      lua_settable (L, -3); /* .X = info_table */
    }
  }
  return 1;
}

// Module function map
#define MIN_OPT_LEVEL 2
#include "lrodefs.h"
const LUA_REG_TYPE otaupgrade_map[] =
{
  { LSTRKEY( "commence" ), LFUNCVAL( lotaupgrade_commence ) },
  { LSTRKEY( "write" ),    LFUNCVAL( lotaupgrade_write) },
  { LSTRKEY( "complete" ), LFUNCVAL( lotaupgrade_complete) },
  { LSTRKEY( "accept" ),   LFUNCVAL( lotaupgrade_accept) },
  { LSTRKEY( "reject" ),   LFUNCVAL( lotaupgrade_reject) },
  { LSTRKEY( "info" ),     LFUNCVAL( lotaupgrade_info) },
  { LNILKEY, LNILVAL }
};

LUALIB_API int luaopen_otaupgrade( lua_State *L )
{
  LREGISTER ( L, AUXLIB_OTAUPGRADE, otaupgrade_map );
}
#endif /* LUA_USE_MODULES_OTAUPGRADE */
