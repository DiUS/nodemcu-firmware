/******************************************************************************
 * Copyright 2013-2014 Espressif Systems (Wuxi)
 *
 * FileName: user_main.c
 *
 * Description: entry file of user application
 *
 * Modification history:
 *     2014/1/1, v1.0 create this file.
*******************************************************************************/
#include "lua.h"
#include "platform.h"
#include "c_string.h"
#include "c_stdlib.h"
#include "c_stdio.h"

#include "flash_fs.h"
#include "user_interface.h"

#include "ets_sys.h"
#include "driver/uart.h"
#include "mem.h"

#include "main.h"

#define SIG_LUA 0
#define TASK_QUEUE_LEN 4
os_event_t *taskQueue;

void task_lua(os_event_t *e){
    char* lua_argv[] = { (char *)"lua", (char *)"-i", NULL };
    NODE_DBG("Task task_lua started.\n");
    switch(e->sig){
        case SIG_LUA:
            NODE_DBG("SIG_LUA received.\n");
            lua_main( 2, lua_argv );
            break;
        default:
            break;
    }
}

void task_init(void){
    taskQueue = (os_event_t *)os_malloc(sizeof(os_event_t) * TASK_QUEUE_LEN);
    system_os_task(task_lua, USER_TASK_PRIO_0, taskQueue, TASK_QUEUE_LEN);
}

// extern void test_spiffs();
// extern int test_romfs();

// extern uint16_t flash_get_sec_num();


//
// Layout of the RTC storage space for temp sensor:
//
// 0: Magic. If set to 1820, the rest is valid. If not, simply boot into LUA for initialization
// 1: time_of_day, seconds. This is the at-bootup TOD, so the RTC needs to be added
// 2: time_of_day, microseconds.
// 3: measurement alignment, in microseconds
// 4: storage location for next sample. Increases by 2 each time we store a sample
// 5: timestamp for next sample (seconds). Stored when we kick off the conversion. If 0, no sample is waiting
// 6: fill threshold. If (4) at bootup is at or past this value, boot into LUA
// 7: fifo top. If (4) moved to or past this value, stop storing samples
// 8: Debug data
// 9: Whether radio is on(1) or off(0).
// ....
// Samples:
// n: timestamp (seconds)
// n+1: [24:31]='T' for temperature, [0:15]=temperature, in Celsius*100, signed. I.e. for 25.34C, stored 2534
//

#define RTC_POS(x) ((x)+64)
#define RTC_READ(val,pos) system_rtc_mem_read(pos##_POS,&(val),4)
#define RTC_WRITE(val,pos) system_rtc_mem_write(pos##_POS,&(val),4)
#define RTC_WRITE_ADDR(val,pos) system_rtc_mem_write(RTC_POS(pos),&(val),4)

#define MAGIC_POS RTC_POS(0)
#define TOD_S_POS RTC_POS(1)
#define TOD_US_POS RTC_POS(2)
#define ALIGN_POS RTC_POS(3)
#define SAMPLE_LOC_POS RTC_POS(4)
#define SAMPLE_TS_POS RTC_POS(5)
#define THRESHOLD_POS RTC_POS(6)
#define FIFO_TOP_POS RTC_POS(7)
#define DEBUG_POS RTC_POS(8)
#define RADIO_ON_POS RTC_POS(9)

u32 get_rtc_us(void)
{
  uint32_t t = (uint32_t)system_get_rtc_time();
  uint32_t c = system_rtc_clock_cali_proc();
  uint32_t itg = c >> 12;  // ~=5
  uint32_t dec = c & 0xFFF; // ~=2ff

  return (t*itg + ((t>>12)*dec));
}

u64 get_tod_us(void)
{
    u32 s,us;

    RTC_READ(s,TOD_S);
    RTC_READ(us,TOD_US);
    u64 todus=(((u64)s)*1000000)+us+get_rtc_us();
    return todus;
}

void set_tod_us(u64 todus,u32 rtc_us)
{
    todus-=rtc_us;
    u32 s=todus/1000000;
    u32 us=todus%1000000;

    RTC_WRITE(s,TOD_S);
    RTC_WRITE(us,TOD_US);
}

#define DS1820_PIN 7
#define DS1820_CONVERSION_US 750000

#define DEEP_SLEEP_ENTRY_DELAY_US 368500
static u8 ds1820_start_conversion(void)
{
    onewire_reset(DS1820_PIN);
    onewire_skip(DS1820_PIN);
    onewire_write(DS1820_PIN,0x44,1); // start conversion
    return 1;
}

#define FAIL do { failblog=__LINE__; goto fail;} while (0)

// This assumes that the RTC data is valid!
// Use immediate if all you want to do is switch the radio on/off, rather than wait for
// the next sample slot
void enter_1820_deep_sleep(u32 with_radio, u8 immediate, u32 min_sleep)
{
    system_deep_sleep_set_option(with_radio?1:4);
    RTC_WRITE(with_radio,RADIO_ON);

    u64 us=get_tod_us();
    u32 align;
    RTC_READ(align,ALIGN);
    u64 target=(us+align-1);
    target-=(target%align);
    s32 sleep_us=target-us;
    if (immediate)
        sleep_us=1;
    if (sleep_us<min_sleep)
        sleep_us=min_sleep;
    sleep_us-=DEEP_SLEEP_ENTRY_DELAY_US;
    set_tod_us(target,0);
    RTC_WRITE(sleep_us,DEBUG);

    if (sleep_us<=0)
        sleep_us=1;
    system_deep_sleep(sleep_us);
}

static u8 ds1820_read_value(s16* value)
{
    onewire_reset(DS1820_PIN);
    onewire_skip(DS1820_PIN);
    onewire_write(DS1820_PIN,0xBE,1); // read scratchpad
    u8 data[9];

    onewire_read_bytes(DS1820_PIN,data,9);
    u8 crc=onewire_crc8(data,8);
    if (crc!=data[8])
        return 0;
    int32 t=data[0]+256*data[1];
    if (t>0x7fff)
        t-=0x10000;

    t*=625;
    t/=100;

    *value=t;
    return 1;
}


static void ds1820_setup(void)
{
    onewire_init(DS1820_PIN);
    // Default setup is exactly what we want
}

static void handle_1820(void)
{
    u32 sample_timestamp;
    u32 sample_location;
    u32 threshold;
    u16 failblog=0;

    if (!RTC_READ(sample_timestamp,SAMPLE_TS))
        FAIL;
    if (!RTC_READ(sample_location,SAMPLE_LOC))
        FAIL;
    if (!RTC_READ(threshold,THRESHOLD))
        FAIL;

    u8 enter_lua=(sample_location>=threshold);
    ds1820_setup();
    if (sample_timestamp)
    {
        u32 sample;
        u32 max_location;

        if (!RTC_READ(max_location,FIFO_TOP))
            FAIL;
        if (sample_location+2<=max_location)
        {
            s16 temp;
            if (!ds1820_read_value(&temp))
                FAIL;
            sample=(u32)((u16)temp);
            u32 to_write=sample|(((u32)'T')<<24);
            if (!RTC_WRITE_ADDR(sample_timestamp,sample_location))
                FAIL;
            if (!RTC_WRITE_ADDR(to_write,sample_location+1))
                FAIL;

            sample_location+=2;
            if (!RTC_WRITE(sample_location,SAMPLE_LOC))
                FAIL;
        }
    }

    u32 have_radio;
    if (!RTC_READ(have_radio,RADIO_ON))
        FAIL;
    uint8_t want_radio=sample_location>=threshold;
    // If we currently have the radio on, we can't do a conversion. So we need to go to sleep
    // until the next appropriate conversion point, and wake up without radio.
    // If we currently don't have the radio on, we can do a conversion, and sleep either long
    // enough for it to complete (if we actually want the radio), or until the next conversion
    // point (if the radio is meant to stay off)
    if (!have_radio)
    {
        // Store the timestamp of the about-to-be-started conversion
        u32 now;
        if (!RTC_READ(now,TOD_S))
            FAIL;
        if (!RTC_WRITE(now,SAMPLE_TS))
            FAIL;
        if (!ds1820_start_conversion())
            FAIL;
    }
    else
    { // Don't start a conversion
        u32 now=0;
        if (!RTC_WRITE(now,SAMPLE_TS))
            FAIL;
    }

    if (!(enter_lua && have_radio))
    {
        if (have_radio)
            enter_1820_deep_sleep(0,0,0); // Radio off, wake up at conversion point, no minimum
        else
            enter_1820_deep_sleep(want_radio,want_radio,DS1820_CONVERSION_US);
    }
    return;

 fail:
    {
        u32 badmagic=1000000+failblog;
        RTC_WRITE(badmagic,MAGIC);
        return;
    }
}

void nodemcu_init(void)
{
    NODE_ERR("\n");

    u32 magic;
    if (system_rtc_mem_read(64,&magic,4) && magic==1820)
    {
        handle_1820();
    }

    // Initialize platform first for lua modules.
    if( platform_init() != PLATFORM_OK )
    {
        // This should never happen
        NODE_DBG("Can not init platform for modules.\n");
        return;
    }

#if defined(FLASH_SAFE_API)
    if( flash_safe_get_size_byte() != flash_rom_get_size_byte()) {
        NODE_ERR("Self adjust flash size.\n");
        // Fit hardware real flash size.
        flash_rom_set_size_byte(flash_safe_get_size_byte());
        // Flash init data at FLASHSIZE - 0x04000 Byte.
        flash_init_data_default();
        // Flash blank data at FLASHSIZE - 0x02000 Byte.
        flash_init_data_blank();
        if( !fs_format() )
        {
            NODE_ERR( "\ni*** ERROR ***: unable to format. FS might be compromised.\n" );
            NODE_ERR( "It is advised to re-flash the NodeMCU image.\n" );
        }
        else{
            NODE_ERR( "format done.\n" );
        }
        fs_unmount();   // mounted by format.
    }
#endif // defined(FLASH_SAFE_API)

    if( !flash_init_data_written() ){
        NODE_ERR("Restore init data.\n");
        // Flash init data at FLASHSIZE - 0x04000 Byte.
        flash_init_data_default();
        // Flash blank data at FLASHSIZE - 0x02000 Byte.
        flash_init_data_blank(); 
    }

#if defined( BUILD_WOFS )
    romfs_init();

    // if( !wofs_format() )
    // {
    //     NODE_ERR( "\ni*** ERROR ***: unable to erase the flash. WOFS might be compromised.\n" );
    //     NODE_ERR( "It is advised to re-flash the NodeWifi image.\n" );
    // }
    // else
    //     NODE_ERR( "format done.\n" );

    // test_romfs();
#elif defined ( BUILD_SPIFFS )
    fs_mount();
    // test_spiffs();
#endif
    // endpoint_setup();

    // char* lua_argv[] = { (char *)"lua", (char *)"-e", (char *)"print(collectgarbage'count');ttt={};for i=1,100 do table.insert(ttt,i*2 -1);print(i);end for k, v in pairs(ttt) do print('<'..k..' '..v..'>') end print(collectgarbage'count');", NULL };
    // lua_main( 3, lua_argv );
    // char* lua_argv[] = { (char *)"lua", (char *)"-i", NULL };
    // lua_main( 2, lua_argv );
    // char* lua_argv[] = { (char *)"lua", (char *)"-e", (char *)"pwm.setup(0,100,50) pwm.start(0) pwm.stop(0)", NULL };
    // lua_main( 3, lua_argv );
    // NODE_DBG("Flash sec num: 0x%x\n", flash_get_sec_num());
    task_init();
    system_os_post(USER_TASK_PRIO_0,SIG_LUA,'s');
    gpio_output_set(BIT12,0,BIT12,0);
}

/******************************************************************************
 * FunctionName : user_init
 * Description  : entry of user application, init user function here
 * Parameters   : none
 * Returns      : none
*******************************************************************************/

uint32_t startup_rtc;
uint32_t my_secret_data=0x12345678;
const uint32_t __attribute__ ((section ("my_test"))) my_testy_test2[16]={87654321,};
const uint32_t my_testy_test=12345678;

void user_init(void)
{
    if (!my_secret_data)
        return;

    // NODE_DBG("SDK version:%s\n", system_get_sdk_version());
    // system_print_meminfo();
    // os_printf("Heap size::%d.\n",system_get_free_heap_size());
    // os_delay_us(50*1000);   // delay 50ms before init uart
    startup_rtc = (uint32_t)system_get_rtc_time();

#ifdef DEVELOP_VERSION
    uart_init(BIT_RATE_74880, BIT_RATE_74880);
#else
    uart_init(BIT_RATE_115200, BIT_RATE_115200);
#endif
    // uart_init(BIT_RATE_115200, BIT_RATE_115200);

    #ifndef NODE_DEBUG
    system_set_os_print(0);
    #endif

    system_init_done_cb(nodemcu_init);
}
