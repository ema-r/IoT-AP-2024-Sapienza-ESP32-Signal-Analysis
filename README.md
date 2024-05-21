# _IoT Algorithms and Services Sapienza 23/24 - Signal Analysis_

The following repository contains the coursework project for the 23/34 edition
of the IoT Algorithms and Services course held in Sapienza University of Rome.

The code is a small ESP32 based application that performs some simple analysis
of an analog input signal and sends the resulting data to a broker employing the
MQTT protocol.

```
idf.py set-target <esp32 platform> && idf.py -p /dev/ttyUSB<port_num> flash monitor
```
The system will expect a signal on the pin corresponding to ADC_CHANNEL_0.

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

Due to the operations needed between each time interval, the sampling frequency
drops, and can reach only ~64-68 KHz, compared to the 83,3 KHz for the non-interrupted sampling. This can be observed by defining the FORCE_MAX_SAMPL_FREQ macro, that forces the "optimal" sampling loop to operate at the max frequency.
If operating under the 64 KHz roof, the interval callback doesn't influence
the sampling rate.

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

### Transmission to MQTT Server and latency
During the timer callback, the program sends the calculated value to the
configured MQTT broker. The MQTT protocol is secured by TLS: while this adds
overall communication costs, the most of it is during the single TLS handshake
while the connection is estabilished, and the advantages in security are well
worth it. Mutual TLS authentication was however not implemented, resorting to
(encrypted) username and password pairs to authenticate to the broker.

To perform MQTT operations, the ESP MQTT client was utilized. The client
requires, like most other ESP libraries, an initialization with a argument
struct, and an event loop callback function that handles the events sent by the
MQTT client. The default event loop is shared between Wi-fi and MQTT, as the MQTT client is tricky to configure to utilize any other loop. They do employ
2 different event handlers however. 

Given that we only need to send data to the broker for this application, the
MQTT event handler only takes care of connection/disconnect events, and ACKs
for published messages with QoS > 0.

Connection from local network to wider internet is handled via Wi-Fi.
Configuration is done via the usual order: arg structs are create, and then
the ESP driver is initialized with the given structs. We also initialize IP, so
that we can carry TCP/IP data over our Wi-Fi connection. We create another event
handler that takes care of TCP/IP and Wi-Fi events, discriminating on a
"event_base" basis. If the Wi-Fi connection fails, the event callback will be
activated and a a reconnection will be attempted. If a connection is not
established yet, a connection will be attempted.
IP events will be handled to make sure that the connection is correctly
established and an IP address for the device is obtained.

RTT is calculated via a global mqtt_clock variable that operates by taking a
snapshot of the UNIX clock when MQTT is published during the timer interrupt.
This is used to calculate the time passed when the ACK is received by the MQTT
event handler.
When the connection is first estabilished, the RTT quickly drops from 4 seconds
to the average value of about 0.4-0.3 seconds per MQTT publish operation. The
reason for the initial delay is due to eventual enqueued packets that have been
added while the actual MQTT or Wi-Fi connection is finishing the setup. The RTT
for each publish operation is printed to console if the device is monitored, but
requires publish QoS to be set over 0, as it requires ACKs to function.

## System Performance
### Latency
As mentioned beforehand, the system operates by sending aggreagated data over
MQTTS every 5 seconds. This data is generated by a simple division, and then
sent over via MQTT. The latency was analyzed for a local edge router and a remote
cloud server, the former obviously offering more useful data by allowing a simple
sniffing operation; the latency data for the cloud server had to be
extrapolated from ACK arrival times instead.
#### Remote cloud server
Taking the timer callback as the generation of the input
and optimistically assuming that the time to reach the server to be half the RTT
the latency on the system can vary from a almost 3 second long wait while the
system processes the initial enqueued inputs, then stabilizing closer to about
0.3 seconds.
#### Local edge router
In this case, the latency is incredibly minor, with delays between the ESP32
nodes publishing a message and the broker producing an ACK to be in the order of ~5
milliseconds or less.

![image](https://github.com/ema-r/IoT-AP-2024-Sapienza-ESP32-Signal-Analysis/assets/103453667/0fa34fb0-789e-448b-bde9-b6147d758ff5)
![image](https://github.com/ema-r/IoT-AP-2024-Sapienza-ESP32-Signal-Analysis/assets/103453667/7a1750bc-050c-46ff-a515-621011cd5271)
![image](https://github.com/ema-r/IoT-AP-2024-Sapienza-ESP32-Signal-Analysis/assets/103453667/96a738aa-03d0-4551-a52a-0368ffebaedd)

However, this pretty much represents the best case scenario for this experiment:
the ESP32 device was operating on a not too busy Wi-Fi network, with the edge
router particularly close by. It's likely that in a real scenario, the latency
could vastly increase.
It's worth noting that the local edge router test had to
be performed unencrypted due to platform limitation. While the effect of this
decision are vastly negligible in this section, it's worth pointing out as this
capture doesn't depict the initial latency caused by the TLS handshake. In this,
unencrypted case, the initialization of the connection is in the hundredths of a
second.

### Energy Savings
#### Sampling frequency
#### Networking costs.

### Packet size
The MQTT data is sent over TLS; this means some slight overhead in terms of
transmitted data, especially in the thankfully rare evenience that requires
setting or resetting the connection.
