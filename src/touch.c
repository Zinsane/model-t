
#include "ch.h"
#include "hal.h"
#include "touch.h"
#include "terminal.h"
#include "lcd.h"
#include "touch_calib.h"
#include "gui.h"

#include <stdbool.h>


#define XP 4
#define XN 6
#define YP 5
#define YN 7

#define NUM_SAMPLES 8
#define DISCARDED_SAMPLES 1

#define TOUCH_THRESHOLD 950
#define DEBOUNCE_TIME MS2ST(100)

// ADC sample resolution
#define Q 1024

typedef struct {
  uint16_t drive_pos_pad;
  uint16_t drive_neg_pad;
  uint16_t sample_pos_pad;
  uint16_t sample_neg_pad;
  const ADCConversionGroup* conv_grp;
} axis_cfg_t;

typedef struct touch_handler_s {
  touch_handler_t on_touch;
  void* user_data;

  struct touch_handler_s* next;
} touch_handler_entry_t;


static uint16_t read_axis(const axis_cfg_t* axis_cfg);
static adcsample_t adc_avg(adcsample_t* samples, uint16_t num_samples);
static msg_t touch_thread(void* arg);
static void touch_dispatch(void);


static const ADCConversionGroup xp_conv_grp = {
  .circular = FALSE,
  .num_channels = 1,
  .end_cb = NULL,
  .error_cb = NULL,

  /* HW dependent part.*/
  .cr1   = ADC_CR1_RES_0, // 10-bit resolution
  .cr2   = ADC_CR2_SWSTART, // software triggered
  .smpr1 = 0,
  .smpr2 = ADC_SMPR2_SMP_AN4(ADC_SAMPLE_480),
  .sqr1  = ADC_SQR1_NUM_CH(1),
  .sqr2  = 0,
  .sqr3  = ADC_SQR3_SQ1_N(ADC_CHANNEL_IN4),
};

static const ADCConversionGroup yp_conv_grp = {
  .circular = FALSE,
  .num_channels = 1,
  .end_cb = NULL,
  .error_cb = NULL,

  /* HW dependent part.*/
  .cr1   = ADC_CR1_RES_0, // 10-bit resolution
  .cr2   = ADC_CR2_SWSTART, // software triggered
  .smpr1 = 0,
  .smpr2 = ADC_SMPR2_SMP_AN5(ADC_SAMPLE_480),
  .sqr1  = ADC_SQR1_NUM_CH(1),
  .sqr2  = 0,
  .sqr3  = ADC_SQR3_SQ1_N(ADC_CHANNEL_IN5),
};

static const ADCConversionGroup yn_conv_grp = {
  .circular = FALSE,
  .num_channels = 1,
  .end_cb = NULL,
  .error_cb = NULL,

  /* HW dependent part.*/
  .cr1   = ADC_CR1_RES_0, // 10-bit resolution
  .cr2   = ADC_CR2_SWSTART, // software triggered
  .smpr1 = 0,
  .smpr2 = ADC_SMPR2_SMP_AN7(ADC_SAMPLE_480),
  .sqr1  = ADC_SQR1_NUM_CH(1),
  .sqr2  = 0,
  .sqr3  = ADC_SQR3_SQ1_N(ADC_CHANNEL_IN7),
};

static const axis_cfg_t z1_axis = {
    .drive_pos_pad = YP,
    .drive_neg_pad = XN,
    .sample_pos_pad = XP,
    .sample_neg_pad = YN,
    .conv_grp = &xp_conv_grp,
};

static const axis_cfg_t z2_axis = {
    .drive_pos_pad = YP,
    .drive_neg_pad = XN,
    .sample_pos_pad = XP,
    .sample_neg_pad = YN,
    .conv_grp = &yn_conv_grp,
};

static const axis_cfg_t x_axis = {
    .drive_pos_pad = XP,
    .drive_neg_pad = XN,
    .sample_pos_pad = YP,
    .sample_neg_pad = YN,
    .conv_grp = &yp_conv_grp,
};

static const axis_cfg_t y_axis = {
    .drive_pos_pad = YP,
    .drive_neg_pad = YN,
    .sample_pos_pad = XP,
    .sample_neg_pad = XN,
    .conv_grp = &xp_conv_grp,
};

static uint8_t wa_touch_thread[1024];
static uint8_t touch_down;
static systime_t last_touch_time;
static point_t touch_coord_raw;
static point_t touch_coord_calib;
static touch_handler_entry_t* touch_handlers;

static matrix_t calib_matrix = {
    .An      = 76320,
    .Bn      = 3080,
    .Cn      = -9475080,
    .Dn      = -560,
    .En      = 60340,
    .Fn      = -4360660,
    .Divider = 205664,
};

void
touch_init()
{
  adcStart(&ADCD1, NULL);

  chThdCreateStatic(wa_touch_thread, sizeof(wa_touch_thread), NORMALPRIO, touch_thread, NULL);
}

void
touch_handler_register(touch_handler_t touch_handler, void* user_data)
{
  touch_handler_entry_t* entry = malloc(sizeof(touch_handler_entry_t));
  entry->on_touch = touch_handler;
  entry->user_data = user_data;

  entry->next = touch_handlers;
  touch_handlers = entry;
}

void
touch_handler_unregister(touch_handler_t handler)
{
  touch_handler_entry_t* prev = NULL;
  touch_handler_entry_t* h;

  for (h = touch_handlers; h != NULL; h = h->next) {
    if (h->on_touch == handler) {
      if (prev == NULL)
        touch_handlers = h->next;
      else
        prev->next = h->next;
      break;
    }
    prev = h;
  }
}

static void
touch_dispatch()
{
  touch_handler_entry_t* h;
  for (h = touch_handlers; h != NULL; h = h->next) {
    if (h->on_touch != NULL)
      h->on_touch(touch_down, touch_coord_raw, touch_coord_calib, h->user_data);
  }
}

void
touch_calibrate(
    const point_t* ref_pts,
    const point_t* sampled_pts)
{
  setCalibrationMatrix(ref_pts, sampled_pts, &calib_matrix);

  chprintf(stdout, "touch calib params: %d %d %d %d %d %d %d\r\n",
      calib_matrix.An,
      calib_matrix.Bn,
      calib_matrix.Cn,
      calib_matrix.Dn,
      calib_matrix.En,
      calib_matrix.Fn,
      calib_matrix.Divider);
}

static uint16_t
read_axis(const axis_cfg_t* axis_cfg)
{
  adcsample_t samples[NUM_SAMPLES];

  /* setup the pad modes */
  palSetPadMode(GPIOA, axis_cfg->sample_pos_pad, PAL_MODE_INPUT_ANALOG);
  palSetPadMode(GPIOA, axis_cfg->sample_neg_pad, PAL_MODE_INPUT_ANALOG);
  palSetPadMode(GPIOA, axis_cfg->drive_pos_pad, PAL_MODE_OUTPUT_PUSHPULL);
  palSetPadMode(GPIOA, axis_cfg->drive_neg_pad, PAL_MODE_OUTPUT_PUSHPULL);

  /* set the 'drive' pins to the correct state */
  palSetPad(GPIOA, axis_cfg->drive_pos_pad);
  palClearPad(GPIOA, axis_cfg->drive_neg_pad);

  /* Let the pins settle */
  chThdSleepMicroseconds(100);

  /* capture a number of samples from the read pin */
  adcConvert(&ADCD1, axis_cfg->conv_grp, samples, NUM_SAMPLES);

  /* average and return the samples */
  return adc_avg(samples+DISCARDED_SAMPLES,
      NUM_SAMPLES-DISCARDED_SAMPLES);
}

static msg_t
touch_thread(void* arg)
{
  (void)arg;

  while (1) {
    uint32_t z1 = read_axis(&z1_axis);
    uint32_t z2 = read_axis(&z2_axis);
    uint32_t x = read_axis(&x_axis);
    uint32_t y = read_axis(&y_axis);

    /* Calculate pressure of touch based on equations from TI Application Note SBAA155A */
    /* Prevent divide by zero */
    z1 = MAX(1, z1);
    /* Modified form of equation 8 with rx = 1 */
    uint32_t rz = (((x * z2) / z1) - x) / Q;
    /* Modified form of equation 9 with a = 1024, b = 1 */
    uint32_t p = Q - rz;

    if (p > TOUCH_THRESHOLD) {
#if (DISP_ORIENT == LANDSCAPE)
      /* swap the coordinates since the screen is rotated */
      touch_coord_raw.x = y;
      touch_coord_raw.y = x;
#else
      touch_coord_raw.x = x;
      touch_coord_raw.y = y;
#endif

      /* calibrate the raw touch coordinate */
      getDisplayPoint(
          &touch_coord_calib,
          &touch_coord_raw,
          &calib_matrix);

      touch_down = 1;
      last_touch_time = chTimeNow();

      /* distribute the touch event */
      touch_dispatch();
    }
    else {
      if (touch_down &&
          !chTimeIsWithin(last_touch_time, last_touch_time + DEBOUNCE_TIME)) {
        touch_down = 0;
        touch_dispatch();
      }
    }
  }

  return 0;
}

adcsample_t
adc_avg(adcsample_t* samples, uint16_t num_samples)
{
  uint32_t avg_sample = 0;
  int i;

  for (i = 0; i < num_samples; ++i) {
    avg_sample += samples[i];
  }
  avg_sample /= num_samples;

  return avg_sample;
}
