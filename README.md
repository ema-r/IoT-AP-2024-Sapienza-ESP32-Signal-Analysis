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
drops, and can reach only ~67-69 KHz, compared to the 83,3 KHz for the non-interrupted sampling. This can be observed by defining the FORCE_MAX_SAMPL_FREQ macro, that forces the "optimal" sampling loop to operate at the max frequency.
If operating under a 64 KHz roof, the interval callback doesn't influence
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
decision are vastly negligible in this section, barring the increased latency
thanks to the need of symmetric encryption for session data, it's worth pointing out as this capture doesn't depict the initial latency "spike" caused by the TLS handshake. In this, unencrypted case, the initialization of the connection is in the hundredths of a second.

### Energy Savings
The energy analysis was performed via an INA 219 device connected to another
ATMEGA board (gently lent by another student). Three tests were performed: two
with the network functionalities of the project disabled to just gauge the
energy cost for sampling and analysis of data at max and optimized sampling
frequency (forced 1/10 of the max sampling frequency for this test), plus another test with optimized sampling frequency (again, 1/10 of max) and network
functionalities working to gauge the energy cost increase when operating
(securely) on a wireless network. The INA output was sampled every 2 seconds.
The main focus of the sampling operation was the final loop in the main
function, that simply samples as much ADC data as it can before the timer
callback is called to perform an average.
#### Sampling frequency
The two tests were run respectively at "as-fast-as-possible" sampling, which
produced the aforementioned 65 KHz sampling frequency when performing other
calculations, and at a lowered (1/10 of SOC_ADC_SAMPLE_THRES_HIGH, resulting in about 8 KHz) frequency, to check for any kind of relationship between the drop in sampling rate and in power usage.

At Max sampling rate, the device averaged a current draw of 50 mA, with periodic
decreases to 45, likely triggered by the timer callback function activating
and disrupting the high speed sampling. After the drop the current draw rapidly
rises back to 50 mA

```
Bus Voltage: 1.06 V
Shunt Voltage: 5.12 mV
Load Voltage: 1.07 V
Current: 50.70 mA
Power: 52.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.55 mV
Load Voltage: 1.06 V
Current: 50.90 mA
Power: 52.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.92 mV
Load Voltage: 1.06 V
Current: 51.30 mA
Power: 52.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.47 mV
Load Voltage: 1.06 V
Current: 45.80 mA
Power: 50.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.48 mV
Load Voltage: 1.06 V
Current: 46.30 mA
Power: 52.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.48 mV
Load Voltage: 1.06 V
Current: 49.90 mA
Power: 52.00 mW
```

The "optimal" sampling rate on the other hand produces a consistent, even if
not particularly major drop in current draw: sitting at a constant 45 mA

```
Bus Voltage: 1.06 V
Shunt Voltage: 5.07 mV
Load Voltage: 1.07 V
Current: 44.50 mA
Power: 46.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.48 mV
Load Voltage: 1.06 V
Current: 44.70 mA
Power: 46.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.47 mV
Load Voltage: 1.07 V
Current: 45.00 mA
Power: 46.00 mW
```

The drop of "just" 5 mA during pure sampling suggests that at this rate, the
device consumes just about the same power as it does during the timer interrupt,
i.e. the energy cost of sampling becomes negligible when compared to the general
current draw of the device. Another, sampling-less run of the system could've
confirmed or dismissed this hypothesis.

#### Networking costs.
The energy costs of the "full" system, networking included, is far more
interesting. While obviously the current drain increases, the way it does so
vastly depends on the status of the Wi-Fi client, and the MQTTS broker
connection. If we run the program with the INA "listening" we get various big
current draw spikes that happen to roughly correspond to the ESP32 attempting to
join the wireless network, and opening a connection with the broker (an
operation which involves a relatively costly TLS handshake).

```
Bus Voltage: 1.06 V
Shunt Voltage: 5.96 mV
Load Voltage: 1.07 V
Current: 59.30 mA
Power: 62.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 11.73 mV
Load Voltage: 1.07 V
Current: 112.40 mA
Power: 118.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.95 mV
Load Voltage: 1.06 V
Current: 48.90 mA
Power: 52.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 11.77 mV
Load Voltage: 1.07 V
Current: 110.80 mA
Power: 118.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.96 mV
Load Voltage: 1.06 V
Current: 49.10 mA
Power: 52.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.89 mV
Load Voltage: 1.06 V
Current: 49.70 mA
Power: 52.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 11.77 mV
Load Voltage: 1.07 V
Current: 127.90 mA
Power: 134.00 mW

Bus Voltage: 1.06 V
Shunt Voltage: 4.86 mV
Load Voltage: 1.06 V
Current: 54.70 mA
Power: 52.00 mW
```
Once the initial setup is done, the current drain subsides at around 48 to 55
mA, a variation likely depending on the device elaborating and transmitting
data to the broker. The event loops needed for TCP/IP, Wi-Fi and MQTT handling
also likely take a toll on the power consumption, considering the "base" value
with other operations going on has increased by 3 mA between this and the 
connectionless optimized sampling loop.

### Amount of data sent
The MQTT data is sent over TLS; this means some slight overhead in terms of
transmitted data, especially in the thankfully rare evenience that requires
setting or resetting the connection. Sadly, the platform utilized as an MQTT
broker prevented easy collection of network data to get some concrete information on datagram sizes: the standard cloud solution doesn't allow for
sniffing obviously, and the free trial of the locally run instance isn't setup
OOTB for TLS.
As a result the gathered data is only significant for a non-TLS utilizing
instance of the program.
TLS would obviously add some (well worth it) overhead, starting with the
~6 Kb worth of handshake data. With the handshake out of the way, the protocol
will add some bytes worth of header data.

Each MQTT publish message will be around 70 to 80 bytes worth of data, with 
the last 20 containing the MQTT data: 1 byte of header flags, 1 byte of message length, topic and topic length (4 and 2 bytes here), then the 2 byte message
id and a variable length of message (it is a encoded as a string during publishing).

MQTTs ACKs (optional depending on QoS settings) save about ~16 bytes by only
containing info on what message they refer to at MQTT level.

The MQTT CONN and ACK messages are similiarly succinct, with the CONN message
only adding 26 bytes on top of the chosen lower layer protocols, distributing
some basic information and data about the device initiating the connection; some
flags define the characteristics of the requested connection.

