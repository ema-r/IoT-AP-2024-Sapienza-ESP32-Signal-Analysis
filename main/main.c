#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "esp_err.h"
#include "hal/adc_types.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_continuous.h"
#include "soc/soc_caps.h" 
#include "esp_check.h"

#define TOTAL_READ_LEN        4096
#define N_OF_ROUNDS           4
#define READ_LEN_SINGLE_ROUND TOTAL_READ_LEN/N_OF_ROUNDS
#define READ_BUFFER_SIZE      READ_LEN_SINGLE_ROUND*2

static TaskHandle_t s_task_handle;
static const char *TAG = "EXAMPLE";

static bool IRAM_ATTR s_conv_done_ch(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data) {
  BaseType_t mustYield = pdFALSE;

  vTaskNotifyGiveFromISR(s_task_handle, &mustYield);
  return (mustYield == pdTRUE);
}

// ************************************************************
// ADC Continuous Read
// ************************************************************
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
        ESP_LOGI("ADC SAMPLER", "Value: %f", output_array[j]);
        j++;
      }
    }
  }
  
  ESP_ERROR_CHECK(adc_continuous_stop(*handle));
}

void init_adc_continuous_driver(adc_continuous_handle_t * handle_to_fill) {
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
    .sample_freq_hz  = SOC_ADC_SAMPLE_FREQ_THRES_HIGH,   // Max driver freq.
    .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2, // 12 bits?
    .conv_mode = ADC_CONV_SINGLE_UNIT_1,    // We're using single mode on adc unit 1
  };
  ESP_ERROR_CHECK(adc_continuous_config(handle, &adc_cont_cfg));

  *handle_to_fill = handle;
}

// ************************************************************
// FFT Functions
// ************************************************************

// ************************************************************
// Main Function
// ************************************************************

void app_main(void) {
  adc_continuous_handle_t handle = NULL;
  float * sampling_array = (float *) calloc(READ_LEN_SINGLE_ROUND, sizeof(float));
  init_adc_continuous_driver(&handle);

  adc_sample_single_block(&handle, sampling_array);

  for (int i = 0; i < READ_LEN_SINGLE_ROUND; i++) {
    ESP_LOGI("YES", "Value %f", sampling_array[i]);
  }
}
