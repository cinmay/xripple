#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_WATER_HEIGHT 255
#define RIPPLE_VERTICAL_SPEED 0.09
#define RIPPLE_HORIZONTAL_SPEED 0.06
#define RIPPLE_VERTICAL_PERIOD 48.0
#define RIPPLE_HORIZONTAL_PERIOD 64.0
#define RIPPLE_VERTICAL_AMPLITUDE 7.0
#define RIPPLE_HORIZONTAL_AMPLITUDE 10.0

struct app_state;
struct output_state;

enum capture_kind {
  CAPTURE_KIND_SCANLINE,
  CAPTURE_KIND_PREFILL,
};

struct shared_buffer {
  struct wl_buffer *buffer;
  void *data;
  size_t size;
  int width;
  int height;
  int stride;
  uint32_t format;
  bool busy;
};

struct capture_frame {
  struct output_state *output;
  struct zwlr_screencopy_frame_v1 *frame;
  struct shared_buffer buffer;
  enum capture_kind kind;
  uint32_t format;
  int width;
  int height;
  int stride;
  int ring_row_index;
  bool has_shm_format;
  bool copy_started;
  struct timespec started_at;
};

struct output_state {
  struct app_state *app;
  struct wl_output *output;
  uint32_t registry_name;
  uint32_t output_version;
  char *name;

  int mode_width;
  int mode_height;
  int scale;

  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct wl_callback *frame_callback;

  int surface_width;
  int surface_height;
  int ring_row_count;
  int capture_row_index;

  uint32_t *ring_pixels;
  struct shared_buffer render_buffers[2];

  bool configured;
  bool capture_in_flight;
  bool closed;
  bool logged_capture_geometry;
  bool prefilled;

  double vertical_phase;
  double horizontal_phase;

  uint64_t frames_rendered;
  uint64_t captures_completed;
  uint64_t captures_failed;
  uint64_t dropped_frames;
  uint64_t total_render_ns;
  uint64_t total_capture_ns;

  struct output_state *next;
};

struct app_state {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct zwlr_screencopy_manager_v1 *screencopy_manager;
  uint32_t screencopy_version;

  struct output_state *outputs;

  int requested_water_height;
  uint64_t max_frames;
  uint64_t total_frames_rendered;
  bool verbose;
  bool benchmark;
  bool running;
};

static volatile sig_atomic_t should_exit = 0;

static void handle_signal(int signal_number)
{
  (void)signal_number;
  should_exit = 1;
}

static uint32_t min_u32(uint32_t left, uint32_t right)
{
  return left < right ? left : right;
}

static int clamp_int(int value, int minimum, int maximum)
{
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

static uint64_t monotonic_ns(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static uint64_t elapsed_ns_since(struct timespec started_at)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t start_ns = (uint64_t)started_at.tv_sec * 1000000000ull + (uint64_t)started_at.tv_nsec;
  uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
  return now_ns >= start_ns ? now_ns - start_ns : 0;
}

static bool parse_positive_int(const char *text, int *out_value)
{
  char *end = NULL;
  errno = 0;
  long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > 100000) {
    return false;
  }
  *out_value = (int)value;
  return true;
}

static bool parse_positive_u64(const char *text, uint64_t *out_value)
{
  char *end = NULL;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value == 0) {
    return false;
  }
  *out_value = (uint64_t)value;
  return true;
}

static void print_usage(const char *program_name)
{
  fprintf(stderr,
          "usage: %s [--water-height N] [--max-frames N] [--benchmark] [--verbose]\n"
          "\n"
          "  --water-height N   ripple band height in logical pixels, default %d\n"
          "  --max-frames N     exit after N rendered frames across all outputs\n"
          "  --benchmark        print benchmark stats on exit\n"
          "  --verbose          print setup and diagnostic messages\n",
          program_name,
          DEFAULT_WATER_HEIGHT);
}

static bool parse_arguments(struct app_state *app, int argc, char **argv)
{
  for (int index = 1; index < argc; index++) {
    if (strcmp(argv[index], "--water-height") == 0) {
      if (++index >= argc || !parse_positive_int(argv[index], &app->requested_water_height)) {
        fprintf(stderr, "invalid --water-height value\n");
        return false;
      }
    } else if (strcmp(argv[index], "--max-frames") == 0) {
      if (++index >= argc || !parse_positive_u64(argv[index], &app->max_frames)) {
        fprintf(stderr, "invalid --max-frames value\n");
        return false;
      }
    } else if (strcmp(argv[index], "--benchmark") == 0) {
      app->benchmark = true;
      if (app->max_frames == 0) {
        app->max_frames = 600;
      }
    } else if (strcmp(argv[index], "--verbose") == 0) {
      app->verbose = true;
    } else if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
      print_usage(argv[0]);
      exit(0);
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[index]);
      return false;
    }
  }
  return true;
}

static int create_shared_file(size_t size)
{
  int file_descriptor = -1;

#ifdef __linux__
  file_descriptor = memfd_create("hypripple-shm", MFD_CLOEXEC);
  if (file_descriptor >= 0) {
    if (ftruncate(file_descriptor, (off_t)size) == 0) {
      return file_descriptor;
    }
    close(file_descriptor);
    return -1;
  }
#endif

  char path[] = "/tmp/hypripple-shm-XXXXXX";
  file_descriptor = mkstemp(path);
  if (file_descriptor < 0) {
    return -1;
  }
  unlink(path);

  if (ftruncate(file_descriptor, (off_t)size) != 0) {
    close(file_descriptor);
    return -1;
  }

  return file_descriptor;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
  (void)buffer;
  struct shared_buffer *shared_buffer = data;
  shared_buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
  .release = buffer_release,
};

static void destroy_shared_buffer(struct shared_buffer *shared_buffer)
{
  if (shared_buffer->buffer != NULL) {
    wl_buffer_destroy(shared_buffer->buffer);
  }
  if (shared_buffer->data != NULL && shared_buffer->size > 0) {
    munmap(shared_buffer->data, shared_buffer->size);
  }
  memset(shared_buffer, 0, sizeof(*shared_buffer));
}

static bool create_shared_buffer_with_stride(struct app_state *app,
                                             struct shared_buffer *shared_buffer,
                                             int width,
                                             int height,
                                             int stride,
                                             uint32_t format)
{
  destroy_shared_buffer(shared_buffer);

  if (width <= 0 || height <= 0 || stride < width * 4) {
    return false;
  }

  size_t size = (size_t)stride * (size_t)height;
  int file_descriptor = create_shared_file(size);
  if (file_descriptor < 0) {
    perror("create shared memory file");
    return false;
  }

  void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, 0);
  if (data == MAP_FAILED) {
    perror("mmap shared memory");
    close(file_descriptor);
    return false;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, file_descriptor, (int)size);
  struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
  wl_shm_pool_destroy(pool);
  close(file_descriptor);

  shared_buffer->buffer = buffer;
  shared_buffer->data = data;
  shared_buffer->size = size;
  shared_buffer->width = width;
  shared_buffer->height = height;
  shared_buffer->stride = stride;
  shared_buffer->format = format;
  shared_buffer->busy = false;
  wl_buffer_add_listener(buffer, &buffer_listener, shared_buffer);

  return true;
}

static bool create_shared_buffer(struct app_state *app,
                                 struct shared_buffer *shared_buffer,
                                 int width,
                                 int height,
                                 uint32_t format)
{
  return create_shared_buffer_with_stride(app, shared_buffer, width, height, width * 4, format);
}

static void destroy_capture_frame(struct capture_frame *capture)
{
  if (capture == NULL) {
    return;
  }
  if (capture->frame != NULL) {
    zwlr_screencopy_frame_v1_destroy(capture->frame);
  }
  destroy_shared_buffer(&capture->buffer);
  free(capture);
}

static void free_output(struct output_state *output)
{
  if (output == NULL) {
    return;
  }
  if (output->frame_callback != NULL) {
    wl_callback_destroy(output->frame_callback);
  }
  if (output->layer_surface != NULL) {
    zwlr_layer_surface_v1_destroy(output->layer_surface);
  }
  if (output->surface != NULL) {
    wl_surface_destroy(output->surface);
  }
  if (output->output != NULL) {
    wl_output_destroy(output->output);
  }
  destroy_shared_buffer(&output->render_buffers[0]);
  destroy_shared_buffer(&output->render_buffers[1]);
  free(output->ring_pixels);
  free(output->name);
  free(output);
}

static int estimate_output_logical_height(struct output_state *output)
{
  if (output->mode_width > 0 && output->mode_height > 0 && output->surface_width > 0) {
    double width_scale = (double)output->mode_width / (double)output->surface_width;
    if (width_scale > 0.0) {
      int logical_height = (int)llround((double)output->mode_height / width_scale);
      if (logical_height > 0) {
        return logical_height;
      }
    }
  }

  if (output->mode_height > 0 && output->scale > 0) {
    return output->mode_height / output->scale;
  }

  return output->mode_height;
}

static bool ensure_output_storage(struct output_state *output, int width, int height)
{
  if (width <= 0 || height <= 0) {
    return false;
  }

  bool size_changed = output->surface_width != width || output->surface_height != height;
  if (!size_changed && output->ring_pixels != NULL) {
    return true;
  }

  output->surface_width = width;
  output->surface_height = height;
  output->ring_row_count = height * 2;
  output->capture_row_index = 0;

  free(output->ring_pixels);
  output->ring_pixels = calloc((size_t)output->ring_row_count * (size_t)width, sizeof(uint32_t));
  if (output->ring_pixels == NULL) {
    perror("allocate ripple ring buffer");
    return false;
  }

  if (!create_shared_buffer(output->app, &output->render_buffers[0], width, height, WL_SHM_FORMAT_XRGB8888)) {
    return false;
  }
  if (!create_shared_buffer(output->app, &output->render_buffers[1], width, height, WL_SHM_FORMAT_XRGB8888)) {
    return false;
  }

  if (output->app->verbose) {
    int logical_height = estimate_output_logical_height(output);
    fprintf(stderr,
            "configured output %s surface %dx%d, mode %dx%d, estimated logical height %d, source ring rows %d\n",
            output->name != NULL ? output->name : "(unnamed)",
            output->surface_width,
            output->surface_height,
            output->mode_width,
            output->mode_height,
            logical_height,
            output->ring_row_count);
  }

  return true;
}

static struct shared_buffer *next_render_buffer(struct output_state *output)
{
  for (int index = 0; index < 2; index++) {
    if (!output->render_buffers[index].busy) {
      return &output->render_buffers[index];
    }
  }
  return NULL;
}

static void request_capture(struct output_state *output);

static void frame_done(void *data, struct wl_callback *callback, uint32_t callback_data)
{
  (void)callback_data;
  struct output_state *output = data;
  wl_callback_destroy(callback);
  output->frame_callback = NULL;

  if (should_exit || output->closed) {
    return;
  }

  request_capture(output);
}

static const struct wl_callback_listener frame_listener = {
  .done = frame_done,
};

static void commit_render_buffer(struct output_state *output, struct shared_buffer *buffer)
{
  buffer->busy = true;
  output->frame_callback = wl_surface_frame(output->surface);
  wl_callback_add_listener(output->frame_callback, &frame_listener, output);
  wl_surface_attach(output->surface, buffer->buffer, 0, 0);
  wl_surface_damage(output->surface, 0, 0, output->surface_width, output->surface_height);
  wl_surface_commit(output->surface);
}

static void schedule_frame_without_render(struct output_state *output)
{
  if (output->frame_callback != NULL || output->surface == NULL) {
    return;
  }

  output->frame_callback = wl_surface_frame(output->surface);
  wl_callback_add_listener(output->frame_callback, &frame_listener, output);
  wl_surface_commit(output->surface);
}

static void render_ripple(struct output_state *output)
{
  struct shared_buffer *buffer = next_render_buffer(output);
  if (buffer == NULL) {
    output->dropped_frames++;
    schedule_frame_without_render(output);
    return;
  }

  uint64_t started_ns = monotonic_ns();
  int width = output->surface_width;
  int height = output->surface_height;
  int ring_rows = output->ring_row_count;

  output->vertical_phase += RIPPLE_VERTICAL_SPEED;
  if (output->vertical_phase > M_PI * 2.0) {
    output->vertical_phase = 0.0;
  }

  output->horizontal_phase += RIPPLE_HORIZONTAL_SPEED;
  if (output->horizontal_phase > M_PI * 2.0) {
    output->horizontal_phase = 0.0;
  }

  for (int y = 0; y < height; y++) {
    double progress = ((double)(height - y)) / (double)height;
    double vertical_angle = progress * progress * RIPPLE_VERTICAL_PERIOD + output->vertical_phase;
    int vertical_offset = y + (int)(sin(vertical_angle) * RIPPLE_VERTICAL_AMPLITUDE) + 1;
    int source_row = ring_rows - vertical_offset;
    source_row = clamp_int(source_row, 0, ring_rows - 1);

    double horizontal_angle = progress * progress * RIPPLE_HORIZONTAL_PERIOD + output->horizontal_phase;
    int horizontal_offset = (int)(sin(horizontal_angle) * RIPPLE_HORIZONTAL_AMPLITUDE * (1.0 - progress));

    const uint32_t *source_pixels = output->ring_pixels + (size_t)source_row * (size_t)width;
    uint8_t *destination_bytes = (uint8_t *)buffer->data + (size_t)y * (size_t)buffer->stride;
    uint32_t *destination_row = (uint32_t *)destination_bytes;
    for (int x = 0; x < width; x++) {
      int source_x = clamp_int(x - horizontal_offset, 0, width - 1);
      destination_row[x] = source_pixels[source_x] | 0xff000000u;
    }
  }

  output->total_render_ns += monotonic_ns() - started_ns;
  output->frames_rendered++;
  output->app->total_frames_rendered++;

  commit_render_buffer(output, buffer);

  if (output->app->max_frames > 0 && output->app->total_frames_rendered >= output->app->max_frames) {
    should_exit = 1;
  }
}

static bool capture_format_is_supported(uint32_t format)
{
  return format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888;
}

static void copy_row_to_ring(struct output_state *output,
                             int destination_row,
                             const uint32_t *source_row,
                             int source_width)
{
  if (output->ring_pixels == NULL || source_row == NULL || output->surface_width <= 0 ||
      destination_row < 0 || destination_row >= output->ring_row_count) {
    return;
  }

  int copy_width = source_width < output->surface_width ? source_width : output->surface_width;
  if (copy_width <= 0) {
    return;
  }

  uint32_t *destination_pixels = output->ring_pixels + (size_t)destination_row * (size_t)output->surface_width;

  for (int x = 0; x < copy_width; x++) {
    destination_pixels[x] = source_row[x] | 0xff000000u;
  }
  for (int x = copy_width; x < output->surface_width; x++) {
    destination_pixels[x] = destination_pixels[copy_width - 1];
  }
}

static void copy_capture_to_ring(struct capture_frame *capture)
{
  struct output_state *output = capture->output;
  if (output->ring_pixels == NULL || capture->buffer.data == NULL || output->surface_width <= 0) {
    return;
  }

  if (capture->kind == CAPTURE_KIND_PREFILL) {
    int rows_to_copy = capture->height < output->ring_row_count ? capture->height : output->ring_row_count;
    for (int row = 0; row < rows_to_copy; row++) {
      uint8_t *source_bytes = (uint8_t *)capture->buffer.data + (size_t)row * (size_t)capture->buffer.stride;
      copy_row_to_ring(output, row, (const uint32_t *)source_bytes, capture->width);
    }

    if (rows_to_copy > 0 && rows_to_copy < output->ring_row_count) {
      uint32_t *last_row = output->ring_pixels + (size_t)(rows_to_copy - 1) * (size_t)output->surface_width;
      for (int row = rows_to_copy; row < output->ring_row_count; row++) {
        uint32_t *destination = output->ring_pixels + (size_t)row * (size_t)output->surface_width;
        memcpy(destination, last_row, (size_t)output->surface_width * sizeof(uint32_t));
      }
    }

    output->capture_row_index = 0;
    output->prefilled = true;
    return;
  }

  uint8_t *source_bytes = capture->buffer.data;
  copy_row_to_ring(output, capture->ring_row_index, (const uint32_t *)source_bytes, capture->width);

  output->capture_row_index = (output->capture_row_index + 1) % output->ring_row_count;
}

static void start_capture_copy(struct capture_frame *capture)
{
  if (capture->copy_started) {
    return;
  }
  if (!capture->has_shm_format || capture->width <= 0 || capture->height <= 0) {
    return;
  }
  if (!capture_format_is_supported(capture->format)) {
    fprintf(stderr, "unsupported screencopy format: 0x%x\n", capture->format);
    capture->output->captures_failed++;
    capture->output->capture_in_flight = false;
    render_ripple(capture->output);
    destroy_capture_frame(capture);
    return;
  }
  int capture_stride = capture->stride > 0 ? capture->stride : capture->width * 4;
  if (!create_shared_buffer_with_stride(capture->output->app,
                                        &capture->buffer,
                                        capture->width,
                                        capture->height,
                                        capture_stride,
                                        capture->format)) {
    capture->output->captures_failed++;
    capture->output->capture_in_flight = false;
    render_ripple(capture->output);
    destroy_capture_frame(capture);
    return;
  }

  capture->copy_started = true;
  zwlr_screencopy_frame_v1_copy(capture->frame, capture->buffer.buffer);
}

static void capture_handle_buffer(void *data,
                                  struct zwlr_screencopy_frame_v1 *frame,
                                  uint32_t format,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t stride)
{
  (void)frame;
  struct capture_frame *capture = data;
  capture->format = format;
  capture->width = (int)width;
  capture->height = (int)height;
  capture->stride = (int)stride;
  capture->has_shm_format = true;

  if (capture->output->app->screencopy_version < 3) {
    start_capture_copy(capture);
  }
}

static void capture_handle_flags(void *data,
                                 struct zwlr_screencopy_frame_v1 *frame,
                                 uint32_t flags)
{
  (void)data;
  (void)frame;
  (void)flags;
}

static void capture_handle_ready(void *data,
                                 struct zwlr_screencopy_frame_v1 *frame,
                                 uint32_t tv_sec_hi,
                                 uint32_t tv_sec_lo,
                                 uint32_t tv_nsec)
{
  (void)frame;
  (void)tv_sec_hi;
  (void)tv_sec_lo;
  (void)tv_nsec;

  struct capture_frame *capture = data;
  struct output_state *output = capture->output;
  output->captures_completed++;
  output->total_capture_ns += elapsed_ns_since(capture->started_at);
  output->capture_in_flight = false;

  copy_capture_to_ring(capture);
  render_ripple(output);
  destroy_capture_frame(capture);
}

static void capture_handle_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
  (void)frame;
  struct capture_frame *capture = data;
  struct output_state *output = capture->output;
  output->captures_failed++;
  output->total_capture_ns += elapsed_ns_since(capture->started_at);
  output->capture_in_flight = false;

  if (output->app->verbose) {
    fprintf(stderr,
            "screencopy failed for output %s\n",
            output->name != NULL ? output->name : "(unnamed)");
  }

  render_ripple(output);
  destroy_capture_frame(capture);
}

static void capture_handle_damage(void *data,
                                  struct zwlr_screencopy_frame_v1 *frame,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t width,
                                  uint32_t height)
{
  (void)data;
  (void)frame;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
}

static void capture_handle_linux_dmabuf(void *data,
                                        struct zwlr_screencopy_frame_v1 *frame,
                                        uint32_t format,
                                        uint32_t width,
                                        uint32_t height)
{
  (void)data;
  (void)frame;
  (void)format;
  (void)width;
  (void)height;
}

static void capture_handle_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
  (void)frame;
  struct capture_frame *capture = data;
  start_capture_copy(capture);
}

static const struct zwlr_screencopy_frame_v1_listener capture_listener = {
  .buffer = capture_handle_buffer,
  .flags = capture_handle_flags,
  .ready = capture_handle_ready,
  .failed = capture_handle_failed,
  .damage = capture_handle_damage,
  .linux_dmabuf = capture_handle_linux_dmabuf,
  .buffer_done = capture_handle_buffer_done,
};

static bool compute_capture_geometry(struct output_state *output,
                                     int *out_source_top,
                                     int *out_source_height,
                                     int *out_capture_width,
                                     int *out_logical_height)
{
  int logical_height = estimate_output_logical_height(output);
  if (logical_height <= 0) {
    return false;
  }

  int source_top = logical_height - output->surface_height * 3;
  if (source_top < 0) {
    source_top = 0;
  }

  int source_height = output->ring_row_count;
  int available_height = logical_height - source_top;
  if (source_height > available_height) {
    source_height = available_height;
  }
  if (source_height <= 0) {
    return false;
  }

  *out_source_top = source_top;
  *out_source_height = source_height;
  *out_capture_width = output->surface_width;
  *out_logical_height = logical_height;
  return true;
}

static bool begin_capture_region(struct output_state *output,
                                 enum capture_kind kind,
                                 int ring_row_index,
                                 int source_y,
                                 int capture_width,
                                 int capture_height)
{
  if (output->capture_in_flight || output->app->screencopy_manager == NULL ||
      output->output == NULL || output->mode_width <= 0 || output->mode_height <= 0 ||
      output->surface_width <= 0 || output->surface_height <= 0) {
    return false;
  }

  struct capture_frame *capture = calloc(1, sizeof(*capture));
  if (capture == NULL) {
    perror("allocate capture frame");
    return false;
  }

  clock_gettime(CLOCK_MONOTONIC, &capture->started_at);
  capture->output = output;
  capture->kind = kind;
  capture->ring_row_index = ring_row_index;
  capture->frame = zwlr_screencopy_manager_v1_capture_output_region(output->app->screencopy_manager,
                                                                     0,
                                                                     output->output,
                                                                     0,
                                                                     source_y,
                                                                     capture_width,
                                                                     capture_height);
  zwlr_screencopy_frame_v1_add_listener(capture->frame, &capture_listener, capture);
  output->capture_in_flight = true;
  return true;
}

static bool request_prefill_capture(struct output_state *output)
{
  int source_top = 0;
  int source_height = 0;
  int capture_width = 0;
  int logical_height = 0;
  if (!compute_capture_geometry(output, &source_top, &source_height, &capture_width, &logical_height)) {
    return false;
  }

  if (output->app->verbose && !output->logged_capture_geometry) {
    int source_bottom = source_top + source_height - 1;
    fprintf(stderr,
            "capture output %s logical rows %d..%d of %d, width %d\n",
            output->name != NULL ? output->name : "(unnamed)",
            source_top,
            source_bottom,
            logical_height,
            capture_width);
    output->logged_capture_geometry = true;
  }

  return begin_capture_region(output, CAPTURE_KIND_PREFILL, 0, source_top, capture_width, source_height);
}

static void request_capture(struct output_state *output)
{
  int source_top = 0;
  int source_height = 0;
  int capture_width = 0;
  int logical_height = 0;
  if (!compute_capture_geometry(output, &source_top, &source_height, &capture_width, &logical_height)) {
    render_ripple(output);
    return;
  }

  int ring_row_index = output->capture_row_index;
  int source_row = source_top + (ring_row_index % source_height);
  source_row = clamp_int(source_row, 0, logical_height - 1);

  if (!begin_capture_region(output,
                            CAPTURE_KIND_SCANLINE,
                            ring_row_index,
                            source_row,
                            capture_width,
                            1)) {
    render_ripple(output);
  }
}

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *layer_surface,
                                    uint32_t serial,
                                    uint32_t width,
                                    uint32_t height)
{
  struct output_state *output = data;
  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

  int configured_width = width > 0 ? (int)width : output->mode_width;
  int configured_height = height > 0 ? (int)height : output->app->requested_water_height;
  if (configured_width <= 0 || configured_height <= 0) {
    return;
  }

  if (!ensure_output_storage(output, configured_width, configured_height)) {
    should_exit = 1;
    return;
  }

  output->configured = true;

  if (!output->prefilled && request_prefill_capture(output)) {
    return;
  }

  render_ripple(output);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
  (void)layer_surface;
  struct output_state *output = data;
  output->closed = true;
  should_exit = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
  .configure = layer_surface_configure,
  .closed = layer_surface_closed,
};

static void output_handle_geometry(void *data,
                                   struct wl_output *output,
                                   int32_t x,
                                   int32_t y,
                                   int32_t physical_width,
                                   int32_t physical_height,
                                   int32_t subpixel,
                                   const char *make,
                                   const char *model,
                                   int32_t transform)
{
  (void)data;
  (void)output;
  (void)x;
  (void)y;
  (void)physical_width;
  (void)physical_height;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;
}

static void output_handle_mode(void *data,
                               struct wl_output *output,
                               uint32_t flags,
                               int32_t width,
                               int32_t height,
                               int32_t refresh)
{
  (void)output;
  (void)refresh;
  struct output_state *output_state = data;
  if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
    output_state->mode_width = width;
    output_state->mode_height = height;
  }
}

static void output_handle_done(void *data, struct wl_output *output)
{
  (void)data;
  (void)output;
}

static void output_handle_scale(void *data, struct wl_output *output, int32_t factor)
{
  (void)output;
  struct output_state *output_state = data;
  output_state->scale = factor > 0 ? factor : 1;
}

static void output_handle_name(void *data, struct wl_output *output, const char *name)
{
  (void)output;
  struct output_state *output_state = data;
  free(output_state->name);
  output_state->name = strdup(name);
}

static void output_handle_description(void *data, struct wl_output *output, const char *description)
{
  (void)data;
  (void)output;
  (void)description;
}

static const struct wl_output_listener output_listener = {
  .geometry = output_handle_geometry,
  .mode = output_handle_mode,
  .done = output_handle_done,
  .scale = output_handle_scale,
  .name = output_handle_name,
  .description = output_handle_description,
};

static void create_layer_surface_for_output(struct output_state *output)
{
  struct app_state *app = output->app;
  output->surface = wl_compositor_create_surface(app->compositor);

  struct wl_region *empty_input_region = wl_compositor_create_region(app->compositor);
  wl_surface_set_input_region(output->surface, empty_input_region);
  wl_region_destroy(empty_input_region);

  output->layer_surface = zwlr_layer_shell_v1_get_layer_surface(app->layer_shell,
                                                                output->surface,
                                                                output->output,
                                                                ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
                                                                "hypripple");
  zwlr_layer_surface_v1_add_listener(output->layer_surface, &layer_surface_listener, output);
  zwlr_layer_surface_v1_set_size(output->layer_surface, 0, (uint32_t)app->requested_water_height);
  zwlr_layer_surface_v1_set_anchor(output->layer_surface,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
  zwlr_layer_surface_v1_set_exclusive_zone(output->layer_surface, 0);
  zwlr_layer_surface_v1_set_keyboard_interactivity(output->layer_surface,
                                                   ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_surface_commit(output->surface);
}

static void registry_handle_global(void *data,
                                   struct wl_registry *registry,
                                   uint32_t name,
                                   const char *interface,
                                   uint32_t version)
{
  struct app_state *app = data;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, min_u32(version, 4));
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    app->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, min_u32(version, 4));
  } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
    app->screencopy_version = min_u32(version, 3);
    app->screencopy_manager = wl_registry_bind(registry,
                                               name,
                                               &zwlr_screencopy_manager_v1_interface,
                                               app->screencopy_version);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    struct output_state *output = calloc(1, sizeof(*output));
    if (output == NULL) {
      perror("allocate output state");
      should_exit = 1;
      return;
    }
    output->app = app;
    output->registry_name = name;
    output->output_version = min_u32(version, 4);
    output->scale = 1;
    output->output = wl_registry_bind(registry, name, &wl_output_interface, output->output_version);
    wl_output_add_listener(output->output, &output_listener, output);
    output->next = app->outputs;
    app->outputs = output;
  }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
  (void)registry;
  struct app_state *app = data;
  struct output_state **link = &app->outputs;
  while (*link != NULL) {
    struct output_state *output = *link;
    if (output->registry_name == name) {
      *link = output->next;
      free_output(output);
      return;
    }
    link = &output->next;
  }
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_handle_global,
  .global_remove = registry_handle_global_remove,
};

static void print_stats(struct app_state *app)
{
  uint64_t total_frames = 0;
  uint64_t total_captures = 0;
  uint64_t total_failed = 0;
  uint64_t total_dropped = 0;
  uint64_t total_render_ns = 0;
  uint64_t total_capture_ns = 0;

  for (struct output_state *output = app->outputs; output != NULL; output = output->next) {
    total_frames += output->frames_rendered;
    total_captures += output->captures_completed;
    total_failed += output->captures_failed;
    total_dropped += output->dropped_frames;
    total_render_ns += output->total_render_ns;
    total_capture_ns += output->total_capture_ns;

    double average_render_ms = output->frames_rendered > 0
                                 ? (double)output->total_render_ns / (double)output->frames_rendered / 1000000.0
                                 : 0.0;
    double average_capture_ms = output->captures_completed + output->captures_failed > 0
                                  ? (double)output->total_capture_ns /
                                      (double)(output->captures_completed + output->captures_failed) /
                                      1000000.0
                                  : 0.0;
    fprintf(stderr,
            "output %s: frames=%llu captures=%llu failed=%llu dropped=%llu avg_render_ms=%.3f avg_capture_ms=%.3f\n",
            output->name != NULL ? output->name : "(unnamed)",
            (unsigned long long)output->frames_rendered,
            (unsigned long long)output->captures_completed,
            (unsigned long long)output->captures_failed,
            (unsigned long long)output->dropped_frames,
            average_render_ms,
            average_capture_ms);
  }

  double average_render_ms = total_frames > 0
                               ? (double)total_render_ns / (double)total_frames / 1000000.0
                               : 0.0;
  double average_capture_ms = total_captures + total_failed > 0
                                ? (double)total_capture_ns / (double)(total_captures + total_failed) / 1000000.0
                                : 0.0;

  fprintf(stderr,
          "total: frames=%llu captures=%llu failed=%llu dropped=%llu avg_render_ms=%.3f avg_capture_ms=%.3f\n",
          (unsigned long long)total_frames,
          (unsigned long long)total_captures,
          (unsigned long long)total_failed,
          (unsigned long long)total_dropped,
          average_render_ms,
          average_capture_ms);
}

static void cleanup_app(struct app_state *app)
{
  struct output_state *output = app->outputs;
  while (output != NULL) {
    struct output_state *next = output->next;
    free_output(output);
    output = next;
  }
  if (app->screencopy_manager != NULL) {
    zwlr_screencopy_manager_v1_destroy(app->screencopy_manager);
  }
  if (app->layer_shell != NULL) {
    zwlr_layer_shell_v1_destroy(app->layer_shell);
  }
  if (app->shm != NULL) {
    wl_shm_destroy(app->shm);
  }
  if (app->compositor != NULL) {
    wl_compositor_destroy(app->compositor);
  }
  if (app->registry != NULL) {
    wl_registry_destroy(app->registry);
  }
  if (app->display != NULL) {
    wl_display_disconnect(app->display);
  }
}

int main(int argc, char **argv)
{
  struct app_state app;
  memset(&app, 0, sizeof(app));
  app.requested_water_height = DEFAULT_WATER_HEIGHT;
  app.running = true;

  if (!parse_arguments(&app, argc, argv)) {
    print_usage(argv[0]);
    return 2;
  }

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  app.display = wl_display_connect(NULL);
  if (app.display == NULL) {
    fprintf(stderr, "failed to connect to Wayland display\n");
    return 1;
  }

  app.registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(app.registry, &registry_listener, &app);
  wl_display_roundtrip(app.display);
  wl_display_roundtrip(app.display);

  if (app.compositor == NULL || app.shm == NULL || app.layer_shell == NULL ||
      app.screencopy_manager == NULL) {
    fprintf(stderr,
            "missing required Wayland globals: compositor=%s shm=%s layer_shell=%s screencopy=%s\n",
            app.compositor != NULL ? "yes" : "no",
            app.shm != NULL ? "yes" : "no",
            app.layer_shell != NULL ? "yes" : "no",
            app.screencopy_manager != NULL ? "yes" : "no");
    cleanup_app(&app);
    return 1;
  }

  if (app.outputs == NULL) {
    fprintf(stderr, "no Wayland outputs found\n");
    cleanup_app(&app);
    return 1;
  }

  for (struct output_state *output = app.outputs; output != NULL; output = output->next) {
    create_layer_surface_for_output(output);
  }

  wl_display_roundtrip(app.display);

  while (!should_exit) {
    if (wl_display_dispatch(app.display) < 0) {
      break;
    }
  }

  if (app.benchmark || app.verbose || app.total_frames_rendered > 0) {
    print_stats(&app);
  }

  cleanup_app(&app);
  return 0;
}
