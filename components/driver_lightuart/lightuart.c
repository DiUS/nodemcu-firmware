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

#include "driver/lightuart.h"
#include <esp_intr.h>
#include <esp_intr_alloc.h>
#include <esp_clk.h>
#include <esp_log.h>
#include <soc/uart_struct.h>
#include <driver/periph_ctrl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <rom/uart.h> // for uart_tx_wait_idle

static const char *tag = "lightuart";

static struct lightuart {
  unsigned      uart_no;
  task_handle_t tsk;
  task_prio_t   prio;
  xQueueHandle  tx_q;
  xQueueHandle  rx_q;
  intr_handle_t intr;

  LightUartSetup_t cfg;
} lightuart_setup[3];

static uart_dev_t * DRAM_ATTR uarts[] = { &UART0, &UART1, &UART2 };

static void IRAM_ATTR lightuart_isr (void *arg)
{
  struct lightuart *lu = arg;

  int events = 0;
  // If we enter with data already on the rx queue, we assume we've already
  // posted about it. A bit dangerous, but much easier on the post queue.
  bool should_post = xQueueIsQueueEmptyFromISR(lu->rx_q);

  // If we flag an error below, also set rx_rdy if there's stuff in the queue.
  if (!should_post)
    events |= LIGHTUART_RX_RDY;

  uart_dev_t *dev = uarts[lu->uart_no];

  uint32_t ints = dev->int_st.val;
  while (ints)
  {
    if (ints & UART_FRM_ERR_INT_ENA)
    {
      events |= LIGHTUART_FRAME_ERR;
      dev->int_clr.frm_err = 1;
      should_post = true;
    }
    if (ints & UART_RXFIFO_OVF_INT_ENA)
    {
      events |= LIGHTUART_HW_OVF;
      dev->int_clr.rxfifo_ovf = 1;
      should_post = true;
    }
    if ((ints & UART_RXFIFO_TOUT_INT_ENA) ||
        (ints & UART_RXFIFO_FULL_INT_ENA))
    {
      uint32_t fifo_len = dev->status.rxfifo_cnt;
      for (uint32_t i = 0; i < fifo_len; ++i)
      {
        events |= LIGHTUART_RX_RDY;
        char c = dev->fifo.rw_byte;
        if (!xQueueSendToBackFromISR(lu->rx_q, &c, NULL))
        {
          events |= LIGHTUART_SOFT_OVF;
          should_post = true;
        }
      }
      dev->int_clr.val = UART_RXFIFO_TOUT_INT_ENA | UART_RXFIFO_FULL_INT_ENA;
    }
    if (ints & UART_TXFIFO_EMPTY_INT_ENA)
    {
      uint8_t byte;
      uint32_t n = 0;
      uint32_t FIFO_OUT_MAX = 127;
      while (n < FIFO_OUT_MAX && xQueueReceiveFromISR(lu->tx_q, &byte, NULL))
      {
        dev->fifo.rw_byte = byte;
        ++n;
      }
      dev->int_clr.txfifo_empty = 1;

      if (n < FIFO_OUT_MAX)
        dev->int_ena.txfifo_empty = 0;
    }
    ints = dev->int_st.val;
  }

  // TODO: suppress floods of overflow events somehow
  if (events && should_post)
    task_post(lu->prio, lu->tsk, MK_LIGHTUART_TASK_PARAM(lu->uart_no, events));
}


void lightuart_write_bytes(uint32_t uart_no, const void *bytes, size_t len)
{
  struct lightuart *lu = &lightuart_setup[uart_no];
  uart_dev_t *dev = uarts[uart_no];
  for (; len; --len, ++bytes)
  {
    if (xQueueSendToBack(lu->tx_q, bytes, 0) != pdTRUE)
    {
      // queue full, need to drain it
      dev->int_ena.txfifo_empty = 1;
      xQueueSendToBack(lu->tx_q, bytes, portMAX_DELAY);
    }
  }
  // make sure the ISR will drain the tx q when it can
  dev->int_ena.txfifo_empty = 1;
}


int lightuart_read_bytes(uint32_t uart_no, void *out, size_t bufsiz, uint32_t ticks_to_wait)
{
  struct lightuart *lu = &lightuart_setup[uart_no];
  char *p = out;
  int n = 0;
  for (; n < bufsiz; ++n, ++p)
  {
    if (!xQueueReceive(lu->rx_q, p, ticks_to_wait))
      break;
  }
  return n;
}


void lightuart_getconfig(uint32_t uart_no, LightUartSetup_t *cfg)
{
  struct lightuart *lu = &lightuart_setup[uart_no];
  *cfg = lu->cfg;
}


void lightuart_init (uint32_t uart_no, const LightUartSetup_t *cfg, task_handle_t tsk, task_prio_t prio)
{
  if (uart_no > 2 || uart_no == CONFIG_CONSOLE_UART_NUM)
  {
    ESP_LOGE(tag, "invalid uart %d (console conflict?)", uart_no);
    return;
  }

  struct lightuart *lu = &lightuart_setup[uart_no];
  if (lu->tx_q)
  {
    vQueueDelete(lu->tx_q);
    lu->tx_q = NULL;
  }
  if (lu->rx_q)
  {
    vQueueDelete(lu->rx_q);
    lu->rx_q = NULL;
  }
  if (lu->intr)
  {
    esp_intr_free(lu->intr);
    lu->intr = NULL;
  }

  lu->uart_no = uart_no;
  lu->tsk = tsk;
  lu->prio = prio;
  lu->tx_q = xQueueCreate(cfg->tx_q_size, sizeof(char));
  lu->rx_q = xQueueCreate(cfg->rx_q_size, sizeof(char));
  lu->cfg = *cfg;

  static const int periph[] = {
    PERIPH_UART0_MODULE, PERIPH_UART1_MODULE, PERIPH_UART2_MODULE
  };
  periph_module_enable(periph[uart_no]);

  uart_tx_wait_idle(uart_no);

  uart_dev_t *dev = uarts[uart_no];

  // The rxfifo_rst flag is apparently broken for uart1/2, and the rxfifo_cnt
  // does not appear to be entirely reliable either.
  while (dev->mem_rx_status.wr_addr != dev->mem_rx_status.rd_addr)
  {
    (void)dev->fifo.rw_byte;
  }

  dev->int_ena.rxfifo_full = 1;
  dev->int_ena.rxfifo_tout = 1;
  dev->int_ena.rxfifo_ovf = 1;
  dev->int_ena.frm_err = 1;

  dev->conf0.val = 0;

  dev->conf0.tick_ref_always_on = 1; // 80MHz clock, not the 1MHz one please
  dev->conf0.bit_num = cfg->data_bits;
  dev->conf0.parity_en = cfg->parity != LIGHTUART_PARITY_NONE;
  dev->conf0.parity = cfg->parity & 0x1;
  dev->conf0.stop_bit_num = cfg->stop_bits;
  dev->conf0.err_wr_mask = 0;

  dev->conf1.val = 0;
  dev->conf1.rx_tout_en = 1;
  dev->conf1.rx_tout_thrhd = 2;
  dev->conf1.rxfifo_full_thrhd = 8;
  dev->conf1.txfifo_empty_thrhd = 1;

  dev->auto_baud.en = 0;
  dev->flow_conf.val = 0;
  dev->rs485_conf.val = 0;

  int clk_freq =
    dev->conf0.tick_ref_always_on ? esp_clk_apb_freq() : REF_CLK_FREQ;
  uint32_t clk_div = (clk_freq << 4) / cfg->bit_rate;
  if (clk_div < 16)
    ESP_LOGE(tag, "bit rate too high for clock on uart %d", uart_no);
  dev->clk_div.div_int = clk_div >> 4;
  dev->clk_div.div_frag = clk_div & 0xf;

  if (cfg->tx_io >= 0)
  {
    static const int tx_sig[] = { U0TXD_OUT_IDX, U1TXD_OUT_IDX, U2TXD_OUT_IDX };
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[cfg->tx_io], PIN_FUNC_GPIO);
    gpio_set_level(cfg->tx_io, 1);
    gpio_matrix_out(cfg->tx_io, tx_sig[uart_no], 0, 0);
  }
  if (cfg->rx_io >= 0)
  {
    static const int rx_sig[] = { U0RXD_IN_IDX, U1RXD_IN_IDX, U2RXD_IN_IDX };
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[cfg->rx_io], PIN_FUNC_GPIO);
    gpio_set_pull_mode(cfg->rx_io, GPIO_PULLUP_ONLY);
    gpio_set_direction(cfg->rx_io, GPIO_MODE_INPUT);
    gpio_matrix_in(cfg->rx_io, rx_sig[uart_no], 0);
  }

  static const int srcs[] = {
    ETS_UART0_INTR_SOURCE, ETS_UART1_INTR_SOURCE, ETS_UART2_INTR_SOURCE
  };
  int flags =
    ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_INTRDISABLED;

  esp_err_t err =
    esp_intr_alloc(srcs[uart_no], flags, lightuart_isr, lu, &lu->intr);
  if (err != ESP_OK)
    ESP_LOGE(tag,"failed to install isr for %u due to code %d\n", uart_no, err);

  err = esp_intr_enable(lu->intr);
  if (err != ESP_OK)
    ESP_LOGE(tag, "failed to enable isr for %u due to code %d\n", uart_no, err);
}
