/*
 * Copyright (c) 2019-2020, University of Southampton and Contributors.
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* ------ Includes ----------------------------------------------------------*/
#include <math.h>
#include <msp430fr5994.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <support.h>

/* ------ Macros ------------------------------------------------------------*/

/* ------ Extern functions --------------------------------------------------*/

/* ------ Function Prototypes -----------------------------------------------*/
void assert(bool c);

/* ------ Variable Declarations ---------------------------------------------*/

/* ------ Function Declarations ---------------------------------------------*/

void __attribute__((__interrupt__(DMA_VECTOR), optimize("O1"))) dma_isr(void) {}

void dma_copy(char* src, char* dst, const int len) {
  // Blocking block-transfer, autoincrement, 2-byte word size
  DMA0CTL = DMADT_1 | DMASRCINCR_3 | DMADSTINCR_3;
  DMA0SA = src;
  DMA0DA = dst;
  DMA0SZ = len / 2;
  DMA0CTL |= DMAEN | DMAREQ;
  while (DMA0CTL & DMAEN)
    ;             // Wait for transfer complete
  if (len % 2) {  // transfer last byte
    *(dst + len - 1) = *(src + len - 1);
  }
}

static int a[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
static int b[sizeof(a)] = {0};

int main(void) {
  while (1) {
    dma_copy((char*)a, (char*)b, sizeof(a));

    for (size_t i = 0; i < sizeof(a) / sizeof(int); i++) {
      assert(b[i] == a[i]);
    }

    end_experiment();
  }
}

void assert(bool c) {
  if (!c) {
    indicate_test_fail();
    while (1)
      ;  // stall
  }
}
