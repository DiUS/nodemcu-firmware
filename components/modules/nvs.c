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
#include "lmem.h"
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <nvs.h>
#include <nvs_flash.h>

_Static_assert(sizeof(lua_Number) <= sizeof(uint64_t), "storage size mismatch");

#define NVS_PART "nvsmodule"
#define NVS_NS "nodemcu"

static nvs_handle handle;

static int check_err(lua_State *L, esp_err_t err)
{
  switch (err)
  {
    case ESP_OK: break;
    case ESP_ERR_NOT_FOUND: // I'm dubious about this code, but it's doc'd
    case ESP_ERR_NVS_PART_NOT_FOUND:
      return luaL_error(L, "partition '" NVS_PART "' not found");
    case ESP_ERR_NVS_NO_FREE_PAGES:
      return luaL_error(L, "no free NVS pages - partition truncated?");
    case ESP_ERR_NVS_INVALID_HANDLE:
      return luaL_error(L, "NVS not initialised");
    case ESP_ERR_NVS_INVALID_NAME:
      return luaL_error(L, "key name invalid");
    case ESP_ERR_NVS_KEY_TOO_LONG:
      return luaL_error(L, "key too long");
    case ESP_ERR_NVS_VALUE_TOO_LONG:
      return luaL_error(L, "value too long");
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
      return luaL_error(L, "out of space");
    case ESP_ERR_NVS_TYPE_MISMATCH:
      return luaL_error(L, "value type mismatch");
    case ESP_ERR_NVS_NOT_FOUND:
      return luaL_error(L, "key not found");
    case ESP_ERR_NVS_INVALID_LENGTH:
      return luaL_error(L, "invalid length");
    default:
      return luaL_error(L, "unexpected NVS error %d", err);
  }
  return 0;
}


// Lua: nvs.init( { key1=, key2= } )
static int lnvs_init(lua_State *L)
{
  esp_err_t err;
  if (lua_istable(L, 1))
  {
    lua_getfield(L, 1, "key1");
    size_t key1_len;
    const char *key1 = luaL_checklstring(L, -1, &key1_len);
    lua_getfield(L, 1, "key2");
    size_t key2_len;
    const char *key2 = luaL_checklstring(L, -1, &key2_len);

    nvs_sec_cfg_t sec_cfg;
    if (key1_len != sizeof(sec_cfg.eky))
      return luaL_error(L, "expected key1 of size %d", sizeof(sec_cfg.eky));
    if (key2_len != sizeof(sec_cfg.tky))
      return luaL_error(L, "expected key2 of size %d", sizeof(sec_cfg.tky));

    memcpy(sec_cfg.eky, key1, key1_len);
    memcpy(sec_cfg.tky, key2, key2_len);

    err = nvs_flash_secure_init_partition(NVS_PART, &sec_cfg);
  }
  else
    err = nvs_flash_init_partition(NVS_PART);

  check_err(L, err);

  err = nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return luaL_error(L, "failed to open NVS: err %d", err);

  return 0;
}


// Lua: nvs.set(key, value)
static int lnvs_set(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  esp_err_t err;
  if (lua_isnumber(L, 2))
  {
    lua_Number n = lua_tonumber(L, 2);
    err = nvs_set_u64(handle, key, (uint64_t)n); // nasty double->uint64_t hack
  }
  else if (lua_isstring(L, 2))
  {
    size_t len;
    const char *blob = lua_tolstring(L, 2, &len);
    err = nvs_set_blob(handle, key, blob, len);
  }
  else
    return luaL_error(L, "unsupported value type");

  if (err == ESP_OK)
    err = nvs_commit(handle);

  check_err(L, err);

  return 0;
}


//Lua: value = nvs.get(key)
static int lnvs_get(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  uint64_t num;
  esp_err_t err = nvs_get_u64(handle, key, &num);
  if (err == ESP_OK)
  {
    lua_pushnumber(L, (lua_Number)num); // nasty uint64_t->number hack
    return 1;
  }
  else // I expected ESP_ERR_NVS_TYPE_MISMATCH for this, but got a not-found...
  {
    size_t needed_len;
    err = nvs_get_blob(handle, key, NULL, &needed_len);
    if (err == ESP_OK)
    {
      char *blob = luaM_malloc(L, needed_len);
      size_t len = needed_len; // don't overwrite needed_len, or bad things(tm)
      err = nvs_get_blob(handle, key, blob, &len);
      if (err == ESP_OK) // you'd sure hope it is!
        lua_pushlstring(L, blob, len);
      luaM_freemem(L, blob, needed_len);
      if (err == ESP_OK)
        return 1;
    }
  }

  if (err == ESP_ERR_NVS_NOT_FOUND) // Bernie doesn't want to pcall()
  {
    lua_pushnil(L);
    return 1;
  }

  return check_err(L, err);
}


// Lua: nvs.remove(key)
static int lnvs_remove(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  esp_err_t err = nvs_erase_key(handle, key);
  switch (err)
  {
    case ESP_OK: break;
    case ESP_ERR_NVS_NOT_FOUND: break; // not an error for us
    default:
      return check_err(L, err);
  }
  err = nvs_commit(handle);
  return check_err(L, err);
}


static int lnvs_erase(lua_State *L)
{
  return check_err(L, nvs_erase_all(handle));
}


// Lua: used, free, total = nvs.stats()
static int lnvs_stats(lua_State *L)
{
  nvs_stats_t stats;
  esp_err_t err = nvs_get_stats(NVS_PART, &stats);
  check_err(L, err);
  lua_pushnumber(L, stats.used_entries);
  lua_pushnumber(L, stats.free_entries);
  lua_pushnumber(L, stats.total_entries);
  return 3;
}


LROT_BEGIN(nvs)
  LROT_FUNCENTRY( init,      lnvs_init )
  LROT_FUNCENTRY( set,       lnvs_set )
  LROT_FUNCENTRY( get,       lnvs_get )
  LROT_FUNCENTRY( remove,    lnvs_remove )
  LROT_FUNCENTRY( erase,     lnvs_erase )
  LROT_FUNCENTRY( stats,     lnvs_stats )
LROT_END(nvs, NULL, 0)

NODEMCU_MODULE(NVS, "nvs", nvs, NULL);
