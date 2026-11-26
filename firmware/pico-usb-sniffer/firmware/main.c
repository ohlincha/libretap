// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022, Alex Taradov <alex@taradov.com>. All rights reserved.
// Changed for LibreTap's USB Sniffer Stream (2026): core 0 now handles the commands itself, takes the packets core 1
// leaves in the capture ring, filters and encodes them (stream.c) into an output ring and sends that over the
// virtual COM port as fast as the host reads it. Clock, timer and USB setup are usb-sniffer-lite's, unchanged.

/*- Includes ----------------------------------------------------------------*/
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rp2040.h"
#include "hal_gpio.h"
#include "capture.h"
#include "globals.h"
#include "stream.h"
#include "utils.h"
#include "usb.h"

/*- Definitions -------------------------------------------------------------*/
#define F_REF      12000000
#define F_CPU      120000000
#define F_PER      120000000
#define F_RTC      (F_REF / 256)
#define F_TICK     1000000

#define USB_BUFFER_SIZE    64
#define STATUS_TIMEOUT     500000 // us
#define STREAM_BLINK       100000 // us
#define STATUS_PERIOD      1000000 // us: a STATUS record this often

#ifndef TX_SIZE
#define TX_SIZE            (80 * 1024) // the output ring
#endif
#define TX_HEADROOM        (8 * 1024)  // room for what one raw packet can produce (text mode: a long line)
#define CONSUME_BUDGET     64          // packets per main loop pass, so USB is served in between

HAL_GPIO_PIN(LED_O, 0, 25, sio_25)
HAL_GPIO_PIN(LED_R, 0, 26, sio_26)

enum
{
  State_Idle,
  State_Starting,   // waiting for core 1 to clear the ring ('S')
  State_Running,
  State_Stopping,   // 'p' sent; drain the ring once core 1 answered 'P'
};

/*- Variables ---------------------------------------------------------------*/
static uint8_t app_recv_buffer[USB_BUFFER_SIZE];
static uint8_t app_send_buffer[USB_BUFFER_SIZE];
static bool app_send_zlp = false;
static bool app_send_pending = false;
static bool app_recv_pending = false;
static bool app_vcp_open = false;

static uint8_t tx_buf[TX_SIZE];
static int tx_head = 0;
static int tx_tail = 0;
static int tx_used = 0;
static int tx_peak = 0;

static se_t g_se;
static int g_state = State_Idle;
static bool g_core1_started = false;
static bool g_core1_stopped = false;
static bool g_keep_all = false;
static bool g_text = false;
static uint32_t g_rd = 0;
static uint32_t g_last_status = 0;
static uint32_t g_last_errors = 0;
static uint32_t g_ring_peak = 0;

static const char *capture_speed_str[CaptureSpeedCount] =
{
  [CaptureSpeed_Low]  = "Low",
  [CaptureSpeed_Full] = "Full",
};

/*- Implementations ---------------------------------------------------------*/

//-----------------------------------------------------------------------------
static void sys_init(void)
{
  // Enable XOSC
  XOSC->CTRL     = (XOSC_CTRL_FREQ_RANGE_1_15MHZ << XOSC_CTRL_FREQ_RANGE_Pos);
  XOSC->STARTUP  = 47; // ~1 ms @ 12 MHz
  XOSC_SET->CTRL = (XOSC_CTRL_ENABLE_ENABLE << XOSC_CTRL_ENABLE_Pos);
  while (0 == (XOSC->STATUS & XOSC_STATUS_STABLE_Msk));

  // Setup SYS PLL for 12 MHz * 40 / 4 / 1 = 120 MHz
  RESETS_CLR->RESET = RESETS_RESET_pll_sys_Msk;
  while (0 == RESETS->RESET_DONE_b.pll_sys);

  PLL_SYS->CS = (1 << PLL_SYS_CS_REFDIV_Pos);
  PLL_SYS->FBDIV_INT = 40;
  PLL_SYS->PRIM = (4 << PLL_SYS_PRIM_POSTDIV1_Pos) | (1 << PLL_SYS_PRIM_POSTDIV2_Pos);

  PLL_SYS_CLR->PWR = PLL_SYS_PWR_VCOPD_Msk | PLL_SYS_PWR_PD_Msk;
  while (0 == PLL_SYS->CS_b.LOCK);

  PLL_SYS_CLR->PWR = PLL_SYS_PWR_POSTDIVPD_Msk;

  // Setup USB PLL for 12 MHz * 36 / 3 / 3 = 48 MHz
  RESETS_CLR->RESET = RESETS_RESET_pll_usb_Msk;
  while (0 == RESETS->RESET_DONE_b.pll_usb);

  PLL_USB->CS = (1 << PLL_SYS_CS_REFDIV_Pos);
  PLL_USB->FBDIV_INT = 36;
  PLL_USB->PRIM = (3 << PLL_SYS_PRIM_POSTDIV1_Pos) | (3 << PLL_SYS_PRIM_POSTDIV2_Pos);

  PLL_USB_CLR->PWR = PLL_SYS_PWR_VCOPD_Msk | PLL_SYS_PWR_PD_Msk;
  while (0 == PLL_USB->CS_b.LOCK);

  PLL_USB_CLR->PWR = PLL_SYS_PWR_POSTDIVPD_Msk;

  // Switch clocks to their final socurces
  CLOCKS->CLK_REF_CTRL = (CLOCKS_CLK_REF_CTRL_SRC_xosc_clksrc << CLOCKS_CLK_REF_CTRL_SRC_Pos);

  CLOCKS->CLK_SYS_CTRL = (CLOCKS_CLK_SYS_CTRL_AUXSRC_clksrc_pll_sys << CLOCKS_CLK_SYS_CTRL_AUXSRC_Pos);
  CLOCKS_SET->CLK_SYS_CTRL = (CLOCKS_CLK_SYS_CTRL_SRC_clksrc_clk_sys_aux << CLOCKS_CLK_SYS_CTRL_SRC_Pos);

  CLOCKS->CLK_PERI_CTRL = CLOCKS_CLK_PERI_CTRL_ENABLE_Msk |
      (CLOCKS_CLK_PERI_CTRL_AUXSRC_clk_sys << CLOCKS_CLK_PERI_CTRL_AUXSRC_Pos);

  CLOCKS->CLK_USB_CTRL = CLOCKS_CLK_USB_CTRL_ENABLE_Msk |
      (CLOCKS_CLK_USB_CTRL_AUXSRC_clksrc_pll_usb << CLOCKS_CLK_USB_CTRL_AUXSRC_Pos);

  CLOCKS->CLK_ADC_CTRL = CLOCKS_CLK_ADC_CTRL_ENABLE_Msk |
      (CLOCKS_CLK_ADC_CTRL_AUXSRC_clksrc_pll_usb << CLOCKS_CLK_ADC_CTRL_AUXSRC_Pos);

  CLOCKS->CLK_RTC_DIV = (256 << CLOCKS_CLK_RTC_DIV_INT_Pos); // 12MHz / 256 = 46875 Hz
  CLOCKS->CLK_RTC_CTRL = CLOCKS_CLK_RTC_CTRL_ENABLE_Msk |
      (CLOCKS_CLK_RTC_CTRL_AUXSRC_xosc_clksrc << CLOCKS_CLK_ADC_CTRL_AUXSRC_Pos);

  // Configure 1 us tick for watchdog and timer
  WATCHDOG->TICK = ((F_REF/F_TICK) << WATCHDOG_TICK_CYCLES_Pos) | WATCHDOG_TICK_ENABLE_Msk;

  // Enable GPIOs
  RESETS_CLR->RESET = RESETS_RESET_io_bank0_Msk | RESETS_RESET_pads_bank0_Msk;
  while (0 == RESETS->RESET_DONE_b.io_bank0 || 0 == RESETS->RESET_DONE_b.pads_bank0);
}

//-----------------------------------------------------------------------------
static void serial_number_init(void)
{
  volatile uint8_t *uid = (volatile uint8_t *)0x20041f01;
  uint32_t sn = 5381;

  for (int i = 0; i < 16; i++)
    sn = ((sn << 5) + sn) ^ uid[i];

  for (int i = 0; i < 8; i++)
    usb_serial_number[i] = "0123456789ABCDEF"[(sn >> (i * 4)) & 0xf];

  usb_serial_number[8] = 0;
}

//-----------------------------------------------------------------------------
static void timer_init(void)
{
  RESETS_CLR->RESET = RESETS_RESET_timer_Msk;
  while (0 == RESETS->RESET_DONE_b.timer);

  TIMER->ALARM0 = TIMER->TIMELR + STATUS_TIMEOUT;
}

//-----------------------------------------------------------------------------
static void status_timer_task(void)
{
  if (TIMER->INTR & TIMER_INTR_ALARM_0_Msk)
  {
    TIMER->INTR = TIMER_INTR_ALARM_0_Msk;
    TIMER->ALARM0 = TIMER->TIMELR + ((g_state == State_Idle) ? STATUS_TIMEOUT : STREAM_BLINK);
    HAL_GPIO_LED_O_toggle();
  }
}

/*- The output ring ---------------------------------------------------------*/

//-----------------------------------------------------------------------------
static int tx_space(void *ctx)
{
  (void)ctx;
  return TX_SIZE - tx_used;
}

//-----------------------------------------------------------------------------
static void tx_write(void *ctx, const uint8_t *data, int size)
{
  (void)ctx;

  while (size > 0 && tx_used < TX_SIZE)
  {
    int n = size;

    if (n > TX_SIZE - tx_head)
      n = TX_SIZE - tx_head;
    if (n > TX_SIZE - tx_used)
      n = TX_SIZE - tx_used;

    memcpy(&tx_buf[tx_head], data, n);
    tx_head += n;
    if (tx_head == TX_SIZE)
      tx_head = 0;
    tx_used += n;
    data += n;
    size -= n;
  }

  if (tx_used > tx_peak)
    tx_peak = tx_used;
}

//-----------------------------------------------------------------------------
static void tx_clear(void)
{
  tx_head = tx_tail = tx_used = 0;
  app_send_zlp = false;
}

//-----------------------------------------------------------------------------
void usb_cdc_send_callback(void)
{
  app_send_pending = false;
}

//-----------------------------------------------------------------------------
static void tx_task(void)
{
  int n;

  if (!app_vcp_open)
  {
    tx_clear();
    return;
  }

  if (app_send_pending)
    return;

  if (0 == tx_used)
  {
    if (app_send_zlp) // the host completes a read on a short packet
    {
      app_send_zlp = false;
      app_send_pending = true;
      usb_cdc_send(app_send_buffer, 0);
    }
    return;
  }

  n = LIMIT(tx_used, USB_BUFFER_SIZE);

  for (int i = 0; i < n; i++)
  {
    app_send_buffer[i] = tx_buf[tx_tail++];
    if (tx_tail == TX_SIZE)
      tx_tail = 0;
  }
  tx_used -= n;

  app_send_zlp = (USB_BUFFER_SIZE == n);
  app_send_pending = true;
  usb_cdc_send(app_send_buffer, n);
}

/*- Commands ----------------------------------------------------------------*/

//-----------------------------------------------------------------------------
// Replies: plain text, or a TEXT record while binary records are streaming
static void out_text(const char *s)
{
  if (g_state != State_Idle && !g_text)
    se_text(&g_se, TIMER->TIMERAWL, s);
  else
    tx_write(0, (const uint8_t *)s, strlen(s));
}

//-----------------------------------------------------------------------------
static void print_help(void)
{
  static char buf[1024];

  buf[0] = 0;
  strcat(buf, "\r\n-------------------------------------------------------------------\r\n");
  strcat(buf, SE_NAME " " SE_VERSION " (protocol 1), based on USB Sniffer Lite. Built on "__DATE__" "__TIME__".\r\n");
  strcat(buf, "\r\n");
  strcat(buf, "Settings:\r\n");
  strcat(buf, "  e - Capture speed       : "); strcat(buf, capture_speed_str[g_capture_speed]); strcat(buf, "\r\n");
  strcat(buf, "  k - Keep idle traffic   : "); strcat(buf, g_keep_all ? "Enabled" : "Disabled"); strcat(buf, "\r\n");
  strcat(buf, "  o - Output format       : "); strcat(buf, g_text ? "Text" : "Binary"); strcat(buf, "\r\n");
  strcat(buf, "\r\n");
  strcat(buf, "Commands:\r\n");
  strcat(buf, "  h - Print this help message\r\n");
  strcat(buf, "  v - Print the firmware version\r\n");
  strcat(buf, "  s - Start streaming\r\n");
  strcat(buf, "  p - Stop streaming\r\n");
  strcat(buf, "\r\n");
  out_text(buf);
}

//-----------------------------------------------------------------------------
static void setting_changed(const char *name, const char *value)
{
  static char buf[96];

  buf[0] = 0;
  strcat(buf, name);
  strcat(buf, " changed to ");
  strcat(buf, value);
  strcat(buf, "\r\n");
  out_text(buf);
}

//-----------------------------------------------------------------------------
static void start_streaming(void)
{
  static const se_out_t out = { tx_space, tx_write, 0 };

  if (g_state != State_Idle)
    return;

  set_error(false);
  g_rd = 0;
  g_last_errors = 0;
  g_ring_peak = 0;
  tx_peak = tx_used;
  g_core1_started = false;
  g_core1_stopped = false;
  g_last_status = TIMER->TIMERAWL;
  se_start(&g_se, &out, g_capture_speed == CaptureSpeed_Full, g_keep_all, g_text, g_last_status,
           sizeof(g_ring), TX_SIZE);
  g_state = State_Starting;
  capture_command('s');
}

//-----------------------------------------------------------------------------
static void stop_streaming(void)
{
  if (g_state != State_Starting && g_state != State_Running)
    return;

  g_state = State_Stopping;
  capture_command('p');
}

//-----------------------------------------------------------------------------
static void command(int cmd)
{
  if (cmd == 'h' || cmd == '?')
    print_help();
  else if (cmd == 'v')
    out_text(SE_NAME " " SE_VERSION " protocol 1\r\n");
  else if (cmd == 's')
    start_streaming();
  else if (cmd == 'p')
    stop_streaming();
  else if (cmd == 'k')
  {
    g_keep_all = !g_keep_all;
    g_se.keep_all = g_keep_all; // takes effect at once, also while streaming
    setting_changed("Keep idle traffic", g_keep_all ? "Enabled" : "Disabled");
  }
  else if (cmd == 'e' || cmd == 'o')
  {
    if (g_state != State_Idle)
      out_text("Stop streaming first (p)\r\n");
    else if (cmd == 'e')
    {
      g_capture_speed = (g_capture_speed + 1) % CaptureSpeedCount;
      setting_changed("Capture speed", capture_speed_str[g_capture_speed]);
    }
    else
    {
      g_text = !g_text;
      setting_changed("Output format", g_text ? "Text" : "Binary");
    }
  }
}

/*- Streaming ---------------------------------------------------------------*/

//-----------------------------------------------------------------------------
static void stream_task(void)
{
  int reply;

  if (g_state == State_Idle)
    return;

  reply = capture_reply();

  if (reply == 'S')
  {
    g_core1_started = true; // the ring is cleared: g_cap.wr is valid from now on
    if (g_state == State_Starting)
      g_state = State_Running;
  }
  else if (reply == 'P')
  {
    g_core1_stopped = true;
  }

  if (!g_core1_started)
    return;

  for (int i = 0; i < CONSUME_BUDGET && tx_space(0) >= TX_HEADROOM; i++)
  {
    uint32_t wr = g_cap.wr;
    uint32_t used = wr - g_rd;

    if (0 == used)
      break;

    if (used > g_ring_peak)
      g_ring_peak = used;

    __DMB();
    g_rd = se_consume(&g_se, g_ring, CAP_MASK, g_rd);
    __DMB();
    g_cap.rd = g_rd;
  }

  if (g_se.errors != g_last_errors)
  {
    g_last_errors = g_se.errors;
    set_error(true);
  }

  uint32_t now = TIMER->TIMERAWL;

  if (g_state == State_Running && (now - g_last_status) >= STATUS_PERIOD)
  {
    g_last_status = now;
    g_se.ring_peak = (g_ring_peak * 100 + CAP_RING_WORDS - 1) / CAP_RING_WORDS;
    g_se.out_peak = ((uint32_t)tx_peak * 100 + TX_SIZE - 1) / TX_SIZE;
    g_ring_peak = 0;
    tx_peak = tx_used;
    se_status(&g_se, now, SE_STATUS);
  }

  if (g_state == State_Stopping && g_core1_stopped && g_rd == g_cap.wr && tx_space(0) >= TX_HEADROOM)
  {
    if (g_cap.lost)
      se_lost(&g_se, g_cap.lost);
    g_se.ring_peak = (g_ring_peak * 100 + CAP_RING_WORDS - 1) / CAP_RING_WORDS;
    g_se.out_peak = ((uint32_t)tx_peak * 100 + TX_SIZE - 1) / TX_SIZE;
    se_status(&g_se, now, SE_END);
    g_state = State_Idle;
  }
}

/*- USB ---------------------------------------------------------------------*/

//-----------------------------------------------------------------------------
void usb_cdc_line_coding_updated(usb_cdc_line_coding_t *line_coding)
{
  (void)line_coding;
}

//-----------------------------------------------------------------------------
void usb_cdc_control_line_state_update(int line_state)
{
  app_vcp_open = (line_state & USB_CDC_CTRL_SIGNAL_DTE_PRESENT);

  if (!app_vcp_open)
  {
    stop_streaming(); // nobody is listening
    return;
  }

  tx_clear();
  print_help();

  if (!app_recv_pending)
  {
    app_recv_pending = true;
    usb_cdc_recv(app_recv_buffer, sizeof(app_recv_buffer));
  }
}

//-----------------------------------------------------------------------------
static char lower(char c)
{
  if ('A' <= c && c <= 'Z')
    return c - ('A' - 'a');
  return c;
}

//-----------------------------------------------------------------------------
void usb_cdc_recv_callback(int size)
{
  app_recv_pending = false;

  if (!app_vcp_open)
    return;

  for (int i = 0; i < size; i++)
    command(lower(app_recv_buffer[i]));

  app_recv_pending = true;
  usb_cdc_recv(app_recv_buffer, sizeof(app_recv_buffer));
}

//-----------------------------------------------------------------------------
bool usb_class_handle_request(usb_request_t *request)
{
  if (usb_cdc_handle_request(request))
    return true;
  else
    return false;
}

//-----------------------------------------------------------------------------
void usb_configuration_callback(int config)
{
  (void)config;
}

//-----------------------------------------------------------------------------
void set_error(bool error)
{
  HAL_GPIO_LED_R_write(error);
}

//-----------------------------------------------------------------------------
int main(void)
{
  sys_init();
  timer_init();
  usb_init();
  usb_cdc_init();
  serial_number_init();
  capture_init();

  HAL_GPIO_LED_O_out();
  HAL_GPIO_LED_O_clr();

  HAL_GPIO_LED_R_out();
  HAL_GPIO_LED_R_clr();

  while (1)
  {
    usb_task();
    stream_task();
    tx_task();
    status_timer_task();
  }

  return 0;
}
