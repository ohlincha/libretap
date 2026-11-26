// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022, Alex Taradov <alex@taradov.com>. All rights reserved.
// Changed for LibreTap's USB Sniffer Stream (2026): core 1 captures continuously into the capture ring
// (capture_ring.h) instead of filling the buffer once; decoding, filtering and output moved to core 0 (stream.c,
// main.c). The PIO programs and the pin setup are usb-sniffer-lite's, unchanged. The trigger input, the capture
// limit and the buffer display are gone.

/*- Includes ----------------------------------------------------------------*/
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rp2040.h"
#include "hal_gpio.h"
#include "pio_asm.h"
#include "capture.h"
#include "capture_ring.h"
#include "globals.h"

/*- Definitions -------------------------------------------------------------*/
#define CORE1_STACK_SIZE       512 // words

// DP and DM can be any pins, but they must be consequitive and in that order
#define DP_INDEX       10
#define DM_INDEX       11
#define START_INDEX    12

HAL_GPIO_PIN(DP,       0, 10, pio0_10)
HAL_GPIO_PIN(DM,       0, 11, pio0_11)
HAL_GPIO_PIN(START,    0, 12, pio1_12) // Internal trigger from PIO1 to PIO0

/*- Variables ---------------------------------------------------------------*/
uint32_t g_ring[CAP_RING_WORDS];
cap_t g_cap = { .ring = g_ring };

int g_capture_speed = CaptureSpeed_Full;

/*- Implementations ---------------------------------------------------------*/

//-----------------------------------------------------------------------------
static int poll_cmd(void)
{
  if (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk)
    return SIO->FIFO_RD;
  return 0;
}

//-----------------------------------------------------------------------------
// Runs until core 0 sends 'p'; answers with 'P' once the PIO is stopped and everything captured is in the ring.
static void capture_stream(void)
{
  volatile uint32_t *PIO0_INSTR_MEM = (volatile uint32_t *)&PIO0->INSTR_MEM0;
  volatile uint32_t *PIO1_INSTR_MEM = (volatile uint32_t *)&PIO1->INSTR_MEM0;
  bool fs;

  HAL_GPIO_DP_init();
  HAL_GPIO_DM_init();
  HAL_GPIO_START_init();

  RESETS_SET->RESET = RESETS_RESET_pio0_Msk | RESETS_RESET_pio1_Msk;
  RESETS_CLR->RESET = RESETS_RESET_pio0_Msk | RESETS_RESET_pio1_Msk;
  while (0 == RESETS->RESET_DONE_b.pio0 && 0 == RESETS->RESET_DONE_b.pio1);

  fs = (g_capture_speed == CaptureSpeed_Full);

  static const uint16_t pio0_ops[] =
  {
    // idle:
    /* 0 */  OP_MOV | MOV_DST_X | MOV_SRC_NULL | MOV_OP_INVERT,   // Reset the bit counter
    /* 1 */  OP_WAIT | WAIT_POL_1 | WAIT_SRC_PIN | WAIT_INDEX(0), // Wait until the bus goes idle
    /* 2 */  OP_WAIT | WAIT_POL_0 | WAIT_SRC_PIN | WAIT_INDEX(0), // Wait for the SOP

    // start0:
    /* 3 */  OP_NOP | OP_DELAY(1), // Skip to the middle of the bit

    // read0:
    /* 4 */  OP_JMP | JMP_COND_X_NZ_PD | JMP_ADDR(5/*next*/),  // Decrement the bit counter
    /* 5 */  OP_IN  | IN_SRC_PINS | IN_CNT(1), // Sample D+
    /* 6 */  OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV, // Sample D+ and D-
    /* 7 */  OP_OUT | OUT_DST_Y | OUT_CNT(2),
    /* 8 */  OP_JMP | JMP_COND_Y_ZERO | JMP_ADDR(21/*eop*/), // If both are 0, then it is an EOP
    /* 9 */  OP_NOP | OP_DELAY(3), // Skip to the middle of the bit
    /* 10 */ OP_JMP | JMP_COND_PIN | JMP_ADDR(4/*read0*/), // If D- is high, then D+ is low, read 0

    // read1:
    /* 11 */ OP_JMP | JMP_COND_X_NZ_PD | JMP_ADDR(12/*next*/), // Decrement the bit counter
    /* 12 */ OP_IN  | IN_SRC_PINS | IN_CNT(1), // Sample D+
    /* 13 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV, // Sample D+ and D-
    /* 14 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
    /* 15 */ OP_JMP | JMP_COND_Y_ZERO | JMP_ADDR(21/*eop*/), // If both are 0, then it is an EOP
    /* 16 */ OP_JMP | JMP_COND_PIN | JMP_ADDR(3/*start0*/),  // Look for a low to high transition on
    /* 17 */ OP_JMP | JMP_COND_PIN | JMP_ADDR(3/*start0*/),  // D- to adjust the sample point location
    /* 18 */ OP_JMP | JMP_COND_PIN | JMP_ADDR(3/*start0*/),
    /* 19 */ OP_JMP | JMP_COND_PIN | JMP_ADDR(3/*start0*/),
    /* 20 */ OP_JMP | JMP_ADDR(11/*read1*/),

    // eop:
    /* 21 */ OP_PUSH, // Transfer the last data
    /* 22 */ OP_MOV | MOV_DST_ISR | MOV_SRC_X, // Transfer the bit count
    /* 23 */ OP_PUSH,

    // poll_reset:
    /* 24 */ OP_SET | SET_DST_X | SET_DATA(31),

    // poll_loop:
    /* 25 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV, // Sample D+ and D-
    /* 26 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
    /* 27 */ OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(0/*idle*/), // If either is not zero, back to idle
    /* 28 */ OP_JMP | JMP_COND_X_NZ_PD | JMP_ADDR(25/*poll_loop*/),
    /* 29 */ OP_MOV | MOV_DST_ISR | MOV_SRC_NULL | MOV_OP_INVERT,
    /* 30 */ OP_PUSH,
    // Wrap to 0 from here

    // Entry point, wait for a START signal from the PIO1
    /* 31 */ OP_WAIT | WAIT_POL_1 | WAIT_SRC_PIN | WAIT_INDEX(2),
  };

  static const uint16_t pio1_ops[] =
  {
    /* 0 */  OP_NOP | OP_DELAY(31), // Wait for the PIO0 to start
    /* 1 */  OP_NOP | OP_DELAY(31),
    /* 2 */  OP_NOP | OP_DELAY(31),
    /* 3 */  OP_NOP | OP_DELAY(31),

    // wait_se0:
    /* 4 */  OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV,
    /* 5 */  OP_OUT | OUT_DST_Y | OUT_CNT(2),
    /* 6 */  OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(4/*wait_se0*/),

    /* 7 */  OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV,
    /* 8 */  OP_OUT | OUT_DST_Y | OUT_CNT(2),
    /* 9 */  OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(4/*wait_se0*/),

    /* 10 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV,
    /* 11 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
    /* 12 */ OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(4/*wait_se0*/),

    /* 13 */ OP_MOV | MOV_DST_OSR | MOV_SRC_PINS | MOV_OP_BIT_REV,
    /* 14 */ OP_OUT | OUT_DST_Y | OUT_CNT(2),
    /* 15 */ OP_JMP | JMP_COND_Y_NZ_PD | JMP_ADDR(4/*wait_se0*/),

    /* 16 */ OP_SET | SET_DST_PINS | SET_DATA(1), // Set the START output
    /* 17 */ OP_JMP | JMP_ADDR(17/*self*/), // Infinite loop
  };

  // PIO0 init
  PIO0->SM0_CLKDIV = ((fs ? 1 : 8) << PIO0_SM0_CLKDIV_INT_Pos);

  for (int i = 0; i < (int)(sizeof(pio0_ops)/sizeof(uint16_t)); i++)
    PIO0_INSTR_MEM[i] = pio0_ops[i];

  if (!fs)
  {
    PIO0_INSTR_MEM[1] = OP_WAIT | WAIT_POL_1 | WAIT_SRC_PIN | WAIT_INDEX(1);
    PIO0_INSTR_MEM[2] = OP_WAIT | WAIT_POL_0 | WAIT_SRC_PIN | WAIT_INDEX(1);
  }

  PIO0->SM0_EXECCTRL = ((fs ? DM_INDEX : DP_INDEX) << PIO0_SM0_EXECCTRL_JMP_PIN_Pos) |
      (30 << PIO0_SM0_EXECCTRL_WRAP_TOP_Pos) | (0 << PIO0_SM0_EXECCTRL_WRAP_BOTTOM_Pos);

  PIO0->SM0_SHIFTCTRL = PIO0_SM0_SHIFTCTRL_FJOIN_RX_Msk | PIO0_SM0_SHIFTCTRL_AUTOPUSH_Msk |
      (31 << PIO0_SM0_SHIFTCTRL_PUSH_THRESH_Pos);

  PIO0->SM0_PINCTRL = (DP_INDEX << PIO0_SM0_PINCTRL_IN_BASE_Pos);

  PIO0->SM0_INSTR = OP_JMP | JMP_ADDR(31);

  // PIO1 init
  PIO1->SM0_CLKDIV = ((fs ? 1 : 8) << PIO0_SM0_CLKDIV_INT_Pos);

  for (int i = 0; i < (int)(sizeof(pio1_ops)/sizeof(uint16_t)); i++)
    PIO1_INSTR_MEM[i] = pio1_ops[i];

  PIO1->SM0_EXECCTRL  = (31 << PIO0_SM0_EXECCTRL_WRAP_TOP_Pos) | (0 << PIO0_SM0_EXECCTRL_WRAP_BOTTOM_Pos);
  PIO1->SM0_SHIFTCTRL = 0;
  PIO1->SM0_PINCTRL   = (DP_INDEX << PIO0_SM0_PINCTRL_IN_BASE_Pos) |
      (START_INDEX << PIO0_SM0_PINCTRL_SET_BASE_Pos) | (1 << PIO0_SM0_PINCTRL_SET_COUNT_Pos);

  PIO1->SM0_INSTR = OP_SET | SET_DST_PINDIRS | SET_DATA(1); // Clear the START output
  PIO1->SM0_INSTR = OP_SET | SET_DST_PINS    | SET_DATA(0);

  cap_reset(&g_cap); // core 0 waits for 'S' before it reads the ring

  while (0 == (SIO->FIFO_ST & SIO_FIFO_ST_RDY_Msk));
  SIO->FIFO_WR = 'S';

  PIO1_SET->CTRL = (1 << (PIO0_CTRL_SM_ENABLE_Pos + 0));
  PIO0_SET->CTRL = (1 << (PIO0_CTRL_SM_ENABLE_Pos + 0));

  while (1)
  {
    if (0 == (PIO0->FSTAT & (1 << (PIO0_FSTAT_RXEMPTY_Pos + 0))))
      cap_word(&g_cap, PIO0->RXF0);

    if (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk)
    {
      if (SIO->FIFO_RD == 'p')
        break;
    }
  }

  PIO0_CLR->CTRL = (1 << (PIO0_CTRL_SM_ENABLE_Pos + 0));
  PIO1_CLR->CTRL = (1 << (PIO0_CTRL_SM_ENABLE_Pos + 0));

  while (0 == (SIO->FIFO_ST & SIO_FIFO_ST_RDY_Msk));
  SIO->FIFO_WR = 'P';
}

//-----------------------------------------------------------------------------
static void core1_main(void)
{
  while (1)
  {
    if (poll_cmd() == 's')
      capture_stream();
  }
}

//-----------------------------------------------------------------------------
static void core1_start(void)
{
  static uint32_t core1_stack[CORE1_STACK_SIZE];
  uint32_t *stack_ptr = core1_stack + CORE1_STACK_SIZE;
  const uint32_t cmd[] = { 0, 1, (uint32_t)SCB->VTOR, (uint32_t)stack_ptr, (uint32_t)core1_main };

  while (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk)
    (void)SIO->FIFO_RD;

  __SEV();

  while (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk)
    (void)SIO->FIFO_RD;

  for (int i = 0; i < (int)(sizeof(cmd) / sizeof(uint32_t)); i++)
  {
    SIO->FIFO_WR = cmd[i];
    __SEV();

    while (0 == (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk));
    (void)SIO->FIFO_RD;
  }
}

//-----------------------------------------------------------------------------
void capture_init(void)
{
  core1_start();
}

//-----------------------------------------------------------------------------
void capture_command(int cmd)
{
  while (0 == (SIO->FIFO_ST & SIO_FIFO_ST_RDY_Msk));
  SIO->FIFO_WR = cmd;
}

//-----------------------------------------------------------------------------
// Core 1's answer to 's' ('S') or 'p' ('P'), 0 if none yet
int capture_reply(void)
{
  if (SIO->FIFO_ST & SIO_FIFO_ST_VLD_Msk)
    return SIO->FIFO_RD;
  return 0;
}
