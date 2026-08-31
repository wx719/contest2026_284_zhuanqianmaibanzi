/****************************************************************************
 * apps/examples/velapoka/velapoka_inspect.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "velapoka_camera.h"
#include "velapoka_inspect.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VELAPOKA_ENROLL_FRAMES       3
#define VELAPOKA_BLOCK_DIFF_PERCENT  12
#define VELAPOKA_BLOCK_COLUMNS \
  (VELAPOKA_ROI_WIDTH / VELAPOKA_INSPECT_BLOCK_SIZE)
#define VELAPOKA_BLOCK_ROWS \
  (VELAPOKA_ROI_HEIGHT / VELAPOKA_INSPECT_BLOCK_SIZE)
#define VELAPOKA_BLOCK_COUNT \
  (VELAPOKA_BLOCK_COLUMNS * VELAPOKA_BLOCK_ROWS)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct velapoka_component_s
{
  uint8_t min_x;
  uint8_t min_y;
  uint8_t max_x;
  uint8_t max_y;
  uint8_t blocks;
};

struct velapoka_inspector_s
{
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  FAR uint16_t *enroll_sum;
  FAR uint8_t *reference;
  FAR uint8_t *current;
  FAR uint8_t *normalized;
  struct velapoka_result_s result;
  uint32_t sequence;
  unsigned int threshold_percent;
  unsigned int enroll_count;
  bool reference_valid;
  bool pending;
  bool busy;
  bool result_ready;
  bool stop;
  bool thread_started;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void velapoka_inspect_scale(FAR const uint8_t *preview,
                                   FAR uint8_t *output)
{
  unsigned int x;
  unsigned int y;

  for (y = 0; y < VELAPOKA_INSPECT_HEIGHT; y++)
    {
      unsigned int sy = y * VELAPOKA_CAMERA_PREVIEW_HEIGHT /
                        VELAPOKA_INSPECT_HEIGHT;
      FAR const uint8_t *src =
        preview + sy * VELAPOKA_CAMERA_PREVIEW_WIDTH;
      FAR uint8_t *dst = output + y * VELAPOKA_INSPECT_WIDTH;

      for (x = 0; x < VELAPOKA_INSPECT_WIDTH; x++)
        {
          unsigned int sx = x * VELAPOKA_CAMERA_PREVIEW_WIDTH /
                            VELAPOKA_INSPECT_WIDTH;
          dst[x] = src[sx];
        }
    }
}

static uint32_t velapoka_elapsed_ms(FAR const struct timespec *start,
                                    FAR const struct timespec *end)
{
  int64_t milliseconds;

  milliseconds = (int64_t)(end->tv_sec - start->tv_sec) * 1000;
  milliseconds += (end->tv_nsec - start->tv_nsec) / 1000000;
  return milliseconds > 0 ? (uint32_t)milliseconds : 0;
}

static void velapoka_sort_components(
  FAR struct velapoka_component_s *components, unsigned int count)
{
  unsigned int i;
  unsigned int j;

  for (i = 0; i < count; i++)
    {
      for (j = i + 1; j < count; j++)
        {
          if (components[j].blocks > components[i].blocks)
            {
              struct velapoka_component_s temporary = components[i];
              components[i] = components[j];
              components[j] = temporary;
            }
        }
    }
}

static void velapoka_find_components(
  FAR const bool *abnormal, FAR struct velapoka_result_s *result,
  FAR unsigned int *largest_blocks)
{
  struct velapoka_component_s components[VELAPOKA_BLOCK_COUNT];
  bool visited[VELAPOKA_BLOCK_COUNT];
  uint8_t queue[VELAPOKA_BLOCK_COUNT];
  unsigned int component_count = 0;
  unsigned int index;

  memset(visited, 0, sizeof(visited));
  *largest_blocks = 0;

  for (index = 0; index < VELAPOKA_BLOCK_COUNT; index++)
    {
      struct velapoka_component_s component;
      unsigned int head;
      unsigned int tail;

      if (!abnormal[index] || visited[index])
        {
          continue;
        }

      component.min_x = component.max_x = index % VELAPOKA_BLOCK_COLUMNS;
      component.min_y = component.max_y = index / VELAPOKA_BLOCK_COLUMNS;
      component.blocks = 0;
      head = 0;
      tail = 0;
      queue[tail++] = index;
      visited[index] = true;

      while (head < tail)
        {
          unsigned int current = queue[head++];
          unsigned int block_x = current % VELAPOKA_BLOCK_COLUMNS;
          unsigned int block_y = current / VELAPOKA_BLOCK_COLUMNS;
          int dx;
          int dy;

          component.blocks++;
          if (block_x < component.min_x)
            {
              component.min_x = block_x;
            }

          if (block_x > component.max_x)
            {
              component.max_x = block_x;
            }

          if (block_y < component.min_y)
            {
              component.min_y = block_y;
            }

          if (block_y > component.max_y)
            {
              component.max_y = block_y;
            }

          for (dy = -1; dy <= 1; dy++)
            {
              for (dx = -1; dx <= 1; dx++)
                {
                  int neighbor_x = (int)block_x + dx;
                  int neighbor_y = (int)block_y + dy;
                  unsigned int neighbor;

                  if ((dx == 0 && dy == 0) || neighbor_x < 0 ||
                      neighbor_y < 0 ||
                      neighbor_x >= VELAPOKA_BLOCK_COLUMNS ||
                      neighbor_y >= VELAPOKA_BLOCK_ROWS)
                    {
                      continue;
                    }

                  neighbor = neighbor_y * VELAPOKA_BLOCK_COLUMNS +
                             neighbor_x;
                  if (abnormal[neighbor] && !visited[neighbor])
                    {
                      visited[neighbor] = true;
                      queue[tail++] = neighbor;
                    }
                }
            }
        }

      components[component_count++] = component;
      if (component.blocks > *largest_blocks)
        {
          *largest_blocks = component.blocks;
        }
    }

  velapoka_sort_components(components, component_count);
  result->box_count = component_count < VELAPOKA_INSPECT_MAX_BOXES ?
                      component_count : VELAPOKA_INSPECT_MAX_BOXES;
  for (index = 0; index < result->box_count; index++)
    {
      FAR struct velapoka_box_s *box = &result->boxes[index];
      FAR const struct velapoka_component_s *component =
        &components[index];

      box->x = VELAPOKA_ROI_X +
               component->min_x * VELAPOKA_INSPECT_BLOCK_SIZE;
      box->y = VELAPOKA_ROI_Y +
               component->min_y * VELAPOKA_INSPECT_BLOCK_SIZE;
      box->width = (component->max_x - component->min_x + 1) *
                   VELAPOKA_INSPECT_BLOCK_SIZE;
      box->height = (component->max_y - component->min_y + 1) *
                    VELAPOKA_INSPECT_BLOCK_SIZE;
    }
}

static void velapoka_inspect_run(FAR struct velapoka_inspector_s *inspector,
                                 FAR struct velapoka_result_s *result)
{
  bool abnormal[VELAPOKA_BLOCK_COUNT];
  struct timespec started;
  struct timespec finished;
  uint64_t reference_sum = 0;
  uint64_t current_sum = 0;
  uint32_t gain_q12;
  unsigned int abnormal_count = 0;
  unsigned int largest_blocks;
  unsigned int block_x;
  unsigned int block_y;
  unsigned int x;
  unsigned int y;

  clock_gettime(CLOCK_MONOTONIC, &started);
  memset(result, 0, sizeof(*result));
  result->sequence = inspector->sequence;

  for (y = VELAPOKA_ROI_Y;
       y < VELAPOKA_ROI_Y + VELAPOKA_ROI_HEIGHT; y++)
    {
      for (x = VELAPOKA_ROI_X;
           x < VELAPOKA_ROI_X + VELAPOKA_ROI_WIDTH; x++)
        {
          unsigned int offset = y * VELAPOKA_INSPECT_WIDTH + x;
          reference_sum += inspector->reference[offset];
          current_sum += inspector->current[offset];
        }
    }

  if (current_sum == 0)
    {
      current_sum = 1;
    }

  gain_q12 = (reference_sum << 12) / current_sum;
  if (gain_q12 > (8 << 12))
    {
      gain_q12 = 8 << 12;
    }

  for (y = 0; y < VELAPOKA_INSPECT_HEIGHT; y++)
    {
      for (x = 0; x < VELAPOKA_INSPECT_WIDTH; x++)
        {
          unsigned int offset = y * VELAPOKA_INSPECT_WIDTH + x;
          uint32_t value = ((uint32_t)inspector->current[offset] *
                            gain_q12 + (1 << 11)) >> 12;
          inspector->normalized[offset] =
            value > UINT8_MAX ? UINT8_MAX : (uint8_t)value;
        }
    }

  for (block_y = 0; block_y < VELAPOKA_BLOCK_ROWS; block_y++)
    {
      for (block_x = 0; block_x < VELAPOKA_BLOCK_COLUMNS; block_x++)
        {
          uint32_t absolute_sum = 0;
          uint32_t edge_sum = 0;
          uint32_t pixels = 0;
          uint32_t edge_pixels = 0;
          uint32_t metric;
          unsigned int start_x = VELAPOKA_ROI_X +
                                 block_x * VELAPOKA_INSPECT_BLOCK_SIZE;
          unsigned int start_y = VELAPOKA_ROI_Y +
                                 block_y * VELAPOKA_INSPECT_BLOCK_SIZE;
          unsigned int index = block_y * VELAPOKA_BLOCK_COLUMNS + block_x;

          for (y = start_y;
               y < start_y + VELAPOKA_INSPECT_BLOCK_SIZE; y++)
            {
              for (x = start_x;
                   x < start_x + VELAPOKA_INSPECT_BLOCK_SIZE; x++)
                {
                  unsigned int offset = y * VELAPOKA_INSPECT_WIDTH + x;
                  int difference = inspector->normalized[offset] -
                                   inspector->reference[offset];

                  absolute_sum += abs(difference);
                  pixels++;
                  if (x + 1 < start_x + VELAPOKA_INSPECT_BLOCK_SIZE &&
                      y + 1 < start_y + VELAPOKA_INSPECT_BLOCK_SIZE)
                    {
                      unsigned int right = offset + 1;
                      unsigned int down = offset + VELAPOKA_INSPECT_WIDTH;
                      int current_edge =
                        abs(inspector->normalized[right] -
                            inspector->normalized[offset]) +
                        abs(inspector->normalized[down] -
                            inspector->normalized[offset]);
                      int reference_edge =
                        abs(inspector->reference[right] -
                            inspector->reference[offset]) +
                        abs(inspector->reference[down] -
                            inspector->reference[offset]);

                      edge_sum += abs(current_edge - reference_edge) / 2;
                      edge_pixels++;
                    }
                }
            }

          metric = (3 * (absolute_sum / pixels) +
                    (edge_pixels > 0 ? edge_sum / edge_pixels : 0)) / 4;
          abnormal[index] = metric * 100 >=
                            VELAPOKA_BLOCK_DIFF_PERCENT * UINT8_MAX;
          if (abnormal[index])
            {
              abnormal_count++;
            }
        }
    }

  result->difference_tenths = abnormal_count * 1000 /
                              VELAPOKA_BLOCK_COUNT;
  result->score_tenths = 1000 - result->difference_tenths;
  velapoka_find_components(abnormal, result, &largest_blocks);
  result->pass = result->difference_tenths <=
                   inspector->threshold_percent * 10 &&
                 largest_blocks * 1000 / VELAPOKA_BLOCK_COUNT <=
                   inspector->threshold_percent * 5;

  clock_gettime(CLOCK_MONOTONIC, &finished);
  result->elapsed_ms = velapoka_elapsed_ms(&started, &finished);
}

static FAR void *velapoka_inspect_thread(FAR void *arg)
{
  FAR struct velapoka_inspector_s *inspector = arg;

  for (; ; )
    {
      struct velapoka_result_s result;

      pthread_mutex_lock(&inspector->lock);
      while (!inspector->pending && !inspector->stop)
        {
          pthread_cond_wait(&inspector->cond, &inspector->lock);
        }

      if (inspector->stop)
        {
          pthread_mutex_unlock(&inspector->lock);
          break;
        }

      inspector->pending = false;
      pthread_mutex_unlock(&inspector->lock);

      velapoka_inspect_run(inspector, &result);

      pthread_mutex_lock(&inspector->lock);
      inspector->result = result;
      inspector->result_ready = true;
      inspector->busy = false;
      pthread_mutex_unlock(&inspector->lock);
    }

  return NULL;
}

static void velapoka_inspect_cleanup(
  FAR struct velapoka_inspector_s *inspector)
{
  free(inspector->normalized);
  free(inspector->current);
  free(inspector->reference);
  free(inspector->enroll_sum);
  pthread_cond_destroy(&inspector->cond);
  pthread_mutex_destroy(&inspector->lock);
  free(inspector);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velapoka_inspect_start(FAR struct velapoka_inspector_s **inspector_out)
{
  FAR struct velapoka_inspector_s *inspector;
  pthread_attr_t attr;
  int ret;

  if (inspector_out == NULL)
    {
      return -EINVAL;
    }

  *inspector_out = NULL;
  inspector = calloc(1, sizeof(*inspector));
  if (inspector == NULL)
    {
      return -ENOMEM;
    }

  ret = pthread_mutex_init(&inspector->lock, NULL);
  if (ret != 0)
    {
      free(inspector);
      return -ret;
    }

  ret = pthread_cond_init(&inspector->cond, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&inspector->lock);
      free(inspector);
      return -ret;
    }

  inspector->enroll_sum = calloc(VELAPOKA_INSPECT_SIZE,
                                 sizeof(*inspector->enroll_sum));
  inspector->reference = malloc(VELAPOKA_INSPECT_SIZE);
  inspector->current = malloc(VELAPOKA_INSPECT_SIZE);
  inspector->normalized = malloc(VELAPOKA_INSPECT_SIZE);
  if (inspector->enroll_sum == NULL || inspector->reference == NULL ||
      inspector->current == NULL || inspector->normalized == NULL)
    {
      velapoka_inspect_cleanup(inspector);
      return -ENOMEM;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr,
                            CONFIG_EXAMPLES_VELAPOKA_CAMERA_STACKSIZE);
  ret = pthread_create(&inspector->thread, &attr,
                       velapoka_inspect_thread, inspector);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      velapoka_inspect_cleanup(inspector);
      return -ret;
    }

  inspector->thread_started = true;
  *inspector_out = inspector;
  return OK;
}

void velapoka_inspect_stop(FAR struct velapoka_inspector_s *inspector)
{
  if (inspector == NULL)
    {
      return;
    }

  pthread_mutex_lock(&inspector->lock);
  inspector->stop = true;
  pthread_cond_signal(&inspector->cond);
  pthread_mutex_unlock(&inspector->lock);

  if (inspector->thread_started)
    {
      pthread_join(inspector->thread, NULL);
    }

  velapoka_inspect_cleanup(inspector);
}

int velapoka_inspect_enroll_begin(FAR struct velapoka_inspector_s *inspector)
{
  if (inspector == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&inspector->lock);
  if (inspector->busy)
    {
      pthread_mutex_unlock(&inspector->lock);
      return -EBUSY;
    }

  inspector->enroll_count = 0;
  inspector->reference_valid = false;
  inspector->result_ready = false;
  memset(inspector->enroll_sum, 0,
         VELAPOKA_INSPECT_SIZE * sizeof(*inspector->enroll_sum));
  pthread_mutex_unlock(&inspector->lock);
  return OK;
}

int velapoka_inspect_enroll_frame(
  FAR struct velapoka_inspector_s *inspector,
  FAR const uint8_t *preview, size_t size,
  FAR unsigned int *captured, FAR bool *ready)
{
  unsigned int i;

  if (inspector == NULL || preview == NULL || captured == NULL ||
      ready == NULL || size != VELAPOKA_CAMERA_PREVIEW_SIZE)
    {
      return -EINVAL;
    }

  velapoka_inspect_scale(preview, inspector->current);
  pthread_mutex_lock(&inspector->lock);
  if (inspector->enroll_count >= VELAPOKA_ENROLL_FRAMES)
    {
      pthread_mutex_unlock(&inspector->lock);
      return -EALREADY;
    }

  for (i = 0; i < VELAPOKA_INSPECT_SIZE; i++)
    {
      inspector->enroll_sum[i] += inspector->current[i];
    }

  inspector->enroll_count++;
  if (inspector->enroll_count == VELAPOKA_ENROLL_FRAMES)
    {
      for (i = 0; i < VELAPOKA_INSPECT_SIZE; i++)
        {
          inspector->reference[i] =
            (inspector->enroll_sum[i] + VELAPOKA_ENROLL_FRAMES / 2) /
            VELAPOKA_ENROLL_FRAMES;
        }

      inspector->reference_valid = true;
    }

  *captured = inspector->enroll_count;
  *ready = inspector->reference_valid;
  pthread_mutex_unlock(&inspector->lock);
  return OK;
}

bool velapoka_inspect_reference_ready(
  FAR struct velapoka_inspector_s *inspector)
{
  bool ready;

  if (inspector == NULL)
    {
      return false;
    }

  pthread_mutex_lock(&inspector->lock);
  ready = inspector->reference_valid;
  pthread_mutex_unlock(&inspector->lock);
  return ready;
}

int velapoka_inspect_reference_export(
  FAR struct velapoka_inspector_s *inspector,
  FAR uint8_t *reference, size_t size)
{
  if (inspector == NULL || reference == NULL ||
      size != VELAPOKA_INSPECT_SIZE)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&inspector->lock);
  if (!inspector->reference_valid)
    {
      pthread_mutex_unlock(&inspector->lock);
      return -ENOENT;
    }

  memcpy(reference, inspector->reference, size);
  pthread_mutex_unlock(&inspector->lock);
  return OK;
}

int velapoka_inspect_reference_import(
  FAR struct velapoka_inspector_s *inspector,
  FAR const uint8_t *reference, size_t size)
{
  if (inspector == NULL || reference == NULL ||
      size != VELAPOKA_INSPECT_SIZE)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&inspector->lock);
  if (inspector->busy)
    {
      pthread_mutex_unlock(&inspector->lock);
      return -EBUSY;
    }

  memcpy(inspector->reference, reference, size);
  inspector->reference_valid = true;
  inspector->enroll_count = VELAPOKA_ENROLL_FRAMES;
  inspector->result_ready = false;
  pthread_mutex_unlock(&inspector->lock);
  return OK;
}

int velapoka_inspect_submit(FAR struct velapoka_inspector_s *inspector,
                            FAR const uint8_t *preview, size_t size,
                            uint32_t sequence,
                            unsigned int threshold_percent)
{
  if (inspector == NULL || preview == NULL ||
      size != VELAPOKA_CAMERA_PREVIEW_SIZE || threshold_percent > 100)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&inspector->lock);
  if (!inspector->reference_valid)
    {
      pthread_mutex_unlock(&inspector->lock);
      return -ENOENT;
    }

  if (inspector->busy)
    {
      pthread_mutex_unlock(&inspector->lock);
      return -EBUSY;
    }

  inspector->busy = true;
  inspector->result_ready = false;
  pthread_mutex_unlock(&inspector->lock);

  velapoka_inspect_scale(preview, inspector->current);

  pthread_mutex_lock(&inspector->lock);
  inspector->sequence = sequence;
  inspector->threshold_percent = threshold_percent;
  inspector->pending = true;
  pthread_cond_signal(&inspector->cond);
  pthread_mutex_unlock(&inspector->lock);
  return OK;
}

int velapoka_inspect_poll(FAR struct velapoka_inspector_s *inspector,
                          FAR struct velapoka_result_s *result)
{
  int ret = 0;

  if (inspector == NULL || result == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&inspector->lock);
  if (inspector->result_ready)
    {
      *result = inspector->result;
      inspector->result_ready = false;
      ret = 1;
    }

  pthread_mutex_unlock(&inspector->lock);
  return ret;
}

int velapoka_inspect_snapshot(FAR struct velapoka_inspector_s *inspector,
                              FAR uint8_t *normalized, size_t size)
{
  if (inspector == NULL || normalized == NULL ||
      size != VELAPOKA_INSPECT_SIZE)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&inspector->lock);
  if (inspector->busy)
    {
      pthread_mutex_unlock(&inspector->lock);
      return -EBUSY;
    }

  memcpy(normalized, inspector->normalized, size);
  pthread_mutex_unlock(&inspector->lock);
  return OK;
}

void velapoka_inspect_discard_result(
  FAR struct velapoka_inspector_s *inspector)
{
  if (inspector == NULL)
    {
      return;
    }

  pthread_mutex_lock(&inspector->lock);
  inspector->result_ready = false;
  pthread_mutex_unlock(&inspector->lock);
}
