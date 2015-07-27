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
#include "rtctime.h"

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



void nodemcu_init(void)
{
    NODE_ERR("\n");

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

void user_init(void)
{
    // NODE_DBG("SDK version:%s\n", system_get_sdk_version());
    // system_print_meminfo();
    // os_printf("Heap size::%d.\n",system_get_free_heap_size());
    // os_delay_us(50*1000);   // delay 50ms before init uart

    rtc_time_register_bootup();
    rtc_time_switch_system();

#ifdef DEVELOP_VERSION
    uart_init(BIT_RATE_74880, BIT_RATE_74880);
#else
    uart_init(BIT_RATE_74880, BIT_RATE_74880);
    // uart_init(BIT_RATE_115200, BIT_RATE_115200);
#endif
    // uart_init(BIT_RATE_115200, BIT_RATE_115200);

    #ifndef NODE_DEBUG
    // system_set_os_print(0);
    #endif

    system_init_done_cb(nodemcu_init);
}

#if 0
uint32_t __wrap_pm_rtc_clock_cali(void)
{
    uint32_t answer=__real_pm_rtc_clock_cali();

    uint32_t* pm_data=(uint32_t*)0x3ffee0e0
    // os_printf("answer %d, pd=%d\n",answer,pm_data[1]);

    return answer;
}
#endif

#if 0
uint32_t (*their_fn)(void* arg)=NULL;

#define COUNT 32
static uint32_t in[COUNT];
static uint32_t out[COUNT];
static pos=0;

uint32_t my_fn(void* arg)
{
    in[pos]=xthal_get_ccount();
    if (their_fn)
        their_fn(arg);
}

uint32_t __wrap_ets_set_idle_cb(void* fn, void* arg)
{
    their_fn=fn;
    // c_printf("idle_cb(%p,%p)\n",fn,arg);
    if (fn)
        return __real_ets_set_idle_cb(my_fn,arg);
    else
    {
        out[pos]=xthal_get_ccount();
        pos++;
        if (pos==COUNT)
        {
            int i;

            for (i=0;i<COUNT;i++)
            {
                uint32_t d=out[i]-in[i];
                c_printf("%9u -> %9u   (%9u)   %4u/%06u -> %4u/%06u (%4u/%06u)\n",
                         in[i],out[i],d,
                         (in[i]/80)/102400,(in[i]/80)%102400,
                         (out[i]/80)/102400,(out[i]/80)%102400,
                         (d/80)/102400,(d/80)%102400);
            }
            pos=0;
        }
        return __real_ets_set_idle_cb(NULL,arg);
    }

}
#endif
