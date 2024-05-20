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

Time is measured both for the initial oversampling, performed to get data to feed
to the FFT to calculate the optimal max frequency, and another "dry run",
with no operations during sampling, is
performed to get another time estimate, with a lot of extra samples. This is
used to get the actual sampling
frequency, utilizing standard UNIX clock() functions and the number of samples
taken. The actual resolution of the clocks in this solution however often
results in incorrect, over-the-maximum calculations, as the time taken is lower
than the minimum clock resolution. In this case, the actual maximum frequency
is assumed to be the same as SOC_ADC_SAMPLE_FREQ_THRES_HIGH.

### Lower frequency sampling
The optimal frequency is obtained by analyzing the signal's Fourier's transform,
obtained via the FFT implementation offered by the ESP-DSP library. FFT Radix 2
is utilized, despite Radix 4 being available and employing roughly 30% less
clock cycles than Radix 2, as the FFT4R implementation offered inconsistent
results during unit testing despite utilizing the correct number of samples. For
the sake of turning the project in before the deadline the student decide to
utilize the more consistent FFT2R.

The sampled data is split in Re(), Im() pairs in a (2*N) size array to perform
the FFT operations. Once the Fourier transform is obtained, the first N real
half spectrum is considered. To get the optimal frequency, we get the first
"good" frequency found by going over the FT array "right-to-left". What
constitutes a good frequency is obtained by calculating mean and standard
deviation on the FFT samples. If an FFT sample passes mean + (std dev * a finetunable sensitivity value), we choose that sample index. We use that index
to get the actual frequency, by multiplying it for the sampling frequency and
dividing by the size of the FFT sample block (N*2).

The adc_continuous_driver is re-initialized with the new sampling frequency,
operating in a continuous loop. Data is not saved to an array in this case:
we only need an average so we sum it to a "sum" value and we increase a sample
counter value.

### Function aggregation over a window
As mentioned in the previous section, the function aggregate is performed via a
moving window average, that operates on the number of samples taken during 5
seconds intervals. The timer is implemented via ESP timer callback functions: a
periodic timer is initialized that interrupts for the callback every 5 seconds,
and then the optimal frequency sampling loop is started. The sampling operation
sums value to a global sum value, and a global sample counter is increased. The
callback function could have also taken a pointer to a struct to pass arguments
in, but the global variable solution was chosen for immediate implementation
simplicity in spite of general "code smell" it causes. When the callback is
called, the average is calculated and then sent over via MQTTS. Given that the
sampling only writes to these values, and the callback only reads from them to
obtain an average value, no mutex or any other kind of exclusive access data
structure was needed.


