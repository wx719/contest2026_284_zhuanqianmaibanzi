/****************************************************************************
 * apps/examples/velapoka/velapoka_state.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <syslog.h>

#include "velapoka_state.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool velapoka_state_allowed(enum velapoka_state_e current,
                                   enum velapoka_state_e next)
{
  if (next == VELAPOKA_STATE_ERROR || next == VELAPOKA_STATE_IDLE)
    {
      return true;
    }

  switch (current)
    {
      case VELAPOKA_STATE_STARTUP:
        return next == VELAPOKA_STATE_SELF_TEST;
      case VELAPOKA_STATE_SELF_TEST:
        return next == VELAPOKA_STATE_IDLE;
      case VELAPOKA_STATE_IDLE:
        return next == VELAPOKA_STATE_SAMPLE_CAPTURE ||
               next == VELAPOKA_STATE_READY;
      case VELAPOKA_STATE_SAMPLE_CAPTURE:
        return next == VELAPOKA_STATE_SAVE_SAMPLE;
      case VELAPOKA_STATE_SAVE_SAMPLE:
        return next == VELAPOKA_STATE_READY;
      case VELAPOKA_STATE_READY:
        return next == VELAPOKA_STATE_CAPTURE ||
               next == VELAPOKA_STATE_SAMPLE_CAPTURE;
      case VELAPOKA_STATE_PASS:
      case VELAPOKA_STATE_FAIL:
        return next == VELAPOKA_STATE_SAVE_RESULT;
      case VELAPOKA_STATE_SAVE_RESULT:
        return next == VELAPOKA_STATE_READY;
      case VELAPOKA_STATE_CAPTURE:
        return next == VELAPOKA_STATE_PREPROCESS;
      case VELAPOKA_STATE_PREPROCESS:
        return next == VELAPOKA_STATE_INSPECT;
      case VELAPOKA_STATE_INSPECT:
        return next == VELAPOKA_STATE_PASS ||
               next == VELAPOKA_STATE_FAIL;
      case VELAPOKA_STATE_ERROR:
        return next == VELAPOKA_STATE_SAMPLE_CAPTURE ||
               next == VELAPOKA_STATE_READY;
      default:
        return false;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void velapoka_state_init(FAR struct velapoka_state_s *state)
{
  state->current = VELAPOKA_STATE_STARTUP;
  syslog(LOG_INFO, "VelaPoka state: STARTUP\n");
}

int velapoka_state_transition(FAR struct velapoka_state_s *state,
                              enum velapoka_state_e next)
{
  FAR const char *before;
  FAR const char *after;

  if (state == NULL || next > VELAPOKA_STATE_ERROR)
    {
      return -EINVAL;
    }

  if (state->current == next)
    {
      return OK;
    }

  before = velapoka_state_name(state->current);
  after = velapoka_state_name(next);
  if (!velapoka_state_allowed(state->current, next))
    {
      syslog(LOG_ERR, "VelaPoka state: invalid %s -> %s\n",
             before, after);
      return -EPERM;
    }

  state->current = next;
  syslog(LOG_INFO, "VelaPoka state: %s -> %s\n", before, after);
  return OK;
}

FAR const char *velapoka_state_name(enum velapoka_state_e state)
{
  static FAR const char * const names[] =
  {
    "STARTUP", "SELF_TEST", "IDLE", "SAMPLE_CAPTURE", "SAVE_SAMPLE",
    "READY", "CAPTURE", "PREPROCESS", "INSPECT", "PASS", "FAIL",
    "SAVE_RESULT", "ERROR"
  };

  if (state > VELAPOKA_STATE_ERROR)
    {
      return "UNKNOWN";
    }

  return names[state];
}
