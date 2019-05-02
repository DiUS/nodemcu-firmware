/*
 * Copyright 2016 Dius Computing Pty Ltd. All rights reserved.
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
#include "user_interface.h"

static int cb_ref = LUA_NOREF;

static void send_done_cb (uint8_t status)
{
  wifi_unregister_send_pkt_freedom_cb ();

  lua_State *L = lua_getstate ();
  lua_rawgeti (L, LUA_REGISTRYINDEX, cb_ref);

  luaL_unref (L, LUA_REGISTRYINDEX, cb_ref);
  cb_ref = LUA_NOREF;

  lua_pushinteger (L, status);
  lua_call (L, 1, 0);
}


// Lua: raw80211.send (framedata [, sent_cb [, sys_seq]])
static int raw80211_send (lua_State *L)
{
  size_t len;
  const char *frame = luaL_checklstring (L, 1, &len);

  if (lua_type (L, 2) == LUA_TFUNCTION || lua_type (L, 2) == LUA_TLIGHTFUNCTION)
  {
    luaL_unref (L, LUA_REGISTRYINDEX, cb_ref);
    lua_pushvalue (L, 2);
    cb_ref = luaL_ref (L, LUA_REGISTRYINDEX);
    wifi_register_send_pkt_freedom_cb (send_done_cb);
  }

  int sys_seq = luaL_optinteger (L, 3, 0);

  // CAUTION: const-cast here
  lua_pushinteger (L, wifi_send_pkt_freedom ((void *)frame, len, sys_seq));
  return 1;
}

// Lua: raw80211.set_user_fixed_rate (mask,rate)
static int raw80211_set_user_fixed_rate (lua_State *L)
{
  int mask = luaL_checknumber (L, 1);
  int rate = luaL_checknumber (L, 2);

  lua_pushinteger (L, wifi_set_user_fixed_rate (mask,rate));
}


// Lua: raw80211.set_user_sup_rate (min,max)
static int raw80211_set_user_sup_rate (lua_State *L)
{
  int min = luaL_checknumber (L, 1);
  int max = luaL_checknumber (L, 2);

  lua_pushinteger (L, wifi_set_user_sup_rate (min,max));
}

    // Lua: raw80211.set_user_rate_limit (mode, interface, max, min)
static int raw80211_set_user_rate_limit (lua_State *L)
{
  int mode = luaL_checknumber (L, 1);
  int interface = luaL_checknumber (L, 2);
  int max = luaL_checknumber (L, 3);
  int min = luaL_checknumber (L, 4);

  wifi_set_user_limit_rate_mask(wifi_get_user_limit_rate_mask() | (1<<interface));
  lua_pushinteger (L, wifi_set_user_rate_limit (mode,interface,max,min));
}


static const LUA_REG_TYPE raw80211_map[] =
{
  { LSTRKEY("send"),    LFUNCVAL(raw80211_send) },
  { LSTRKEY("set_user_fixed_rate"),    LFUNCVAL(raw80211_set_user_fixed_rate) },
  { LSTRKEY("set_user_sup_rate"),    LFUNCVAL(raw80211_set_user_sup_rate) },
  { LSTRKEY("set_user_rate_limit"),    LFUNCVAL(raw80211_set_user_rate_limit) },
  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(RAW80211, "raw80211", raw80211_map, NULL);
