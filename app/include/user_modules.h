#ifndef __USER_MODULES_H__
#define __USER_MODULES_H__

#define LUA_USE_BUILTIN_STRING		// for string.xxx()
#define LUA_USE_BUILTIN_TABLE		// for table.xxx()
#define LUA_USE_BUILTIN_COROUTINE	// for coroutine.xxx()
#define LUA_USE_BUILTIN_MATH		// for math.xxx(), partially work
#define LUA_USE_BUILTIN_DEBUG_MINIMAL // for debug.getregistry() and debug.traceback()

#ifndef LUA_CROSS_COMPILER

#define LUA_USE_MODULES_ADC
#define LUA_USE_MODULES_BIT
#define LUA_USE_MODULES_CRYPTO
#define LUA_USE_MODULES_ENCODER
#define LUA_USE_MODULES_ENDUSER_SETUP
#define LUA_USE_MODULES_FILE
#define LUA_USE_MODULES_FLASHFIFO
#define LUA_USE_MODULES_GPIO
#define LUA_USE_MODULES_HTTP
#define LUA_USE_MODULES_NET
#define LUA_USE_MODULES_NODE
#define LUA_USE_MODULES_OTAUPGRADE
#define LUA_USE_MODULES_RTCMEM
#define LUA_USE_MODULES_S4PP
#define LUA_USE_MODULES_SJSON
#define LUA_USE_MODULES_TMR
#define LUA_USE_MODULES_UART
#define LUA_USE_MODULES_WIFI

#endif  /* LUA_CROSS_COMPILER */
#endif	/* __USER_MODULES_H__ */
