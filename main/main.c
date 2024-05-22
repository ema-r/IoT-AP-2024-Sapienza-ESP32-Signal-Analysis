#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include "esp_partition.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_dsp.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_adc/adc_continuous.h"

#include "hal/adc_types.h"
#include "sdkconfig.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "soc/soc_caps.h"
#include "soc/gpio_struct.h"
#include "soc/uart_struct.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "dsps_fft2r.h"
#include "mqtt_client.h"

#define TOTAL_READ_LEN        8192
#define READ_LEN_SINGLE_ROUND 256
#define RESULTING_SAMPLES     TOTAL_READ_LEN/4
#define N_OF_ROUNDS           TOTAL_READ_LEN/READ_LEN_SINGLE_ROUND
#define READ_BUFFER_SIZE      READ_LEN_SINGLE_ROUND*2
#define FREQ_AMPLI_THRESHOLD  0
//#define FORCE_MAX_SAMPL_FREQ  1

#define WIFI_MAX_RETRIES      2
#define WIFI_SUCCESS          BIT0
#define WIFI_FAILURE          BIT1

static TaskHandle_t s_task_handle;

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
        j++;
      }
    }
  }
  
  ESP_ERROR_CHECK(adc_continuous_stop(*handle));
}

void adc_sample_test_freq(adc_continuous_handle_t * handle, uint32_t total_size) {
  // Main code has to take care of initing the handle
  uint32_t ret;
  uint32_t ret_num = 0;
  uint8_t result[READ_LEN_SINGLE_ROUND] = {0};
  memset(result, 0xcc, READ_LEN_SINGLE_ROUND);
  uint16_t n_of_rounds = total_size / READ_LEN_SINGLE_ROUND;

  s_task_handle = xTaskGetCurrentTaskHandle();

  adc_continuous_evt_cbs_t cbs =  {
    .on_conv_done = s_conv_done_ch,
  };
  ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(*handle, &cbs, NULL));
  ESP_ERROR_CHECK(adc_continuous_start(*handle));
  
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  int j = 0;
  for (int r = 0; r < n_of_rounds; r++) {
    ret = adc_continuous_read(*handle, result, READ_LEN_SINGLE_ROUND, &ret_num, 0);
    if (ret == ESP_OK) {
      for (int i = 0; i < ret_num && j < TOTAL_READ_LEN; i += SOC_ADC_DIGI_RESULT_BYTES) {
        j++;
      }
    }
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

float calculate_average_value(float * sampled_block, uint16_t sample_size) {
  float sum = 0.0f;
  for (int i = 0; i < sample_size; i++) {
    sum += sampled_block[i];
  }
  return sum/(float)sample_size;
}

uint32_t threshold_find_max_freq(float * fft_array, uint16_t fft_size, int16_t threshold) {
  for (int i = fft_size - 1; i >= 0; i--) {
    if (fft_array[i] >= threshold) {
      ESP_LOGI(FFT_TAG, "earliest good freq found at %d", i);
      return i * SOC_ADC_SAMPLE_FREQ_THRES_HIGH / (fft_size*4);
    }
  }
  return 0;
}

float calculate_standard_deviation(float * sampled_block, uint16_t sample_size, float mean) {
  float std_dev = 0;

  for (int i = 0; i < sample_size; i++) {
    std_dev += (sampled_block[i] - mean) * (sampled_block[i] - mean);
  }

  return sqrt(std_dev/sample_size);
}

void calculate_max_freq(float * input_arr, uint32_t * output_float) {
  uint16_t fft_size = RESULTING_SAMPLES;
  float * fft_buffer = (float *)calloc(RESULTING_SAMPLES*2, sizeof(float));
  float * wind = (float *)calloc(RESULTING_SAMPLES*2, sizeof(float));

  dsps_wind_hann_f32(wind, RESULTING_SAMPLES);

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

  // calculate mean
  float avg = calculate_average_value(fft_buffer, fft_size/2);
  
  // get standard deviation
  float std_dev = calculate_standard_deviation(fft_buffer, fft_size/2, avg);

  // use the calculated standard deviation as threshold
  float sensitivity = 1.0f;
  uint32_t res = threshold_find_max_freq(fft_buffer, fft_size/2, avg+(std_dev*sensitivity));
  ESP_LOGI(FFT_TAG, "max frequency: %"PRIu32" against total freq: %d", res, SOC_ADC_SAMPLE_FREQ_THRES_HIGH);

  dsps_fft2r_deinit_fc32(); 

  free(fft_buffer);

  *output_float = res;
}

// ************************************************************
// wifi Functions
// ************************************************************

#define SSID "replaceme"
#define PASS "replaceme"

static char * WIFI_TAG = "WIFI";
static EventGroupHandle_t wifi_evt_group;

void wifi_evt_handler(void * args, esp_event_base_t evt_base, int32_t evt_id, void * evt_data) {
  if (evt_base == WIFI_EVENT) {
    if (evt_id == WIFI_EVENT_STA_START) {
      esp_wifi_connect();
    } else if (evt_id == WIFI_EVENT_STA_DISCONNECTED) {
      for (int i = 0; i < WIFI_MAX_RETRIES; i++) {
        esp_wifi_connect();
        ESP_LOGI(WIFI_TAG, "attempting to reconnect...");
      }
      xEventGroupSetBits(wifi_evt_group, WIFI_FAILURE);
    }
  } else if (evt_base == IP_EVENT && evt_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t * event = (ip_event_got_ip_t*) evt_data;
    ESP_LOGI(WIFI_TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(wifi_evt_group, WIFI_SUCCESS);
  }
}

void init_wifi() {
  wifi_evt_group = xEventGroupCreate();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  wifi_config_t wifi_config = {
    .sta =  {
      .ssid = SSID,
      .password = PASS,
      .threshold.authmode = WIFI_AUTH_OPEN,
      .sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED,
      .sae_h2e_identifier = "",
    },
  };

  ESP_ERROR_CHECK(esp_netif_init());
  
  esp_netif_create_default_wifi_sta();

  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_id;
  esp_event_handler_instance_t instance_ip;

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                      &wifi_evt_handler,
                                                      NULL,
                                                      &instance_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                      &wifi_evt_handler,
                                                      NULL,
                                                      &instance_ip));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_LOGI(WIFI_TAG, "setup config done, starting");
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(WIFI_TAG, "started");

  // Event handling
  // This parts waits until connection either fails or is achieved.
  EventBits_t res_bit = xEventGroupWaitBits(wifi_evt_group,
                                            WIFI_SUCCESS | WIFI_FAILURE,
                                            pdFALSE, pdFALSE, portMAX_DELAY);
  
  if (res_bit == WIFI_SUCCESS) {
    ESP_LOGI(WIFI_TAG, "wifi ok");
  } else {
    ESP_LOGI(WIFI_TAG, "wifi is not ok");
  }
}

// ************************************************************
// mqtt Functions
// ************************************************************
#define BROKER_URL "replaceme"
#define MQTT_PWORD "replaceme"
#define MQTT_UNAME "replaceme"

extern const uint8_t mqtt_hivemq_server_cert_pem_start[] asm("_binary_hivemq_servercert_pem_start");
extern const uint8_t mqtt_hivemq_server_cert_pem_stop[] asm("_binary_hivemq_servercert_pem_stop");

clock_t mqtt_rtt_begin = 0;
double rtt = 0;

esp_event_loop_handle_t mqtt_loop;

void mqtt_evt_handler(void * args, esp_event_base_t evt_base, int32_t evt_id, void * evt_data) {
  switch ((esp_mqtt_event_id_t)evt_id) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI("mqtt", "connected to broker");
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI("mqtt", "lost connection to broker");
      break;
    case MQTT_EVENT_PUBLISHED:
      rtt = (double) (clock() - mqtt_rtt_begin) / CLOCKS_PER_SEC;
      ESP_LOGI("mqtt", "data succesfully published to broker and ACK'd, rtt: %lf", rtt);
      break;
    default:
      break;
  }
}

void init_mqtt(esp_mqtt_client_handle_t * mqtt_client_pt) {
  esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.uri = BROKER_URL,
    .broker.verification.certificate = (const char *)mqtt_hivemq_server_cert_pem_start,
    .credentials.username = MQTT_UNAME,
    .credentials.authentication.password = MQTT_PWORD,
  };

  *mqtt_client_pt = esp_mqtt_client_init(&mqtt_cfg);
  
  esp_mqtt_client_register_event(*mqtt_client_pt, ESP_EVENT_ANY_ID, mqtt_evt_handler, NULL);
  esp_mqtt_client_start(*mqtt_client_pt);
}

// ************************************************************
// Main Function
// ************************************************************
static char * MAIN_TAG = "MAIN";

esp_mqtt_client_handle_t mqtt_client_handle;
float sum = 0;
int cntr = 0;

void main_timer_callback(void * arg) {
  ESP_LOGI(MAIN_TAG, "timer triggered, calculating average...");
  float avg = sum / (cntr * 1.0f);
  ESP_LOGI(MAIN_TAG, "calculated average: %f; Sending data over connection...", avg);
  char avg_str[80] = {0};
  snprintf(avg_str, 12, "%f", avg);

  double time_spent = (double) 5000.0f / CLOCKS_PER_SEC;
  ESP_LOGI(MAIN_TAG, "%d blocks sampled in 5 seconds", cntr);
  double found_freq = (cntr * 1.0f) / time_spent;
  ESP_LOGI(MAIN_TAG, "found freq for reduced sampling rate: %lf", found_freq);

  mqtt_rtt_begin = clock();
  esp_mqtt_client_publish(mqtt_client_handle, "/avg", avg_str, 0, 1, 0);
  cntr = 0;
  sum  = 0;
}

void adc_avg_while_sampling(adc_continuous_handle_t * handle) {
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
        sum += ((p)->type2.data) * 1.0f;
        cntr += 1;
        //ESP_LOGI("ADC SAMPLER", "Value: %f", output_array[j]);
        j++;
      }
    }
    //vTaskDelay(1);
  }
  
  ESP_ERROR_CHECK(adc_continuous_stop(*handle));
}

uint32_t test_theoretical_freq(adc_continuous_handle_t * handle) {
  uint32_t n_of_samples = 262144;
  clock_t begin = clock();
  adc_sample_test_freq(handle, n_of_samples);
  clock_t end = clock();

  double time_spent = (double) (end-begin) / CLOCKS_PER_SEC;
  ESP_LOGI(MAIN_TAG, "%"PRIu32" blocks sampled in %lf seconds", n_of_samples, time_spent);
  uint32_t found_freq = ((n_of_samples / time_spent) < SOC_ADC_SAMPLE_FREQ_THRES_HIGH)? found_freq : SOC_ADC_SAMPLE_FREQ_THRES_HIGH;
  ESP_LOGI(MAIN_TAG, "found freq for max sampling rate: %"PRIu32, found_freq);
  return found_freq;
}

void app_main(void) {

  adc_continuous_handle_t handle = NULL;
  float * sampling_array = (float *) calloc(RESULTING_SAMPLES, sizeof(float));
  init_adc_continuous_driver(&handle, SOC_ADC_SAMPLE_FREQ_THRES_HIGH);

  adc_sample_single_block(&handle, sampling_array);

  uint32_t found_freq = test_theoretical_freq(&handle);

  adc_continuous_deinit(handle);

  esp_timer_handle_t timer_handle;
  esp_timer_create_args_t timer_args = {
    .callback = &main_timer_callback,
    .name = "maintimer",
  };

  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle));

  // Connect to wifi
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  init_wifi();
  init_mqtt(&mqtt_client_handle);

  // Get max frequency to reduce sampling rate
#ifndef FORCE_MAX_SAMPL_FREQ
  uint32_t max_freq = 0.0f;
  calculate_max_freq(sampling_array, &max_freq);
  uint32_t sampling_freq = ((max_freq * 2) < found_freq)? (max_freq * 2) : found_freq;
#else
  uint32_t sampling_freq = SOC_ADC_SAMPLE_FREQ_THRES_HIGH;
#endif

  ESP_LOGI(MAIN_TAG, "sampling frequency %"PRIu32, sampling_freq);
  ESP_LOGI(MAIN_TAG, "max sampling frequency %"PRIu32, found_freq);
  free(sampling_array);

  // restart sampling
  init_adc_continuous_driver(&handle, sampling_freq);

  ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle, 5000000));

  while(1) {
    adc_avg_while_sampling(&handle);
  }

  adc_continuous_deinit(handle);
}
