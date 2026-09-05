/****************************************************************************
 * boards/risc-v/esp32p4/contest2026_284_board/src/freertos/semphr.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal FreeRTOS semaphore compatibility required by Espressif's SDMMC
 * upper-half.  The board port otherwise uses native NuttX primitives.
 ****************************************************************************/

#ifndef __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_FREERTOS_SEMPHR_H
#define __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_FREERTOS_SEMPHR_H

#include <nuttx/config.h>

#include <semaphore.h>
#include <stdlib.h>

#include <nuttx/semaphore.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef FAR sem_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t
xSemaphoreCreateWithCount(unsigned int value)
{
  FAR sem_t *sem = malloc(sizeof(*sem));

  if (sem != NULL && nxsem_init(sem, 0, value) < 0)
    {
      free(sem);
      sem = NULL;
    }

  return sem;
}

static inline SemaphoreHandle_t
xSemaphoreCreateBinaryWithCaps(uint32_t caps)
{
  return xSemaphoreCreateWithCount(0);
}

static inline SemaphoreHandle_t
xSemaphoreCreateMutexWithCaps(uint32_t caps)
{
  return xSemaphoreCreateWithCount(1);
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem,
                                        TickType_t ticks)
{
  int ret;

  if (ticks == 0)
    {
      ret = nxsem_trywait(sem);
    }
  else if (ticks == portMAX_DELAY)
    {
      ret = nxsem_wait_uninterruptible(sem);
    }
  else
    {
      ret = nxsem_tickwait_uninterruptible(sem, ticks);
    }

  return ret < 0 ? pdFALSE : pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
  return nxsem_post(sem) < 0 ? pdFALSE : pdTRUE;
}

static inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem,
                                               FAR int *hptw)
{
  if (hptw != NULL)
    {
      *hptw = pdFALSE;
    }

  return xSemaphoreGive(sem);
}

static inline void vSemaphoreDelete(SemaphoreHandle_t sem)
{
  if (sem != NULL)
    {
      nxsem_destroy(sem);
      free(sem);
    }
}

static inline void vSemaphoreDeleteWithCaps(SemaphoreHandle_t sem)
{
  vSemaphoreDelete(sem);
}

/* The IDF SDMMC driver uses an int wake flag even though the OS adapter's
 * queue function takes BaseType_t.  Keep that compatibility detail local.
 */

#define xQueueSendFromISR(queue, item, hptw) \
  xQueueSendFromISR(queue, item, (FAR BaseType_t *)(hptw))

#endif /* __BOARDS_RISCV_ESP32P4_CONTEST_BOARD_SRC_FREERTOS_SEMPHR_H */
