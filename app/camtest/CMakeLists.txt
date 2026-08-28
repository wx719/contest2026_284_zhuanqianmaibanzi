# ##############################################################################
# apps/examples/camtest/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_EXAMPLES_CAMTEST)
  nuttx_add_application(
    MODULE
    ${CONFIG_EXAMPLES_CAMTEST}
    NAME
    ${CONFIG_EXAMPLES_CAMTEST_PROGNAME}
    SRCS
    camtest_main.c
    STACKSIZE
    ${CONFIG_EXAMPLES_CAMTEST_STACKSIZE}
    PRIORITY
    ${CONFIG_EXAMPLES_CAMTEST_PRIORITY})
endif()
