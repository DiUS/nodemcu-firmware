#ifndef _OVERRIDE_C_TYPES_H_
#define _OVERRIDE_C_TYPES_H_

// Get rid of the c_types.h nonsense once and for all, and use proper C types.
#define _C_TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef long long int64_t;

typedef int8_t  sint8_t;
typedef int16_t sint16_t;
typedef int32_t sint32_t;
typedef int64_t sint64_t;

typedef uint8_t   uint8;
typedef uint8_t   u8;
typedef int8_t    sint8;
typedef int8_t    int8;
typedef int8_t    s8;
typedef uint16_t  uint16;
typedef uint16_t  u16;
typedef int16_t   sint16;
typedef int16_t   s16;
typedef uint32_t  uint32;
typedef uint32_t  u_int;
typedef uint32_t  u32;
typedef int32_t   sint32;
typedef int32_t   s32;
typedef int32_t   int32;
typedef int64_t   sint64;
typedef uint64_t  uint64;
typedef uint64_t  u64;

// Miscellanea copied from c_types.h below

#define __le16      u16

#define __packed        __attribute__((packed))

#define LOCAL       static

/* probably should not put STATUS here */
typedef enum {
    OK = 0,
    FAIL,
    PENDING,
    BUSY,
    CANCEL,
} STATUS;

#define BIT(nr)                 (1UL << (nr))

#define REG_SET_BIT(_r, _b)  (*(volatile uint32_t*)(_r) |= (_b))
#define REG_CLR_BIT(_r, _b)  (*(volatile uint32_t*)(_r) &= ~(_b))

#define DMEM_ATTR __attribute__((section(".bss")))
#define SHMEM_ATTR

#ifdef ICACHE_FLASH
#define ICACHE_FLASH_ATTR __attribute__((section(".irom0.text")))
#define ICACHE_RODATA_ATTR __attribute__((section(".irom.text")))
#else
#define ICACHE_FLASH_ATTR
#define ICACHE_RODATA_ATTR
#endif /* ICACHE_FLASH */

#define STORE_ATTR __attribute__((aligned(4)))

#define TRUE            true
#define FALSE           false

#endif
