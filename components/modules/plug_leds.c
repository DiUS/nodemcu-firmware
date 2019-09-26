// Module for interfacing with DiUS plug LEDs

#include "module.h"
#include "lauxlib.h"
#include "platform.h"
#include "driver/ledc.h"
#include "esp_log.h"



#define LEVEL_COUNT 4
#define LED_COUNT   2

static xTaskHandle   hwAccess = NULL;
static QueueHandle_t queue;
typedef enum {
  LEDS_INIT,
  LEDS_UPDATE,
} command_t;

typedef struct {
  union {
    struct {
      uint8_t pin_r;
      uint8_t pin_g;
      uint8_t pin_b;
    };
    uint8_t pins[3];
  };
} led_connection_t;

static led_connection_t led[LED_COUNT];

#define RED_SHIFT    16
#define GREEN_SHIFT   8
#define BLUE_SHIFT    0
#define TRANSPARENT  (1<<24)

typedef struct {
  uint32_t pat;
  uint32_t rgba1;
  uint32_t rgba2;
  uint16_t  count;
} led_pattern_t;

static led_pattern_t leds[LED_COUNT][LEVEL_COUNT];
static uint64_t blank_until=0;
static volatile uint8_t showingPos=0;

#define LEDC_HS_TIMER          LEDC_TIMER_0
#define LEDC_HS_MODE           LEDC_HIGH_SPEED_MODE


static void init_leds(void)
{
  ledc_timer_config_t ledc_timer = {
    .duty_resolution = LEDC_TIMER_8_BIT, // resolution of PWM duty
    .freq_hz = 1500,                     // frequency of PWM signal
    .speed_mode = LEDC_HS_MODE,          // timer mode
    .timer_num = LEDC_HS_TIMER           // timer index
  };
  // Set configuration of timer0 for high speed channels
  ledc_timer_config(&ledc_timer);

  for (int i=0;i<3*LED_COUNT;i++)
  {
    ledc_channel_config_t ledc_channel={
      .channel    = i,
      .duty       = 0,
      .gpio_num   = led[i/3].pins[i%3],
      .speed_mode = LEDC_HS_MODE,
      .hpoint     = 0,
      .timer_sel  = LEDC_HS_TIMER
    };
    ledc_channel_config(&ledc_channel);
  }
}

static void show_leds(uint32_t pos)
{
  uint64_t now=esp_timer_get_time();

  for (int l=0;l<LED_COUNT;l++)
  {
    uint32_t rgba=TRANSPARENT;

    for (int e=0;e<LEVEL_COUNT;e++)
    {
      bool second=(leds[l][e].pat&pos)!=0;
      rgba=second?leds[l][e].rgba2:leds[l][e].rgba1;
      if ((rgba&TRANSPARENT)==0)
        break;
    }
    if (rgba&TRANSPARENT)
      rgba=0;
    if (now<blank_until)
      rgba=0;

    ledc_set_duty(LEDC_HS_MODE,3*l+0,256-((rgba>>RED_SHIFT)&0xff)); ledc_update_duty(LEDC_HS_MODE,3*l+0);
    ledc_set_duty(LEDC_HS_MODE,3*l+1,256-((rgba>>GREEN_SHIFT)&0xff)); ledc_update_duty(LEDC_HS_MODE,3*l+1);
    ledc_set_duty(LEDC_HS_MODE,3*l+2,256-((rgba>>BLUE_SHIFT)&0xff)); ledc_update_duty(LEDC_HS_MODE,3*l+2);
  }
}

static void update_leds(uint32_t pos)
{
  show_leds(pos);
}


#define PATTERN_US 125000
static void hw_access(void* pvParameters)
{
  uint64_t last_tick=esp_timer_get_time();
  uint32_t pos=1;
  bool     initialised=false;

  for (;;)
  {
    uint64_t now=esp_timer_get_time();
    if (now-last_tick<PATTERN_US)
    {
      command_t cmd;
      if (xQueueReceive( queue, &cmd, (PATTERN_US-(now-last_tick))*configTICK_RATE_HZ/1000000)==pdTRUE)
      {
        switch(cmd)
        {
        case LEDS_INIT:   init_leds(); initialised=true; update_leds(pos); break;
        case LEDS_UPDATE: if (initialised) update_leds(pos); break;
        default: break;
        }
      }
    }
    else
    {
      last_tick+=PATTERN_US;
      pos>>=1;
      showingPos++;
      if (!pos)
      {
        pos=0x80000000U;
        showingPos=0;
      }
      if (initialised)
        show_leds(pos);
      for (int l=0;l<LED_COUNT;l++)
      {
        for (int e=0;e<LEVEL_COUNT;e++)
        {
          if (leds[l][e].count)
          {
            if (--leds[l][e].count==0)
              leds[l][e].rgba1=leds[l][e].rgba2=TRANSPARENT;
          }
        }
      }
    }
  }
}

static int led_init( lua_State *L )
{
  int r1 = luaL_checkinteger( L, 1 );
  int g1 = luaL_checkinteger( L, 2 );
  int b1 = luaL_checkinteger( L, 3 );
  int r2 = luaL_checkinteger( L, 4 );
  int g2 = luaL_checkinteger( L, 5 );
  int b2 = luaL_checkinteger( L, 6 );

  led[0].pin_r=r1;
  led[0].pin_g=g1;
  led[0].pin_b=b1;
  led[1].pin_r=r2;
  led[1].pin_g=g2;
  led[1].pin_b=b2;

  for (int l=0;l<LED_COUNT;l++)
    for (int i=0;i<LEVEL_COUNT;i++)
      leds[l][i].rgba1=leds[l][i].rgba2=TRANSPARENT;

  if (!hwAccess)
  {
    queue = xQueueCreate(10, sizeof(command_t));
    xTaskCreate(hw_access, "plug_leds", 4096, NULL, ESP_TASK_MAIN_PRIO + 2, &hwAccess);
  }
  command_t cmd=LEDS_INIT;
  xQueueSendToBack( queue, &cmd, portMAX_DELAY );

  return 0;
}

static int led_red( lua_State *L )
{
  int val = luaL_checkinteger( L, 1 );
  lua_pushinteger(L,val<<RED_SHIFT);
  return 1;
}

static int led_green( lua_State *L )
{
  int val = luaL_checkinteger( L, 1 );
  lua_pushinteger(L,val<<GREEN_SHIFT);
  return 1;
}

static int led_blue( lua_State *L )
{
  int val = luaL_checkinteger( L, 1 );
  lua_pushinteger(L,val<<BLUE_SHIFT);
  return 1;
}

static int led_rgb( lua_State *L )
{
  int r = luaL_checkinteger( L, 1 );
  int g = luaL_checkinteger( L, 2 );
  int b = luaL_checkinteger( L, 3 );
  lua_pushinteger(L,(r<<RED_SHIFT)+(g<<GREEN_SHIFT)+(b<<BLUE_SHIFT));
  return 1;
}

static int led_transparent( lua_State *L )
{
  lua_pushinteger(L,TRANSPARENT);
  return 1;
}

static int led_set_impl( lua_State* L, uint16_t count)
{
  unsigned int l     = luaL_checkinteger( L, 1 );
  unsigned int level = luaL_checkinteger( L, 2 );
  unsigned int rgba1 = luaL_checkinteger( L, 3 );
  unsigned int pat   = luaL_optint(L, 4, 0);
  unsigned int rgba2 = luaL_optint(L, 5, TRANSPARENT);

  if (l>=LED_COUNT)
    return luaL_error(L,"invalid LED index: %u\n",l);
  if (level>=LEVEL_COUNT)
    return luaL_error(L,"invalid LED level: %u\n",level);
  if (count) // limited-time display, so adjust the pattern to start at current position
  {
    if (showingPos!=0)
      pat=(pat>>showingPos)|(pat<<(32-showingPos));
  }
  leds[l][level].pat=pat;
  leds[l][level].rgba1=rgba1;
  leds[l][level].rgba2=rgba2;
  leds[l][level].count=count;

  command_t cmd=LEDS_UPDATE;
  xQueueSendToBack( queue, &cmd, portMAX_DELAY );
  return 0;
}

static int led_set( lua_State* L )
{
  return led_set_impl(L,0);
}

static int led_flash( lua_State* L )
{
  unsigned int count=luaL_optint(L, 6, 2);
  return led_set_impl(L,count);
}


static int led_blank( lua_State* L )
{
  unsigned int us = luaL_checkinteger( L, 1 );
  blank_until=esp_timer_get_time()+us;
  command_t cmd=LEDS_UPDATE;
  xQueueSendToBack( queue, &cmd, portMAX_DELAY );
  return 0;
}

// plug_leds.iomux(pin[,signal[,invert]]). Only selects the output source; Something else needs to set the pin to output
// Returns the previous source value and inversion. If signal is not given, only reads, does not write
// This is an awful hack for the EOL mode!
static int led_iomux( lua_State* L )
{
  unsigned int pin = luaL_checkinteger( L, 1 );
  int sig   = luaL_optint(L, 2, -1);
  unsigned int inv   = luaL_optint(L, 3, 0);

  if (!GPIO_IS_VALID_GPIO(pin))
    return luaL_error(L,"invalid GPIO index: %u\n",pin);
  if (sig>256)
    return luaL_error(L,"invalid signal index: %u\n",sig);
  if (inv>1)
    return luaL_error(L,"invalid invert-signal value (0 and 1 supported): %u\n",inv);

  uint32_t reg=GPIO_FUNC0_OUT_SEL_CFG_REG+4*pin;
  uint32_t previous=READ_PERI_REG(reg);

  if (sig>=0)
    WRITE_PERI_REG(reg,sig|(inv<<9));
  lua_pushinteger(L,previous&0x1ff);
  lua_pushinteger(L,(previous>>9)&0x1);
  return 2;
}

// Module function map
LROT_BEGIN(plug_leds)
  LROT_FUNCENTRY( init,        led_init )

  LROT_FUNCENTRY( red,         led_red )
  LROT_FUNCENTRY( green,       led_green )
  LROT_FUNCENTRY( blue,        led_blue )
  LROT_FUNCENTRY( rgb,         led_rgb )
  LROT_FUNCENTRY( transparent, led_transparent )

  LROT_FUNCENTRY( set,         led_set )
  LROT_FUNCENTRY( flash,       led_flash )
  LROT_FUNCENTRY( blank,       led_blank )

  LROT_FUNCENTRY( iomux,       led_iomux )

LROT_END(plug_leds, NULL, 0)

NODEMCU_MODULE(PLUG_LEDS, "plug_leds", plug_leds, NULL);
