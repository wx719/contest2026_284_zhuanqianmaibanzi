# ##############################################################################
# contest2026_284_zhuanqianmaibanzi/app/velapoka/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_EXAMPLES_VELAPOKA)
  nuttx_add_application(
    NAME
    velapoka
    PRIORITY
    ${CONFIG_EXAMPLES_VELAPOKA_PRIORITY}
    STACKSIZE
    ${CONFIG_EXAMPLES_VELAPOKA_STACKSIZE}
    MODULE
    ${CONFIG_EXAMPLES_VELAPOKA}
    DEPENDS
    lvgl
    SRCS
    velapoka_camera.c
    velapoka_inspect.c
    velapoka_main.c
    velapoka_network.c
    velapoka_state.c
    velapoka_storage.c
    velapoka_ui.c)
endif()
