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

#include <stdint.h>
#include <stdlib.h>
#include <task/task.h>

typedef enum
{
  LIGHTUART_NUM_BITS_5 = 0x0,
  LIGHTUART_NUM_BITS_6 = 0x1,
  LIGHTUART_NUM_BITS_7 = 0x2,
  LIGHTUART_NUM_BITS_8 = 0x3
} LightUartNumBits_t;

typedef enum
{
  LIGHTUART_PARITY_NONE = 0x2,
  LIGHTUART_PARITY_ODD  = 0x1,
  LIGHTUART_PARITY_EVEN = 0x0
} LightUartParity_t;

typedef enum
{
  LIGHTUART_STOP_BITS_1   = 0x1,
  LIGHTUART_STOP_BITS_1_5 = 0x2,
  LIGHTUART_STOP_BITS_2   = 0x3
} LightUartStopBits_t;

typedef struct
{
  uint32_t            bit_rate;
  LightUartNumBits_t  data_bits;
  LightUartParity_t   parity;
  LightUartStopBits_t stop_bits;
  int                 tx_io; // -1 = don't set
  int                 rx_io; // -1 = don't set
  size_t              tx_q_size;
  size_t              rx_q_size;
  bool                tx_inv;
  bool                rx_inv;
} LightUartSetup_t;

#define LIGHTUART_RX_RDY     0x01
#define LIGHTUART_FRAME_ERR  0x02
#define LIGHTUART_HW_OVF     0x04
#define LIGHTUART_SOFT_OVF   0x08

#define MK_LIGHTUART_TASK_PARAM(uart_no, ev) (uart_no << 24 | ev)
#define LIGHTUART_NO(task_param) (task_param >> 24)
#define LIGHTUART_EVENT(task_param) (task_param & 0xffffff)

void lightuart_init (unsigned uart_no, const LightUartSetup_t *cfg, task_handle_t tsk, task_prio_t wanted_prio);

void lightuart_write_bytes(uint32_t uart_no, const void *bytes, size_t len);

int lightuart_read_bytes(uint32_t uart_no, void *out, size_t bufsiz, uint32_t ticks_to_wait);

void lightuart_getconfig(uint32_t uart_no, LightUartSetup_t *cfg);
