/****************************************************************************
 * contest2026_284_zhuanqianmaibanzi/board/contest_board/chips/esp32p4/
 * espressif/esp_allocateheap.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <sys/types.h>

#include <arch/board/board.h>
#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/mm/mm.h>

#include "riscv_internal.h"
#include "rom/rom_layout.h"

#ifdef CONFIG_ESPRESSIF_SPIRAM
#  include "esp_psram.h"
#  include "esp_private/esp_psram_extram.h"
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
#if defined(CONFIG_MM_KERNEL_HEAP) && \
    defined(CONFIG_ESPRESSIF_SPIRAM_USER_HEAP)
  uintptr_t ubase;
  uintptr_t utop;
#endif

  board_autoled_on(LED_HEAPALLOCATE);

#if defined(CONFIG_MM_KERNEL_HEAP) && \
    defined(CONFIG_ESPRESSIF_SPIRAM_USER_HEAP)
  DEBUGASSERT(esp_psram_is_initialized());

  ubase = esp_psram_extram_vaddr_start();
  utop  = esp_psram_extram_vaddr_end();

  DEBUGASSERT(utop > ubase);

  *heap_start = (void *)ubase;
  *heap_size  = utop - ubase;
#else
  uintptr_t ubase = g_idle_topstack;
  uintptr_t utop =
    (uintptr_t)ets_rom_layout_p->dram0_rtos_reserved_start;

#  ifdef CONFIG_MM_KERNEL_HEAP
  DEBUGASSERT(utop > ubase + CONFIG_MM_KERNEL_HEAPSIZE);
  ubase += CONFIG_MM_KERNEL_HEAPSIZE;
#  endif

  *heap_start = (void *)ubase;
  *heap_size  = utop - ubase;
#endif
}

#ifdef CONFIG_MM_KERNEL_HEAP
void up_allocate_kheap(void **heap_start, size_t *heap_size)
{
  uintptr_t kbase = g_idle_topstack;
  uintptr_t ktop  = (uintptr_t)ets_rom_layout_p->dram0_rtos_reserved_start;

  DEBUGASSERT(ktop > kbase);

  board_autoled_on(LED_HEAPALLOCATE);

  *heap_start = (void *)kbase;
#ifdef CONFIG_ESPRESSIF_SPIRAM_USER_HEAP
  *heap_size  = ktop - kbase;
#else
  DEBUGASSERT(ktop > kbase + CONFIG_MM_KERNEL_HEAPSIZE);
  *heap_size  = CONFIG_MM_KERNEL_HEAPSIZE;
#endif
}
#endif

#if CONFIG_MM_REGIONS > 1
void riscv_addregion(void)
{
#if !defined(CONFIG_MM_KERNEL_HEAP) && \
    defined(CONFIG_ESPRESSIF_SPIRAM_USER_HEAP)
  if (esp_psram_is_initialized())
    {
      uintptr_t start = esp_psram_extram_vaddr_start();
      uintptr_t end   = esp_psram_extram_vaddr_end();

      if (end > start)
        {
          kumm_addregion((void *)start, end - start);
        }
    }
#endif
}
#endif
