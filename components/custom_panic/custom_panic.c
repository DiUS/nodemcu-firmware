////////////////////////////////
// The IDF panic/unhandled exception handler sometimes hangs in
// esp_ota_get_app_elf_sha256(), which prevents automatic reboot.
// As that is in the IDF, we can't (easily) patch it, so instead we
// provide alternative (simpler) handlers here and use the linker's
// ability to wrap the references
////////////////////////////////


#include "freertos/FreeRTOS.h"
#include "soc/cpu.h"
#include "soc/rtc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/uart_reg.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include "esp_private/panic_reason.h"
#include "esp32/dport_access.h"
#include "esp_rom_uart.h"

static void reconfigureAllWdts()
{
    TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
    TIMERG0.wdt_feed = 1;
    TIMERG0.wdt_config0.sys_reset_length = 7;           //3.2uS
    TIMERG0.wdt_config0.cpu_reset_length = 7;           //3.2uS
    TIMERG0.wdt_config0.stg0 = TIMG_WDT_STG_SEL_RESET_SYSTEM; //1st stage timeout: reset system
    TIMERG0.wdt_config1.clk_prescale = 80 * 500;        //Prescaler: wdt counts in ticks of 0.5mS
    TIMERG0.wdt_config2 = 2000;                         //1 second before reset
    TIMERG0.wdt_config0.en = 1;
    TIMERG0.wdt_wprotect = 0;
    //Disable wdt 1
    TIMERG1.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
    TIMERG1.wdt_config0.en = 0;
    TIMERG1.wdt_wprotect = 0;
}

static void panicPutChar(char c)
{
    while (((READ_PERI_REG(UART_STATUS_REG(CONFIG_ESP_CONSOLE_UART_NUM)) >> UART_TXFIFO_CNT_S)&UART_TXFIFO_CNT) >= 126) ;
    WRITE_PERI_REG(UART_FIFO_REG(CONFIG_ESP_CONSOLE_UART_NUM), c);
}

static void panicPutStr(const char *c)
{
    int x = 0;
    while (c[x] != 0) {
        panicPutChar(c[x]);
        x++;
    }
}

static void panicPutHex(int a)
{
    int x;
    int c;
    for (x = 0; x < 8; x++) {
        c = (a >> 28) & 0xf;
        if (c < 10) {
            panicPutChar('0' + c);
        } else {
            panicPutChar('a' + c - 10);
        }
        a <<= 4;
    }
}

static void panicPutDec(int a)
{
    int n1, n2;
    n1 = a % 10;
    n2 = a / 10;
    if (n2 == 0) {
        panicPutChar(' ');
    } else {
        panicPutChar(n2 + '0');
    }
    panicPutChar(n1 + '0');
}

static void haltOtherCore()
{
    esp_cpu_stall( xPortGetCoreID() == 0 ? 1 : 0 );
}

static void esp_panic_dig_reset()
{
    // make sure all the panic handler output is sent from UART FIFO
    esp_rom_uart_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);
    // switch to XTAL (otherwise we will keep running from the PLL)
    rtc_clk_cpu_freq_set_xtal();
    // reset the digital part
    esp_cpu_unstall(PRO_CPU_NUM);
    SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    while (true) {
        ;
    }
}

static void commonErrorHandler_dump(XtExcFrame *frame, int core_id)
{
    int *regs = (int *)frame;
    int x, y;
    const char *sdesc[] = {
        "PC      ", "PS      ", "A0      ", "A1      ", "A2      ", "A3      ", "A4      ", "A5      ",
        "A6      ", "A7      ", "A8      ", "A9      ", "A10     ", "A11     ", "A12     ", "A13     ",
        "A14     ", "A15     ", "SAR     ", "EXCCAUSE", "EXCVADDR", "LBEG    ", "LEND    ", "LCOUNT  "
    };

    reconfigureAllWdts();

    /* only dump registers for 'real' crashes, if crashing via abort()
       the register window is no longer useful.
    */
    panicPutStr("Core");
    panicPutDec(core_id);
    panicPutStr(" register dump:\r\n");

    for (x = 0; x < 24; x += 4) {
      for (y = 0; y < 4; y++) {
        if (sdesc[x + y][0] != 0) {
          panicPutStr(sdesc[x + y]);
          panicPutStr(": 0x");
          panicPutHex(regs[x + y + 1]);
          panicPutStr("  ");
        }
      }
      panicPutStr("\r\n");
    }
}

static const char *edesc[] = {
    "IllegalInstruction", "Syscall", "InstructionFetchError", "LoadStoreError",
    "Level1Interrupt", "Alloca", "IntegerDivideByZero", "PCValue",
    "Privileged", "LoadStoreAlignment", "res", "res",
    "InstrPDAddrError", "LoadStorePIFDataError", "InstrPIFAddrError", "LoadStorePIFAddrError",
    "InstTLBMiss", "InstTLBMultiHit", "InstFetchPrivilege", "res",
    "InstrFetchProhibited", "res", "res", "res",
    "LoadStoreTLBMiss", "LoadStoreTLBMultihit", "LoadStorePrivilege", "res",
    "LoadProhibited", "StoreProhibited", "res", "res",
    "Cp0Dis", "Cp1Dis", "Cp2Dis", "Cp3Dis",
    "Cp4Dis", "Cp5Dis", "Cp6Dis", "Cp7Dis"
};

#define NUM_EDESCS (sizeof(edesc) / sizeof(char *))

void __wrap_xt_unhandled_exception(XtExcFrame *frame)
{
    int core_id = xPortGetCoreID();

    haltOtherCore();
    esp_dport_access_int_abort();
    panicPutStr("DiUS Guru Meditation Error (exception): Core ");
    panicPutDec(core_id);
    panicPutStr(" panic'ed (");
    int exccause = frame->exccause;
    if (exccause < NUM_EDESCS) {
      panicPutStr(edesc[exccause]);
    } else {
      panicPutStr("Unknown");
    }
    panicPutStr(")");
    panicPutStr(". Exception was unhandled.\r\n");

    commonErrorHandler_dump(frame,core_id);

    panicPutStr("Rebooting...\r\n");
    esp_panic_dig_reset();
}


void __wrap_panicHandler(XtExcFrame *frame)
{
    int core_id = xPortGetCoreID();
    //Please keep in sync with PANIC_RSN_* defines
    const char *reasons[] = {
        "Unknown reason",
        "Unhandled debug exception",
        "Double exception",
        "Unhandled kernel exception",
        "Coprocessor exception",
        "Interrupt wdt timeout on CPU0",
        "Interrupt wdt timeout on CPU1",
        "Cache disabled but cached memory region accessed",
    };
    const char *reason = reasons[0];
    //The panic reason is stored in the EXCCAUSE register.
    if (frame->exccause <= PANIC_RSN_MAX) {
        reason = reasons[frame->exccause];
    }

    haltOtherCore();
    esp_dport_access_int_abort();
    panicPutStr("DiUS Guru Meditation Error (panic): Core ");
    panicPutDec(core_id);
    panicPutStr(" panic'ed (");
    panicPutStr(reason);
    panicPutStr(")\r\n");

    commonErrorHandler_dump(frame,core_id);

    panicPutStr("Rebooting...\r\n");
    esp_panic_dig_reset();
}
