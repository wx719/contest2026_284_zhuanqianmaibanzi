/****************************************************************************
 * apps/examples/velapoka/velapoka_state.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_VELAPOKA_VELAPOKA_STATE_H
#define __APPS_EXAMPLES_VELAPOKA_VELAPOKA_STATE_H

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum velapoka_state_e
{
  VELAPOKA_STATE_STARTUP = 0,
  VELAPOKA_STATE_SELF_TEST,
  VELAPOKA_STATE_IDLE,
  VELAPOKA_STATE_SAMPLE_CAPTURE,
  VELAPOKA_STATE_SAVE_SAMPLE,
  VELAPOKA_STATE_READY,
  VELAPOKA_STATE_CAPTURE,
  VELAPOKA_STATE_PREPROCESS,
  VELAPOKA_STATE_INSPECT,
  VELAPOKA_STATE_PASS,
  VELAPOKA_STATE_FAIL,
  VELAPOKA_STATE_SAVE_RESULT,
  VELAPOKA_STATE_ERROR
};

struct velapoka_state_s
{
  enum velapoka_state_e current;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void velapoka_state_init(FAR struct velapoka_state_s *state);
int velapoka_state_transition(FAR struct velapoka_state_s *state,
                              enum velapoka_state_e next);
FAR const char *velapoka_state_name(enum velapoka_state_e state);

#endif /* __APPS_EXAMPLES_VELAPOKA_VELAPOKA_STATE_H */
