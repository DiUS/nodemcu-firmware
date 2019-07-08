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

static int romcfg_get(lua_State *L)
{
  const esp_partition_t *part =
    esp_partition_find_first(
      PLATFORM_PARTITION_TYPE_DIUS,
      PLATFORM_PARTITION_SUBTYPE_DIUS_ROMCFG,
      NULL);

  if (!part)
    return luaL_error(L, "no romcfg partition");

  char *page = malloc(SPI_FLASH_SEC_SIZE);
  if (!page)
    return luaL_error(L, "out of memory");

  if (esp_partition_read(part, 0, page, SPI_FLASH_SEC_SIZE) != ESP_OK)
  {
    free(page);
    return luaL_error(L, "error reading romcfg");
  }

  for (int i = 0; i < SPI_FLASH_SEC_SIZE; ++i)
    if (page[i] == 0xff)
      page[i] = 0;

  lua_pushstring(L, page);

  free(page);
  return 1;
}


LROT_BEGIN(romcfg)
  LROT_FUNCENTRY( get, romcfg_get )
LROT_END(s4pp, NULL, 0)

NODEMCU_MODULE(ROMCFG, "romcfg", romcfg, NULL);
