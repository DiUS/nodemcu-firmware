# **NodeMCU 1.5.1** #

[![Join the chat at https://gitter.im/nodemcu/nodemcu-firmware](https://img.shields.io/gitter/room/badges/shields.svg)](https://gitter.im/nodemcu/nodemcu-firmware?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)
[![Build Status](https://travis-ci.org/nodemcu/nodemcu-firmware.svg)](https://travis-ci.org/nodemcu/nodemcu-firmware)
[![Documentation Status](https://readthedocs.org/projects/nodemcu/badge/?version=dev)](http://nodemcu.readthedocs.org/)
[![License](https://img.shields.io/badge/license-MIT-blue.svg?style=flat)](https://github.com/nodemcu/nodemcu-firmware/blob/master/LICENSE)

### A Lua based firmware for ESP8266 WiFi SOC

NodeMCU is an [eLua](http://www.eluaproject.net/) based firmware for the [ESP8266 WiFi SOC from Espressif](http://espressif.com/en/products/esp8266/). The firmware is based on the [Espressif NON-OS SDK 1.5.1](http://bbs.espressif.com/viewtopic.php?f=46&p=5315) and uses a file system based on [spiffs](https://github.com/pellepl/spiffs). The code repository consists of 98.1% C-code that glues the thin Lua veneer to the SDK.

The NodeMCU *firmware* is a companion project to the popular [NodeMCU dev kits](https://github.com/nodemcu/nodemcu-devkit-v1.0), ready-made open source development boards with ESP8266-12E chips.

# Summary

- Easy to program wireless node and/or access point
- Based on Lua 5.1.4 (without *debug, os* modules)
- Asynchronous event-driven programming model
- 35+ [built-in modules](https://github.com/nodemcu/nodemcu-firmware/wiki/Module-list)
- Firmware available with or without floating point support (integer-only uses less memory)
- Up-to-date documentation at [https://nodemcu.readthedocs.org](https://nodemcu.readthedocs.org)

# Programming Model

The NodeMCU programming model is similar to that of [Node.js](https://en.wikipedia.org/wiki/Node.js), only in Lua. It is asynchronous and event-driven. Many functions, therefore, have parameters for callback functions. To give you an idea what a NodeMCU program looks like study the short snippets below. For more extensive examples have a look at the [`/lua_examples`](lua_examples) folder in the repository on GitHub.

```lua
-- a simple HTTP server
srv = net.createServer(net.TCP)
srv:listen(80, function(conn)
	conn:on("receive", function(conn, payload)
		print(payload)
		conn:send("<h1> Hello, NodeMCU.</h1>")
	end)
	conn:on("sent", function(conn) conn:close() end)
end)
```
```lua
-- connect to WiFi access point
wifi.setmode(wifi.STATION)
wifi.sta.config("SSID", "password")
```

# Documentation

The entire [NodeMCU documentation](https://nodemcu.readthedocs.org) is maintained right in this repository at [/docs](docs). The fact that the API documentation is mainted in the same repository as the code that *provides* the API ensures consistency between the two. With every commit the documentation is rebuilt by Read the Docs and thus transformed from terse Markdown into a nicely browsable HTML site at [https://nodemcu.readthedocs.org](https://nodemcu.readthedocs.org). 

- How to [build the firmware](https://nodemcu.readthedocs.org/en/dev/en/build/)
- How to [flash the firmware](https://nodemcu.readthedocs.org/en/dev/en/flash/)
- How to [upload code and NodeMCU IDEs](https://nodemcu.readthedocs.org/en/dev/en/upload/)
- API documentation for every module

# Support

See [https://nodemcu.readthedocs.org/en/dev/en/support/](https://nodemcu.readthedocs.org/en/dev/en/support/).

# License

[MIT](https://github.com/nodemcu/nodemcu-firmware/blob/master/LICENSE) © [zeroday](https://github.com/NodeMCU)/[nodemcu.com](http://nodemcu.com/index_en.html)

# Build Options

The following sections explain some of the options you have if you want to [build your own NodeMCU firmware](http://nodemcu.readthedocs.org/en/dev/en/build/).

### Select Modules

Disable modules you won't be using to reduce firmware size and free up some RAM. The ESP8266 is quite limited in available RAM and running out of memory can cause a system panic. 

Edit `app/include/user_modules.h` and comment-out the `#define` statement for modules you don't need. Example:

```c
...
#define LUA_USE_MODULES_MQTT
// #define LUA_USE_MODULES_COAP
// #define LUA_USE_MODULES_U8G
...
```

### Tag Your Build

Identify your firmware builds by editing `app/include/user_version.h`

```c
#define NODE_VERSION    "NodeMCU 1.5.1+myname"
#ifndef BUILD_DATE
#define BUILD_DATE      "YYYYMMDD"
#endif
```

### Set UART Bit Rate

The initial baud rate at boot time is 9600bps. You can change this by
editing `BIT_RATE_DEFAULT`  in `app/include/user_config.h`:

```c
#define BIT_RATE_DEFAULT BIT_RATE_115200
```

### Debugging

To enable runtime debug messages to serial console edit `app/include/user_config.h`

```c
#define DEVELOP_VERSION
```

`DEVELOP_VERSION` changes the startup baud rate to 74880bps.
