/****************************************************************************
 * arch/risc-v/src/esp32p4/espressif/esp_cam_intr_compat.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include "esp_intr_alloc.h"
#include "platform/os.h"

esp_err_t esp_intr_alloc_intrstatus(int source, int flags,
                                    uint32_t status_reg,
                                    uint32_t status_mask,
                                    intr_handler_t handler,
                                    FAR void *arg,
                                    FAR intr_handle_t *ret_handle)
{
  return esp_os_intr_alloc_intrstatus(source, flags, status_reg,
                                      status_mask, handler, arg,
                                      ret_handle);
}

esp_err_t esp_intr_free(intr_handle_t handle)
{
  return esp_os_intr_free(handle);
}
