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
#include "lextra.h"
#include "wifi_common.h"

static int wifi_getmode (lua_State *L)
{
  wifi_mode_t mode;
  esp_err_t err = esp_wifi_get_mode (&mode);
  if (err != ESP_OK)
    return luaL_error (L, "failed to get mode, code %d", err);
  lua_pushinteger (L, mode);
  return 1;
}

static int wifi_getchannel (lua_State *L)
{
  uint8_t primary;
  wifi_second_chan_t secondary;
  esp_err_t err = esp_wifi_get_channel (&primary, &secondary);
  if (err != ESP_OK)
    return luaL_error (L, "failed to get channel, code %d", err);
  lua_pushinteger (L, primary);
  lua_pushinteger (L, secondary);
  return 2;
}

static int wifi_mode (lua_State *L)
{
  int mode = luaL_checkinteger (L, 1);
  bool save = luaL_optbool (L, 2, DEFAULT_SAVE);
  SET_SAVE_MODE(save);
  esp_err_t err;
  switch (mode)
  {
    case WIFI_MODE_NULL:
    case WIFI_MODE_STA:
    case WIFI_MODE_AP:
    case WIFI_MODE_APSTA:
      return ((err = esp_wifi_set_mode (mode)) == ESP_OK) ?
        0 : luaL_error (L, "failed to set mode, code %d", err);
    default:
      return luaL_error (L, "invalid wifi mode %d", mode);
  }
}


static int wifi_start (lua_State *L)
{
  esp_err_t err = esp_wifi_start ();
  return (err == ESP_OK) ?
    0 : luaL_error(L, "failed to start wifi, code %d", err);
}


static int wifi_stop (lua_State *L)
{
  esp_err_t err = esp_wifi_stop ();
  return (err == ESP_OK) ?
    0 : luaL_error (L, "failed to stop wifi, code %d", err);
}

static int wifi_restore (lua_State *L)
{
  esp_err_t err = esp_wifi_restore ();
  return (err == ESP_OK) ?
    0 : luaL_error (L, "failed to restore wifi, code %d", err);
}

static int wifi_set_ps (lua_State *L)
{
  int mode = luaL_checkinteger (L, 1);

  esp_err_t err = esp_wifi_set_ps ((wifi_ps_type_t)mode);
  return (err == ESP_OK) ?
    0 : luaL_error (L, "failed to set wifi powersave, code %d", err);
}

static int wifi_get_ps (lua_State *L)
{
  wifi_ps_type_t mode;

  esp_err_t err = esp_wifi_get_ps (&mode);
  if (err == ESP_OK)
  {
    lua_pushinteger(L,(int)mode);
    return 1;
  }
  return luaL_error (L, "failed to get wifi powersave, code %d", err);
}


// It's in the IDF, but without any obviously-accessible include file
int pbkdf2_sha1(const char *passphrase, const char *ssid, size_t ssid_len, int iterations, uint8_t *buf, size_t buflen);
static int wifi_derive_key (lua_State *L)
{
  const char* ssid=luaL_checkstring(L,1);
  size_t ssid_len = lua_objlen (L, 1);
  const char* pass=luaL_checkstring(L,2);
  size_t pass_len = lua_objlen (L, 2);

  char derived[65];
  if (pass_len==64)
  {
    for (int i=0;i<64;i++)
      derived[i]=pass[i];
    derived[64]=0;
  }
  else
  {
    uint8_t key[32];
    pbkdf2_sha1(pass,ssid,ssid_len,
                4096,key,sizeof(key));
    for (int i=0;i<32;i++)
      sprintf(derived+2*i,"%02x",key[i]);
  }
  lua_pushstring(L,derived);
  return 1;
}

extern void wifi_ap_init (void);
extern void wifi_sta_init (void);
static int wifi_init (lua_State *L)
{
  wifi_ap_init ();
  wifi_sta_init ();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_wifi_init (&cfg);
  return (err == ESP_OK) ?
    0 : luaL_error (L, "failed to init wifi, code %d", err);
}


LROT_EXTERN(wifi_sta);
LROT_EXTERN(wifi_ap);

LROT_BEGIN(wifi)
  LROT_FUNCENTRY( getchannel,                 wifi_getchannel )
  LROT_FUNCENTRY( getmode,                    wifi_getmode )
  LROT_FUNCENTRY( mode,                       wifi_mode )
  LROT_FUNCENTRY( start,                      wifi_start )
  LROT_FUNCENTRY( stop,                       wifi_stop )
  LROT_FUNCENTRY( restore,                    wifi_restore )
  LROT_FUNCENTRY( derive_key,                 wifi_derive_key )
  LROT_FUNCENTRY( set_ps,                     wifi_set_ps )
  LROT_FUNCENTRY( get_ps,                     wifi_get_ps )

  LROT_TABENTRY ( sta,                        wifi_sta )
  LROT_TABENTRY ( ap,                         wifi_ap )


  LROT_NUMENTRY ( NULLMODE,                   WIFI_MODE_NULL )
  LROT_NUMENTRY ( STATION,                    WIFI_MODE_STA )
  LROT_NUMENTRY ( SOFTAP,                     WIFI_MODE_AP )
  LROT_NUMENTRY ( STATIONAP,                  WIFI_MODE_APSTA )

  LROT_NUMENTRY ( AUTH_OPEN,                  WIFI_AUTH_OPEN )
  LROT_NUMENTRY ( AUTH_WEP,                   WIFI_AUTH_WEP )
  LROT_NUMENTRY ( AUTH_WPA_PSK,               WIFI_AUTH_WPA_PSK )
  LROT_NUMENTRY ( AUTH_WPA2_PSK,              WIFI_AUTH_WPA2_PSK )
  LROT_NUMENTRY ( AUTH_WPA_WPA2_PSK,          WIFI_AUTH_WPA_WPA2_PSK )

  LROT_NUMENTRY ( STR_WIFI_SECOND_CHAN_NONE,  WIFI_SECOND_CHAN_NONE )
  LROT_NUMENTRY ( STR_WIFI_SECOND_CHAN_ABOVE, WIFI_SECOND_CHAN_ABOVE )
  LROT_NUMENTRY ( STR_WIFI_SECOND_CHAN_BELOW, WIFI_SECOND_CHAN_BELOW )

  LROT_NUMENTRY ( PS_NONE, WIFI_PS_NONE )
  LROT_NUMENTRY ( PS_MIN_MODEM, WIFI_PS_MIN_MODEM )
  LROT_NUMENTRY ( PS_MAX_MODEM, WIFI_PS_MAX_MODEM )

LROT_END(wifi, NULL, 0)

NODEMCU_MODULE(WIFI, "wifi", wifi, wifi_init);
