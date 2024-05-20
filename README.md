# _IoT Algorithms and Services Sapienza 23/24 - Signal Analysis_

The following repository contains the coursework project for the 23/34 edition
of the IoT Algorithms and Services course held in Sapienza University of Rome.

The code is a small ESP32 based application that performs some simple analysis
of an analog input signal and sends the resulting data to a broker employing the
MQTT protocol.

```
idf.py set-target <esp32 platform> && idf.py -p /dev/ttyUSB<port_num> flash monitor
```

## How to setup
Clone the repository, then put a copy of the MQTT broker public key in the
"./main/certs" folder, as "hivemq_servercert.pem". Despite the name, the
implementation is platform agnostic regarding the broker.

To finish the setup process, change the macros under main that define the SSID
and Password of a Wi-Fi network one wishes to connect to, and the Username, 
Password and Broker Uri fields for the MQTTS configuration.

Just run the usual idf.py configuration to flash the program onto the esp32 device


## Objectives
The project requires correctly sampling an analog, sum of sinusoids signal,
starting with an initial oversampling phase to correctly gauge the input signal's
maximum frequency, to allow the lowering of the sampling frequency to the 
minimum while still adhering to Shannon's sampling theorem. With the new,
optimal sampling frequency at hand, we start sampling signals at a reduced rate,
reducing energy consumption.

Every 5 seconds, a moving average of the signal is performed and the data is sent
over a secure MQTT over TLS connection to a broker, utilizing a Wi-Fi network to
reach the internet.

### Max frequency oversampling
The program begins by initiating a continuous read over ADC_CHANNEL_0. This
utilizes the "adc_continuous_read" driver for the ESP32 Analog-to-Digital
conversion peripherals.

After initializing the driver, set to sample at the maximum possible sampling
rate via the SOC_ADC_SAMPLE_FREQ_THRES_HIGH macro, at about 83,333 KHz. The 
application works by utilizing the driver to sample 256 sized blocks of uint8_t
sampling data, which are then converted to useful uint32_t data. This requires
the sampling to be performed (N*4)/256 times to fill a N sized array of uint32_t
data.


### 

