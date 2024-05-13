#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "dsps_fft2r.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_dsp.h"
#include "hal/adc_types.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_continuous.h"
#include "soc/soc_caps.h"
#include "soc/gpio_struct.h"
#include "soc/uart_struct.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"


#define TOTAL_READ_LEN        8192
#define READ_LEN_SINGLE_ROUND 256
#define RESULTING_SAMPLES     TOTAL_READ_LEN/4
#define N_OF_ROUNDS           TOTAL_READ_LEN/READ_LEN_SINGLE_ROUND
#define READ_BUFFER_SIZE      READ_LEN_SINGLE_ROUND*2
#define FREQ_AMPLI_THRESHOLD  0

static TaskHandle_t s_task_handle;
static const char *TAG = "EXAMPLE";

static bool IRAM_ATTR s_conv_done_ch(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data) {
  BaseType_t mustYield = pdFALSE;

  vTaskNotifyGiveFromISR(s_task_handle, &mustYield);
  return (mustYield == pdTRUE);
}

// ************************************************************
// ADC Continuous Read
// ***********************************************************
static const char *ADC_TAG = "ADC";

void adc_sample_single_block(adc_continuous_handle_t * handle, float * output_array) {
  // Main code has to take care of initing the handle
  uint32_t ret;
  uint32_t ret_num = 0;
  uint8_t result[READ_LEN_SINGLE_ROUND] = {0};
  memset(result, 0xcc, READ_LEN_SINGLE_ROUND);

  s_task_handle = xTaskGetCurrentTaskHandle();

  adc_continuous_evt_cbs_t cbs =  {
    .on_conv_done = s_conv_done_ch,
  };
  ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(*handle, &cbs, NULL));
  ESP_ERROR_CHECK(adc_continuous_start(*handle));
  
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  int j = 0;
  for (int r = 0; r < N_OF_ROUNDS; r++) {
    ret = adc_continuous_read(*handle, result, READ_LEN_SINGLE_ROUND, &ret_num, 0);
    if (ret == ESP_OK) {
      for (int i = 0; i < ret_num && j < TOTAL_READ_LEN; i += SOC_ADC_DIGI_RESULT_BYTES) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
        output_array[j] = ((p)->type2.data) * 1.0f;
        //ESP_LOGI("ADC SAMPLER", "Value: %f", output_array[j]);
        j++;
      }
    }
    vTaskDelay(1);
  }
  
  ESP_ERROR_CHECK(adc_continuous_stop(*handle));
}

void init_adc_continuous_driver(adc_continuous_handle_t * handle_to_fill, uint32_t freq) {
  adc_continuous_handle_t handle = NULL;
  // Init handle config
  adc_continuous_handle_cfg_t handle_cfg = {
    .max_store_buf_size = READ_BUFFER_SIZE,
    .conv_frame_size = READ_LEN_SINGLE_ROUND,
  };
  ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &handle));

  // Digi pattern config, needed to config the ADC Continuous driver
  adc_digi_pattern_config_t digi_pattern_cfg = {
    .atten   = ADC_ATTEN_DB_12,
    .channel = ADC_CHANNEL_0,
    .unit    = ADC_UNIT_1,    // Unit 2 interferes with WiFi
    .bit_width = ADC_BITWIDTH_12,
  };

  // ADC Continuous driver config
  adc_continuous_config_t adc_cont_cfg = {
    .pattern_num = 1,   // We're only going to use a single channel.
    .adc_pattern = &digi_pattern_cfg,
    .sample_freq_hz  = freq,
    .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2, // 12 bits?
    .conv_mode = ADC_CONV_SINGLE_UNIT_1,    // We're using single mode on adc unit 1
  };
  ESP_ERROR_CHECK(adc_continuous_config(handle, &adc_cont_cfg));

  *handle_to_fill = handle;
}

// ************************************************************
// FFT Functions
// ************************************************************
static const char *FFT_TAG = "FFT";

uint32_t threshold_find_max_freq(float * fft_array, uint16_t fft_size, int16_t threshold) {
  for (int i = fft_size - 1; i >= 0; i--) {
    // printf("%f\n", fft_array[i]);
    if (fft_array[i] >= threshold) {
      ESP_LOGI(FFT_TAG, "earliest good freq found at %d", i);
      return i * SOC_ADC_SAMPLE_FREQ_THRES_HIGH / (fft_size*2);
    }
  }
  return 0;
}

void calculate_max_freq(float * input_arr, uint32_t * output_float) {
  uint16_t fft_size = RESULTING_SAMPLES;
  float * fft_buffer = (float *)calloc(RESULTING_SAMPLES*2, sizeof(float));
  float * wind = (float *)calloc(RESULTING_SAMPLES*2, sizeof(float));

  //for (int i = 0; i < RESULTING_SAMPLES; i++) {
  //  fft_buffer[i] = input_arr[i];
  //}
  dsps_wind_hann_f32(wind, RESULTING_SAMPLES);
  //dsps_tone_gen_f32(input_arr, RESULTING_SAMPLES, 1.0, 0.49, 0);

  for (int i = 0; i < RESULTING_SAMPLES; i++) {
    fft_buffer[i*2] = input_arr[i] * wind[i];
    fft_buffer[i*2+1] = 0 * wind[i];
  }

  if (dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE) != ESP_OK) {
    ESP_LOGE(FFT_TAG, "FFT2R init error");
    return;
  }

  dsps_fft2r_fc32(fft_buffer, fft_size);
  dsps_bit_rev2r_fc32(fft_buffer, fft_size);
  dsps_cplx2reC_fc32(fft_buffer, fft_size);

  for (int i = 0; i < fft_size; i++) {
    fft_buffer[i] = 10 * log10f((fft_buffer[i*2] * fft_buffer[i*2] + fft_buffer[i*2+1] * fft_buffer[i*2+1]) / RESULTING_SAMPLES);
  }

  dsps_view(fft_buffer, fft_size/2, 64, 10 , -60, 40, '|');

  uint32_t res = threshold_find_max_freq(fft_buffer, fft_size/2, FREQ_AMPLI_THRESHOLD);
  ESP_LOGI(FFT_TAG, "max frequency: %"PRIu32" against total freq: %d", res, SOC_ADC_SAMPLE_FREQ_THRES_HIGH);

  dsps_fft2r_deinit_fc32(); 
  //dsps_fft2r_deinit_fc32();
  free(fft_buffer);

  *output_float = res;
}

// ************************************************************
// Connectivity Functions
// ************************************************************

// ************************************************************
// Aux Functions
// ************************************************************
float calculate_average_value(float * sampled_block, uint16_t sample_size) {
  float sum = 0.0f;
  for (int i = 0; i < sample_size; i++) {
    sum += sampled_block[i];
  }
  return sum/(float)sample_size;
}
// ************************************************************
// Main Function
// ************************************************************

static char * MAIN_TAG = "MAIN";
void app_main(void) {
  adc_continuous_handle_t handle = NULL;
  float * sampling_array = (float *) calloc(RESULTING_SAMPLES, sizeof(float));
  init_adc_continuous_driver(&handle, SOC_ADC_SAMPLE_FREQ_THRES_HIGH);

  adc_sample_single_block(&handle, sampling_array);

  adc_continuous_deinit(handle);

  //for (int i = 0; i < RESULTING_SAMPLES; i++) {
  //  ESP_LOGI("YES", "Value %f", sampling_array[i]);
  //  vTaskDelay(1);
  //}

  // Get max frequency to reduce sampling rate
  uint32_t max_freq = 0.0f;
  calculate_max_freq(sampling_array, &max_freq);
  uint32_t sampling_freq = ((max_freq * 2) < SOC_ADC_SAMPLE_FREQ_THRES_HIGH)? (max_freq * 2) : SOC_ADC_SAMPLE_FREQ_THRES_HIGH;
  printf("sampling_freq %ld\n", sampling_freq);
  printf("max sample %d\n", SOC_ADC_SAMPLE_FREQ_THRES_HIGH);

  // Connect to wifi

  // restart sampling
  init_adc_continuous_driver(&handle, sampling_freq);
  float avg = 0;
  while(1) {
    adc_sample_single_block(&handle, sampling_array);
    ESP_LOGI(MAIN_TAG, "block sampled, calculating average...");
    avg = calculate_average_value(sampling_array, RESULTING_SAMPLES);
    ESP_LOGI(MAIN_TAG, "calculated average: %f; Sending data over connection...", avg);
  }

  adc_continuous_deinit(handle);
  free(sampling_array); 
}
