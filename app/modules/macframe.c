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
#include "c_stdio.h"
#include "c_stdlib.h"

#define pletohs(p) \
  ((unsigned short)                       \
   ((unsigned short)*((const unsigned char *)(p)+1)<<8|  \
   (unsigned short)*((const unsigned char *)(p)+0)<<0))

#define phtoles(p, v) \
  {                 \
    (p)[0] = (unsigned char)((v) >> 0);    \
    (p)[1] = (unsigned char)((v) >> 8);    \
  }

#define MAC_HDR_MIN_LEN 24

#define FC_TOWDS      0x0100
#define FC_FROMWDS    0x0200

static const char str_fc[]       = "framecontrol";
static const char str_duration[] = "duration";
static const char str_seq[]      = "sequencecontrol";
static const char str_payload[]  = "payload";

static const char str_dest[]     = "destination";
static const char str_src[]      = "source";
static const char str_trans[]    = "transmitter";
static const char str_recv[]     = "receiver";
static const char str_bssid[]    = "bssid";

static const char mac_fmt[] = "%02x-%02x-%02x-%02x-%02x-%02x";

// Depending on the to/from WDS flags, the interpretation of mac1..4 differs
static const char *mac_addr_names[4][4] =
{
  { str_dest,  str_src,   str_bssid, NULL },
  { str_dest,  str_bssid, str_src,   NULL },
  { str_bssid, str_src,   str_dest,  NULL },
  { str_recv,  str_trans, str_dest,  str_src }
};


static const char *format_mac (char buf[18], const uint8_t *raw)
{
  c_sprintf (buf, (char *)mac_fmt,
    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
  return buf;
}

static unsigned hexval (char c)
{
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return c - '0';
}

static void pack_mac (char *dst, const char *mac_str)
{
  int i;
  for (i = 0; i < 6; ++i)
  {
    if (mac_str[0] && mac_str[1])
    {
      *dst++ = (hexval (mac_str[0]) << 4) | (hexval (mac_str[1]));
      mac_str += 2;
    }
    else
      *dst++ = 0;

    if (*mac_str == ':' || *mac_str == '-')
      ++mac_str;
  }
}


static int macframe_parse (lua_State *L)
{
  size_t len;
  const char *frame = luaL_checklstring (L, 1, &len);
  const char *p = frame;

  if (len < MAC_HDR_MIN_LEN)
    return luaL_error (L, "runt frame");

  lua_createtable (L, 0, 7);

  lua_pushlstring (L, str_fc, sizeof (str_fc) -1);
  uint16_t fc = pletohs (p);
  p += 2;
  lua_pushinteger (L, fc);
  lua_settable (L, -3);

  lua_pushlstring (L, str_duration, sizeof (str_duration) -1);
  uint16_t dura = pletohs (p);
  p += 2;
  lua_pushinteger (L, dura);
  lua_settable (L, -3);

  unsigned addr_interpretation = (fc & (FC_TOWDS | FC_FROMWDS)) >> 8;
  const char **mac_names = mac_addr_names[addr_interpretation];

  if (mac_names[3] && len < 30)
    return luaL_error (L, "runt frame (no addr4)");

  char mac_buf[18];
  int i;
  for (i = 0; i < 4; ++i)
  {
    if (mac_names[i])
    {
      lua_pushstring (L, mac_names[i]);
      lua_pushlstring (L, format_mac (mac_buf, p), sizeof (mac_buf) - 1);
      p += 6;
      lua_settable (L, -3);
    }

    if (i == 2)
    {
      lua_pushlstring (L, str_seq, sizeof (str_seq) -1);
      uint16_t seq = pletohs (p);
      p += 2;
      lua_pushinteger (L, seq);
      lua_settable (L, -3);
    }
  }

  lua_pushlstring (L, str_payload, sizeof (str_payload) - 1);
  lua_pushlstring (L, p, len - (p - frame));
  lua_settable (L, -3);

  return 1;
}


static int macframe_create (lua_State *L)
{
  luaL_checktype (L, 1, LUA_TTABLE);

  lua_getfield (L, 1, str_fc);
  uint16_t fc = luaL_checknumber (L, -1);
  lua_pop (L, 1);

  unsigned addr_interpretation = (fc & (FC_TOWDS | FC_FROMWDS)) >> 8;
  const char **mac_names = mac_addr_names[addr_interpretation];

  lua_getfield (L, 1, str_payload);
  size_t payload_len;
  const char *payload = luaL_checklstring (L, -1, &payload_len);

  size_t hdr_len = MAC_HDR_MIN_LEN + (mac_names[3] ? 6 : 0);
  size_t frame_len = payload_len + hdr_len;

  char *frame = (char *)c_malloc (frame_len);
  char *p = frame;

  memcpy (frame + hdr_len, payload, payload_len);
  lua_pop (L, 1);

  phtoles (p, fc);
  p += 2;

  lua_getfield (L, 1, str_duration);
  uint16_t dura = luaL_checknumber (L, -1);
  lua_pop (L, 1);
  phtoles (p, dura);
  p += 2;

  int i;
  for (i = 0; i < 4; ++i)
  {
    if (mac_names[i])
    {
      lua_getfield (L, 1, mac_names[i]);
      const char *mac = luaL_checkstring (L, -1);
      pack_mac (p, mac);
      p += 6;
      lua_pop (L, 1);
    }

    if (i == 2)
    {
      lua_getfield (L, 1, str_seq);
      uint16_t seq = luaL_checknumber (L, -1);
      lua_pop (L, 1);
      phtoles (p, seq);
      p += 2;
    }
  }

  lua_pushlstring (L, frame, frame_len);
  c_free (frame);

  return 1;
}


static const LUA_REG_TYPE macframe_map[] =
{
  { LSTRKEY("parse"),    LFUNCVAL(macframe_parse) },
  { LSTRKEY("create"),   LFUNCVAL(macframe_create) },

  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(MACFRAME, "macframe", macframe_map, NULL);
