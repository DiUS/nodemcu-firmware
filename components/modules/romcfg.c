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
 */
#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include <esp_partition.h>

static const esp_partition_t *find_partition(void)
{
  return esp_partition_find_first(
      PLATFORM_PARTITION_TYPE_DIUS,
      PLATFORM_PARTITION_SUBTYPE_DIUS_ROMCFG,
      NULL);
}

static int romcfg_erase(lua_State *L)
{
  const esp_partition_t *part = find_partition();
  if (!part)
    return luaL_error(L, "no romcfg partition");

  esp_err_t err = esp_partition_erase_range(part,0,SPI_FLASH_SEC_SIZE);
  if (err!=ESP_OK)
    return luaL_error(L, "error erasing romcfg");
  return 0;
}

// romcfg.write(s [,offset])
static int romcfg_write(lua_State *L)
{
  size_t offset=0;
  size_t data_len;
  const char *data = luaL_checklstring(L, 1, &data_len);
  if (!lua_isnoneornil(L, 2))
    offset = luaL_checkinteger(L, 2);

  if (data_len+offset>SPI_FLASH_SEC_SIZE)
    return luaL_error(L, "romcfg write beyond partition end");
  if ((offset&3) || (data_len&3))
    return luaL_error(L, "romcfg write start/size must be 32 bit aligned");

  const esp_partition_t *part = find_partition();
  if (!part)
    return luaL_error(L, "no romcfg partition");

  esp_err_t err = esp_partition_write(part,offset,data,data_len);
  if (err!=ESP_OK)
    return luaL_error(L, "error writing romcfg");
  return 0;
}

static uint32_t map_integer_to_storage(uint32_t x)
{
  if (x==0x80000000)
    return 0x80000001; // Ever-so-slightly smaller negative number
  if (x==0xffffffff)
    return 0x80000000;
  return x;
}

static uint32_t map_storage_to_integer(uint32_t x)
{
  if (x==0x80000000)
    return 0xffffffff;
  return x;
}

// romcfg.write_integer(i ,offset)
static int romcfg_write_integer(lua_State *L)
{
  int32_t sdata = luaL_checkinteger(L, 1);
  uint32_t data=map_integer_to_storage((uint32_t)sdata);

  size_t offset = luaL_checkinteger(L, 2);
  size_t data_len=4;

  if (data_len+offset>SPI_FLASH_SEC_SIZE)
    return luaL_error(L, "romcfg write beyond partition end");
  if ((offset&3) || (data_len&3))
    return luaL_error(L, "romcfg write start/size must be 32 bit aligned");

  const esp_partition_t *part = find_partition();
  if (!part)
    return luaL_error(L, "no romcfg partition");

  uint32_t current_data;
  esp_err_t err = esp_partition_read(part,offset,&current_data,data_len);
  if (err!=ESP_OK)
    return luaL_error(L, "error reading romcfg");
  if (current_data==data)
  {
    lua_pushboolean( L, true );
    return 1;
  }
  if ((current_data&data)!=data)
  {
    lua_pushboolean( L, false );
    return 1;
  }
  err = esp_partition_write(part,offset,&data,data_len);
  if (err!=ESP_OK)
    return luaL_error(L, "error writing romcfg");
  lua_pushboolean( L, true );
  return 1;
}

// romcfg.read_integer(offset)
static int romcfg_read_integer(lua_State *L)
{
  size_t offset = luaL_checkinteger(L, 1);
  size_t data_len=4;

  if (data_len+offset>SPI_FLASH_SEC_SIZE)
    return luaL_error(L, "romcfg read beyond partition end");
  if ((offset&3) || (data_len&3))
    return luaL_error(L, "romcfg read start/size must be 32 bit aligned");

  const esp_partition_t *part = find_partition();
  if (!part)
    return luaL_error(L, "no romcfg partition");

  uint32_t current_data;
  esp_err_t err = esp_partition_read(part,offset,&current_data,data_len);
  if (err!=ESP_OK)
    return luaL_error(L, "error reading romcfg");
  lua_pushinteger(L, (int32_t)map_storage_to_integer(current_data));
  return 1;
}


// romcfg.get([offset [,len]])
static int romcfg_get_generic(lua_State *L, bool raw)
{
  size_t offset=0;
  if (!lua_isnoneornil(L, 1))
    offset = luaL_checkinteger(L, 1);

  size_t len=SPI_FLASH_SEC_SIZE-offset;
  if (!lua_isnoneornil(L, 2))
    len = luaL_checkinteger(L, 2);

  if (len+offset>SPI_FLASH_SEC_SIZE)
    return luaL_error(L, "romcfg get beyond partition end");

  if ((offset&3) || (len&3))
    return luaL_error(L, "romcfg write start/size must be 32 bit aligned");

  const esp_partition_t *part = find_partition();
  if (!part)
    return luaL_error(L, "no romcfg partition");

  char *page = malloc(len);
  if (!page)
    return luaL_error(L, "out of memory");

  if (esp_partition_read(part, offset, page, len) != ESP_OK)
  {
    free(page);
    return luaL_error(L, "error reading romcfg");
  }

  bool   use_len=true;
  if (!raw)
  {
    for (int i = 0; i < len; ++i)
    {
      if (page[i] == 0xff)
      {
        use_len=false;
        page[i] = 0;
      }
    }
  }
  if (use_len)
    lua_pushlstring(L, page, len);
  else
    lua_pushstring(L, page);

  free(page);
  return 1;
}

static int romcfg_get(lua_State *L)
{
  return romcfg_get_generic(L,false);
}

static int romcfg_get_raw(lua_State *L)
{
  return romcfg_get_generic(L,true);
}

static int romcfg_is_empty(lua_State *L)
{
  size_t data_len;
  const char *data = luaL_checklstring(L, 1, &data_len);

  for (int i=0;i<data_len;i++)
    if (((uint8_t)data[i])!=0xff)
      return 0;
  lua_pushinteger(L, 1);
  return 1;
}

LROT_BEGIN(romcfg)
  LROT_FUNCENTRY( get,     romcfg_get )
  LROT_FUNCENTRY( get_raw, romcfg_get_raw )
  LROT_FUNCENTRY( erase,   romcfg_erase )
  LROT_FUNCENTRY( write,   romcfg_write )
  LROT_FUNCENTRY( write_integer,   romcfg_write_integer )
  LROT_FUNCENTRY( read_integer,   romcfg_read_integer )
  LROT_FUNCENTRY( is_empty,romcfg_is_empty )
LROT_END(s4pp, NULL, 0)

NODEMCU_MODULE(ROMCFG, "romcfg", romcfg, NULL);
