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
#include <sys/random.h>

_Static_assert(sizeof(lua_Number) <= sizeof(uint64_t), "storage size mismatch");

#define NVS_PART "nvsmodule"
#define NVS_NS "nodemcu"

static nvs_handle handle;


// The NVS implementation will happily hold multiple copies of the same key
// as long as they have different type, but then gets really weird when
// updating and erasing. Different types and different versions may be
// uncovered when erasing, so the only safe approach seems to be to keep
// erasing until no more key copies are found. Sigh.
static void erase_all_copies(const char *key)
{
  while (nvs_erase_key(handle, key) == ESP_OK) {}
}


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
#if !defined(CONFIG_LUA_VERSION_51)
  if (lua_isinteger(L, 2))
  {
    int64_t n = lua_tointeger(L, 2);
    erase_all_copies(key);
    err = nvs_set_i64(handle, key, n);
  }
  else
#endif
  if (lua_isnumber(L, 2))
  {
    int64_t n = (int64_t)lua_tonumber(L, 2); // reduce to integer
    erase_all_copies(key);
    err = nvs_set_i64(handle, key, n);
  }
  else if (lua_isstring(L, 2))
  {
    size_t len;
    const char *blob = lua_tolstring(L, 2, &len);
    erase_all_copies(key);
    err = nvs_set_blob(handle, key, blob, len);
  }
  else
    return luaL_error(L, "unsupported value type");

  if (err == ESP_OK)
    err = nvs_commit(handle);

  check_err(L, err);

  return 0;
}


// Lua: nvs.setstring(key, value)
static int lnvs_setstring(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  size_t len;
  const char *blob = luaL_checklstring(L, 2, &len);
  erase_all_copies(key);
  esp_err_t err = nvs_set_blob(handle, key, blob, len);

  if (err == ESP_OK)
    err = nvs_commit(handle);

  return check_err(L, err);
}


//Lua: value = nvs.getstring(key)
static int lnvs_getstring(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  size_t needed_len;
  esp_err_t err = nvs_get_blob(handle, key, NULL, &needed_len);
  if (err == ESP_OK)
  {
    char *blob = luaM_malloc(L, needed_len);
    size_t len = needed_len;
    err = nvs_get_blob(handle, key, blob, &len);
    if (err == ESP_OK)
      lua_pushlstring(L, blob, len);
    luaM_freemem(L, blob, needed_len);
    check_err(L, err);
    return 1;
  }
  else if (err == ESP_ERR_NVS_NOT_FOUND) // Bernie doesn't want to pcall()
  {
    lua_pushnil(L);
    return 1;
  }
  return check_err(L, err);
}


//Lua: value = nvs.get(key)
static int lnvs_get(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  int64_t inum;
  if (nvs_get_i64(handle, key, &inum) == ESP_OK)
  {
    lua_pushinteger(L, inum);
    return 1;
  }
  uint64_t unum;
  if (nvs_get_u64(handle, key, &unum) == ESP_OK)
  {
    lua_pushinteger(L, unum);
    return 1;
  }

  return lnvs_getstring(L);
}


// Lua: nvs.remove(key)
static int lnvs_remove(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  erase_all_copies(key);
  return check_err(L, nvs_commit(handle));
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


// This doesn't belong here, but it's here now due to historical reasons
static int lnvs_makekey(lua_State *L)
{
  char buf[32];
  getrandom(buf,32,0);
  lua_pushlstring (L, buf, 32);
  return 1;
}


// Lua: nvs.forcestring(key)
static int lnvs_forcestring(lua_State *L)
{
  const char *key = luaL_checkstring(L, 1);
  bool convert = false;
  uint64_t unum;
  esp_err_t err = nvs_get_u64(handle, key, &unum);
  if (err == ESP_OK)
  {
    lua_pushinteger(L, unum);
    convert = true;
  }
  int64_t inum;
  err = nvs_get_i64(handle, key, &inum);
  if (err == ESP_OK)
  {
    lua_pushinteger(L, inum);
    convert = true;
  }

  if (convert)
  {
    size_t len;
    const char *blob = lua_tolstring(L, -1, &len);
    erase_all_copies(key);
    check_err(L, nvs_set_blob(handle, key, blob, len));
    return check_err(L, nvs_commit(handle));
  }
  else
  {
    size_t needed_len;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &needed_len);
    if (err == ESP_OK)
    {
      size_t len = needed_len;
      char *blob = luaM_malloc(L, needed_len);
      err = nvs_get_blob(handle, key, blob, &len);
      if (err == ESP_OK && blob[len-1] == '\0')
      {
        // Drop a trailing nul-byte (which we introduced as a workaround for
        // the numeric SSID/passphrase issue).
        erase_all_copies(key);
        err = nvs_set_blob(handle, key, blob, len-1);
        if (err == ESP_OK)
          err = nvs_commit(handle);
      }
      luaM_freemem(L, blob, needed_len);
      return check_err(L, err);
    }
  }

  return 0;
}


LROT_BEGIN(nvs, NULL, 0)
  LROT_FUNCENTRY( init,      lnvs_init )
  LROT_FUNCENTRY( set,       lnvs_set )
  LROT_FUNCENTRY( setstring, lnvs_setstring )
  LROT_FUNCENTRY( get,       lnvs_get )
  LROT_FUNCENTRY( getstring, lnvs_getstring )
  LROT_FUNCENTRY( remove,    lnvs_remove )
  LROT_FUNCENTRY( erase,     lnvs_erase )
  LROT_FUNCENTRY( stats,     lnvs_stats )
  LROT_FUNCENTRY( makekey,   lnvs_makekey )
  LROT_FUNCENTRY( forcestring, lnvs_forcestring )
LROT_END(nvs, NULL, 0)

NODEMCU_MODULE(NVS, "nvs", nvs, NULL);
