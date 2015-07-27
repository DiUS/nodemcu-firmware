// Module for interfacing with timer
#define CPU_MHZ 80

#define INIT __attribute__ ((section (".text")))

//#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "platform.h"
#include "auxmods.h"
#include "lrotable.h"

#include "c_types.h"

#include "rtctime.h"
#include "rtcfifo.h"

static os_timer_t alarm_timer[NUM_TMR];
static int alarm_timer_cb_ref[NUM_TMR] = {LUA_NOREF,LUA_NOREF,LUA_NOREF,LUA_NOREF,LUA_NOREF,LUA_NOREF,LUA_NOREF};

void alarm_timer_common(lua_State* L, unsigned id){
  if(alarm_timer_cb_ref[id] == LUA_NOREF)
    return;
  lua_rawgeti(L, LUA_REGISTRYINDEX, alarm_timer_cb_ref[id]);
  lua_call(L, 0, 0);
}

void alarm_timer_cb0(void *arg){
  if( !arg )
    return;
  alarm_timer_common((lua_State*)arg, 0);
}

void alarm_timer_cb1(void *arg){
  if( !arg )
    return;
  alarm_timer_common((lua_State*)arg, 1);
}

void alarm_timer_cb2(void *arg){
  if( !arg )
    return;
  alarm_timer_common((lua_State*)arg, 2);
}

void alarm_timer_cb3(void *arg){
  if( !arg )
    return;
  alarm_timer_common((lua_State*)arg, 3);
}

void alarm_timer_cb4(void *arg){
  if( !arg )
    return;
  alarm_timer_common((lua_State*)arg, 4);
}

void alarm_timer_cb5(void *arg){
  if( !arg )
    return;
  alarm_timer_common((lua_State*)arg, 5);
}

void alarm_timer_cb6(void *arg){
  if( !arg )
    return;
  alarm_timer_common((lua_State*)arg, 6);
}

typedef void (*alarm_timer_callback)(void *arg);
static alarm_timer_callback alarm_timer_cb[NUM_TMR] = {alarm_timer_cb0,alarm_timer_cb1,alarm_timer_cb2,alarm_timer_cb3,alarm_timer_cb4,alarm_timer_cb5,alarm_timer_cb6};

// Lua: delay( us )
static int tmr_delay( lua_State* L )
{
  s32 us;
  us = luaL_checkinteger( L, 1 );
  if ( us <= 0 )
    return luaL_error( L, "wrong arg range" );
  if(us<1000000)
  {
    os_delay_us( us );
    WRITE_PERI_REG(0x60000914, 0x73);
    return 0;
  }  
  unsigned sec = (unsigned)us / 1000000;
  unsigned remain = (unsigned)us % 1000000;
  int i = 0;
  for(i=0;i<sec;i++){
    os_delay_us( 1000000 );
    WRITE_PERI_REG(0x60000914, 0x73);
  }
  if(remain>0)
    os_delay_us( remain );
  return 0;  
}

// Lua: now() , return system timer in us
static int tmr_now( lua_State* L )
{
  unsigned now = 0x7FFFFFFF & system_get_time();
  lua_pushinteger( L, now );
  return 1; 
}

// Lua: alarm( id, interval, repeat, function )
static int tmr_alarm( lua_State* L )
{
  s32 interval;
  unsigned repeat = 0;
  int stack = 1;
  
  unsigned id = luaL_checkinteger( L, stack );
  stack++;
  MOD_CHECK_ID( tmr, id );

  interval = luaL_checkinteger( L, stack );
  stack++;
  if ( interval <= 0 )
    return luaL_error( L, "wrong arg range" );

  if ( lua_isnumber(L, stack) ){
    repeat = lua_tointeger(L, stack);
    stack++;
    if ( repeat != 1 && repeat != 0 )
      return luaL_error( L, "wrong arg type" );
  }

  // luaL_checkanyfunction(L, stack);
  if (lua_type(L, stack) == LUA_TFUNCTION || lua_type(L, stack) == LUA_TLIGHTFUNCTION){
    lua_pushvalue(L, stack);  // copy argument (func) to the top of stack
    if(alarm_timer_cb_ref[id] != LUA_NOREF)
      luaL_unref(L, LUA_REGISTRYINDEX, alarm_timer_cb_ref[id]);
    alarm_timer_cb_ref[id] = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  os_timer_disarm(&(alarm_timer[id]));
  os_timer_setfn(&(alarm_timer[id]), (os_timer_func_t *)(alarm_timer_cb[id]), L);
  os_timer_arm(&(alarm_timer[id]), interval, repeat); 
  return 0;  
}

// Lua: stop( id )
static int tmr_stop( lua_State* L )
{
  unsigned id = luaL_checkinteger( L, 1 );
  MOD_CHECK_ID( tmr, id );

  os_timer_disarm(&(alarm_timer[id]));
  return 0;  
}

// extern void update_key_led();
// Lua: wdclr()
static int tmr_wdclr( lua_State* L )
{
  WRITE_PERI_REG(0x60000914, 0x73);
  // update_key_led();
  return 0;  
}

static os_timer_t rtc_timer_updator;
static uint32_t cur_count = 0;
static uint32_t rtc_10ms = 0;
void rtc_timer_update_cb(void *arg){
  uint32_t t = (uint32_t)system_get_rtc_time();
  uint32_t delta = 0;
  if(t>=cur_count){
    delta = t-cur_count;
  }else{
    delta = 0xFFFFFFF - cur_count + t + 1;
  }
  // uint64_t delta = (t>=cur_count)?(t - cur_count):(0x100000000 + t - cur_count);
  // NODE_ERR("%x\n",t);
  cur_count = t;
  uint32_t c = system_rtc_clock_cali_proc();
  uint32_t itg = c >> 12;  // ~=5
  uint32_t dec = c & 0xFFF; // ~=2ff
  rtc_10ms += (delta*itg + ((delta*dec)>>12)) / 10000;
  // TODO: store rtc_10ms to rtc memory.
}
// Lua: time() , return rtc time in second
static int tmr_time( lua_State* L )
{
  uint32_t local = rtc_10ms;
  lua_pushinteger( L, ((uint32_t)(local/100)) & 0x7FFFFFFF );
  return 1;
}

static int tmr_gettimeofday( lua_State* L )
{
  struct rtc_timeval tv;
  rtc_time_gettimeofday(&tv);

  lua_pushinteger( L, tv.tv_sec);
  lua_pushinteger( L, tv.tv_usec);
  return 2;
}

static inline void memw(void)
{
  asm volatile ("memw");
}

static int tmr_writemem( lua_State* L )
{
  uint32_t addr=luaL_checkinteger( L, 1 );
  uint32_t val=luaL_checkinteger( L, 2 );

  memw();
  *((volatile uint32_t*)addr)=val;
  memw();
  uint32_t back=*((volatile uint32_t*)addr);
  memw();
  lua_pushinteger( L, back);
  return 1;
}

static int tmr_readmem( lua_State* L )
{
  uint32_t addr=luaL_checkinteger( L, 1 );
  memw();
  uint32_t back=*((volatile uint32_t*)addr);
  memw();
  lua_pushinteger( L, back);

  return 1;
}


static int tmr_setled( lua_State* L )
{
  uint32_t state=luaL_checkinteger( L, 1 );
  if (state)
    gpio_output_set(BIT12,0,BIT12,0);
  else
    gpio_output_set(0,BIT12,BIT12,0);

  return 0;
}

static int tmr_settimeofday( lua_State* L )
{
  struct rtc_timeval tv={
    .tv_sec=luaL_checkinteger( L, 1 ),
    .tv_usec=luaL_checkinteger( L, 2 )
  };
  rtc_time_settimeofday(&tv);
  return 0;
}

static int tmr_getsample( lua_State* L)
{
  sample_t s;

  if (!rtc_fifo_pop_sample(&s))
    return 0;

  lua_pushinteger( L, s.timestamp);

  uint32_t divisor=rtc_fifo_get_divisor(&s);
  lua_pushnumber( L, (lua_Number)s.value/(lua_Number)divisor);

  uint8_t tag[5];
  memset(tag,0,sizeof(tag));
  rtc_fifo_tag_to_string(s.tag,tag);
  lua_pushstring( L, tag);

  return 3;
}

static int tmr_peeksample( lua_State* L)
{
  sample_t s;

  uint32_t offset=luaL_checkinteger( L, 1 );
  if (!rtc_fifo_peek_sample(&s,offset))
    return 0;

  lua_pushinteger( L, s.timestamp);

  uint32_t divisor=rtc_fifo_get_divisor(&s);
  lua_pushnumber( L, (lua_Number)s.value/(lua_Number)divisor);

  uint8_t tag[5];
  memset(tag,0,sizeof(tag));
  rtc_fifo_tag_to_string(s.tag,tag);
  lua_pushstring( L, tag);

  return 3;
}

static int tmr_dropsamples( lua_State* L)
{
  uint32_t offset=luaL_checkinteger( L, 1 );
  rtc_fifo_drop_samples(offset);
  return 0;
}


static int tmr_checkmagic( lua_State* L)
{
  lua_pushinteger( L, rtc_time_check_magic());
  return 1;
}

static int tmr_ccount( lua_State* L)
{
  lua_pushinteger( L, xthal_get_ccount());
  return 1;
}


static inline void read_sar_dout_reimp(uint16_t* data)
{
  volatile uint32_t* adcp=(volatile uint32_t*)0x60000d80;
  int i;

  for (i=0;i<8;i++)
  {
    uint32_t a5=255;
    uint32_t a10=0;

    uint32_t a6=~(adcp[i]);
    uint32_t a4=((uint8_t)a6)-21;
    if (((int32_t)a4)>=0)
      a10=a4;
    a10*=0x117;
    a10>>=8;

    if (255>=a10)
      a5=a10;
    else
    {
      ets_printf("This can never happen!\n");
    }

    data[i]=(a6&0x00000f00)+a5;
  }
}

static inline void read_adcs(uint16 *ptr, uint16 len, uint32_t cycles_per)
{
  rom_sar_init();

  if(len != 0 && ptr != NULL) {
    uint32 i;
    uint16 sum;
    uint16 sar_x[8];
    rom_i2c_writeReg_Mask(108,2,0,5,5,1);
    SET_PERI_REG_MASK(0x60000D5C,0x00200000);
    while(READ_PERI_REG(0x60000D50)&(0x7<<24))
    {}

    uint32_t when=xthal_get_ccount();
    while(len--) {
      while (((int32)(xthal_get_ccount()-when))<0)
      {}
      CLEAR_PERI_REG_MASK(0x60000D50,2);
      SET_PERI_REG_MASK(0x60000D50,2);
      while(READ_PERI_REG(0x60000D50)&(0x7<<24))
      {}
      read_sar_dout_reimp(&sar_x[0]);
      sum = 0;
      for(i=0;i<8;i++)
        sum += sar_x[i];
      *ptr++ = (sum+4)/8;
      when+=cycles_per;
    };
    rom_i2c_writeReg_Mask(108,2,0,5,5,0);
    while(READ_PERI_REG(0x60000D50)&(0x7<<24))
    {}
    CLEAR_PERI_REG_MASK(0x60000D5C,0x00200000);
    CLEAR_PERI_REG_MASK(0x60000D60,1);
    SET_PERI_REG_MASK(0x60000D60,1);
  }
}


static int tmr_test2( lua_State* L )
{
  uint32_t n=luaL_checkinteger( L, 1 );
  uint32_t cycles=luaL_checkinteger( L, 2 );
  uint16_t data[256];
  int i;

  if (n>256)
    n=256;
  read_adcs(data,n,cycles);
  for (i=0;i<n;i++)
    lua_pushinteger( L,data[i]);
  return n;
}

static int tmr_getchannel( lua_State* L )
{
  lua_pushinteger( L,wifi_get_channel());
  return 1;
}

static int tmr_setchannel( lua_State* L )
{
  uint32_t channel=luaL_checkinteger( L, 1 );
  wifi_set_channel(channel);

  lua_pushinteger( L,wifi_get_channel());
  return 1;
}


static int tmr_fifo_prepare( lua_State* L )
{
  uint32_t us=luaL_checkinteger( L, 1 );
  uint32_t spb=luaL_checkinteger( L, 2 );
  rtc_fifo_prepare(spb,us,0);

  return 0;
}

static int tmr_time_prepare( lua_State* L )
{
  rtc_time_prepare();

  return 0;
}

static int tmr_have_time(lua_State* L )
{
  uint32_t data=rtc_time_have_time();

  lua_pushinteger( L, data);
  return 1;
}

static int tmr_request_samples( lua_State* L )
{
  uint32_t spb=luaL_checkinteger( L, 1 );
  rtc_put_samples_to_take(spb);

  return 0;
}

static int tmr_reload_requested_samples( lua_State* L )
{
  rtc_restart_samples_to_take();

  return 0;
}

static int tmr_deep_sleep( lua_State* L )
{
  uint32_t us=luaL_checkinteger( L, 1 );
  // rtc_invalidate_calibration();
  rtc_time_deep_sleep_us(us);

  return 0;
}

static int tmr_sleep_to_sample( lua_State* L )
{
  uint32_t us=luaL_checkinteger( L, 1 );

  // rtc_invalidate_calibration();
  rtc_fifo_deep_sleep_until_sample(us);
  return 0;
}


// Module function map
#define MIN_OPT_LEVEL 2
#include "lrodefs.h"
const LUA_REG_TYPE tmr_map[] =
{
  { LSTRKEY( "delay" ), LFUNCVAL( tmr_delay ) },
  { LSTRKEY( "now" ), LFUNCVAL( tmr_now ) },
  { LSTRKEY( "alarm" ), LFUNCVAL( tmr_alarm ) },
  { LSTRKEY( "stop" ), LFUNCVAL( tmr_stop ) },
  { LSTRKEY( "wdclr" ), LFUNCVAL( tmr_wdclr ) },
  { LSTRKEY( "time" ), LFUNCVAL( tmr_time ) },

  { LSTRKEY( "rm" ), LFUNCVAL( tmr_readmem ) },
  { LSTRKEY( "wm" ), LFUNCVAL( tmr_writemem) },
  { LSTRKEY( "ccount" ), LFUNCVAL( tmr_ccount) },
  { LSTRKEY( "setled" ), LFUNCVAL( tmr_setled) },
  { LSTRKEY( "test2" ), LFUNCVAL( tmr_test2) },
  { LSTRKEY( "getchannel" ), LFUNCVAL( tmr_getchannel) },
  { LSTRKEY( "setchannel" ), LFUNCVAL( tmr_setchannel) },

  { LSTRKEY( "gettimeofday" ), LFUNCVAL( tmr_gettimeofday ) },
  { LSTRKEY( "settimeofday" ), LFUNCVAL( tmr_settimeofday ) },

  { LSTRKEY( "prepare_fifo" ), LFUNCVAL( tmr_fifo_prepare) },
  { LSTRKEY( "prepare_time" ), LFUNCVAL( tmr_time_prepare) },
  { LSTRKEY( "deep_sleep" ), LFUNCVAL( tmr_deep_sleep) },
  { LSTRKEY( "request_samples" ), LFUNCVAL( tmr_request_samples) },
  { LSTRKEY( "reload_requested_samples" ), LFUNCVAL( tmr_reload_requested_samples) },
  { LSTRKEY( "sleep_to_sample" ), LFUNCVAL( tmr_sleep_to_sample) },
  { LSTRKEY( "getsample" ), LFUNCVAL( tmr_getsample) },
  { LSTRKEY( "peeksample" ), LFUNCVAL( tmr_peeksample) },
  { LSTRKEY( "dropsamples" ), LFUNCVAL( tmr_dropsamples) },
  { LSTRKEY( "check_magic" ), LFUNCVAL( tmr_checkmagic) },
  { LSTRKEY( "have_time" ), LFUNCVAL( tmr_have_time) },
#if LUA_OPTIMIZE_MEMORY > 0

#endif
  { LNILKEY, LNILVAL }
};

LUALIB_API int luaopen_tmr( lua_State *L )
{
  int i = 0;
  for(i=0;i<NUM_TMR;i++){
    os_timer_disarm(&(alarm_timer[i]));
    os_timer_setfn(&(alarm_timer[i]), (os_timer_func_t *)(alarm_timer_cb[i]), L);
  }

#if 0
  os_timer_disarm(&rtc_timer_updator);
  os_timer_setfn(&rtc_timer_updator, (os_timer_func_t *)(rtc_timer_update_cb), NULL);
  os_timer_arm(&rtc_timer_updator, 500, 1); 
#endif

#if LUA_OPTIMIZE_MEMORY > 0
  return 0;
#else // #if LUA_OPTIMIZE_MEMORY > 0
  luaL_register( L, AUXLIB_TMR, tmr_map );
  // Add constants

  return 1;
#endif // #if LUA_OPTIMIZE_MEMORY > 0  
}
