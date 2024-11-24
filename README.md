# Phev Remote Core Library 

[![Build Status](https://travis-ci.com/phev-remote/phevcore.svg?branch=master)](https://travis-ci.com/phev-remote/phevcore) [![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A library that can be used to communicate with the Mistushish Outlander PHEV with the Remote WiFi option in the MMC so mainly the GH4 + models.

Device registration and connecting to the car is functional as well as being able to read the car data and swtich on the lights and manual air conditioning.

More features will be avalable soon.

It requires that the device is connected to the same network as the car, the 192.168.8.x subnet.

### The CLI 

Has been tested on a raspberry pi and other linux systems can be found here.

https://github.com/phev-remote/phevctl

### Pre-reqs

#### Messaging core library

```
git clone https://github.com/apra00/msg-core
cd msg-core
mkdir -p build
cd build
cmake ..
make
sudo make install
```
#### cJSON
```
git clone https://github.com/apra00/cJSON
cd cJSON
mkdir -p build
cd build
cmake ..
make
sudo make install
```
### Build instructions
```
git clone https://github.com/apra00/phevcore
cd phevcore
mkdir -p build
cd build
cmake ..
make
sudo make install
```

