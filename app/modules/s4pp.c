#define XMEM_TRACK "s4pp"
#include "xmem.h"

#include "module.h"
#include "lua.h"
#include "lauxlib.h"
#ifdef LWIP_OPEN_SRC
#include "lwip/ip_addr.h"
#else
#include "ip_addr.h"
#endif
#include "espconn.h"
#include "mem.h"
#include "../crypto/sha2.h"
#include "../crypto/digests.h"
#include "../crypto/mech.h"
#include "strbuffer.h"
#include "user_interface.h"

#ifdef LUA_USE_MODULES_FLASHFIFO
# include "rtc/flashfifo.h"
# define MAX_TAGS 64
#endif

typedef enum {
  NTFY_TIME=0,
  NTFY_FIRMWARE=1,
  NTFY_FLAGS=2,
} ntfy_val_t;

#include <stdio.h>

#define PAYLOAD_LIMIT 1400
#define MAX_IN_FLIGHT 2

#define AES_128_BLOCK_SIZE 16

#define lstrbuffer_append(x,...) do { if (!strbuffer_append(x,__VA_ARGS__)) luaL_error(sud->L, "no mem"); } while (0)
#define lstrbuffer_add(x,...) do { if (!strbuffer_add(x,__VA_ARGS__)) luaL_error(sud->L, "no mem"); } while (0)

typedef int8_t (*conn_function_t)(struct espconn *conn);
typedef int8_t (*send_function_t)(struct espconn *conn, const void *data, uint16_t len);

typedef struct
{
  conn_function_t connect;
  conn_function_t disconnect;
  send_function_t send;
} esp_funcs_t;

static const esp_funcs_t esp_plain = {
  .connect = espconn_connect,
  // FIXME: need to post the disconnect
  .disconnect = espconn_disconnect,
  .send = (send_function_t)espconn_send
};

static const esp_funcs_t esp_secure = {
  .connect = espconn_secure_connect,
  // FIXME: need to post the disconnect
  .disconnect = espconn_secure_disconnect,
  .send = (send_function_t)espconn_secure_send
};


typedef struct
{
  lua_State *L;
  strbuffer_t *buffer;
  struct espconn conn;
  const esp_funcs_t *funcs;
  ip_addr_t dns;
  int user_ref;
  int key_ref;
  int iter_ref;
  int cb_ref;
  int ntfy_ref;
  int progress_ref;
  int token_ref;
  int dict_ref;
  int err_ref;

  enum {
    S4PP_INIT,
    S4PP_HELLO,
    S4PP_AUTHED,
    S4PP_BUFFERING,
    S4PP_COMMITTING,
    S4PP_DONE,
    S4PP_ERRORED
  } state;

  char *recv_buf;
  uint16_t recv_len;

  int next_idx;
  uint16_t next_seq;
  uint16_t n_max;
  uint16_t n_used;
  uint32_t n_committed;
  uint32_t lasttime;
  SHA256_CTX ctx;
  bool end_of_data;
  bool all_data_sent; // May not be necessary?
  bool hide_supported;
  bool hide_wanted;
  bool hide_insisted;

  bool buffer_full;
  bool buffer_has_sig;
  bool buffer_need_seq;

  int  buffer_salt;

  int  buffer_send_active;
  int  buffer_written_active;

  uint8_t session_key[AES_128_BLOCK_SIZE];
  uint8_t iv_last_block[AES_128_BLOCK_SIZE];

  // technically the "base" is also flashfifo-only, but it saves us a bunch
  // of ifdefs to leave it in regardless, and the cost is minor enough to
  // opt for clean code over tightest memory/code
  char*       base;
#ifdef LUA_USE_MODULES_FLASHFIFO
  int         baselen;
  uint32_t    fifo_pos;
  uint32_t flashdict[MAX_TAGS];
#endif

  uint32_t connection_initiate_time;
  uint32_t connect_time;
  uint32_t hello_time;
} s4pp_userdata;

static uint16_t max_batch_size = 0; // "use the server setting"


#define goto_err_with_msg(L, ...) \
  do { \
    lua_pushfstring(L, __VA_ARGS__); \
    goto err; \
  } while (0)


static void make_hmac_pad (s4pp_userdata *sud, uint8_t padval)
{
  lua_State *L = sud->L;
  lua_rawgeti (L, LUA_REGISTRYINDEX, sud->key_ref);
  size_t klen;
  const char *key = lua_tolstring (L, -1, &klen);
  char altkey[SHA256_DIGEST_LENGTH];
  if (klen > SHA256_BLOCK_LENGTH)
  {
    SHA256_CTX ctx;
    SHA256_Init (&ctx);
    SHA256_Update (&ctx, key, klen);
    SHA256_Final (altkey, &ctx);
    key = altkey;
    klen = SHA256_DIGEST_LENGTH;
  }

  uint8_t pad[SHA256_BLOCK_LENGTH];
  os_memset (pad, padval, sizeof (pad));
  unsigned i;
  for (i = 0; i < klen; ++i)
    pad[i] ^= key[i];

  lua_pop (L, 1);
  lua_pushlstring (L, pad, sizeof (pad)); // ..and put the pad on the stack
}


static void update_hmac (s4pp_userdata *sud)
{
  lua_State *L = sud->L;
  size_t len;
  const char *data = lua_tolstring (L, -1, &len);
  SHA256_Update (&sud->ctx, data, len);
}

static void update_hmac_from_buffer (s4pp_userdata *sud)
{
  size_t len;
  const char *data = strbuffer_str (sud->buffer, &len);
  data += sud->buffer_salt;
  len -= sud->buffer_salt;
  SHA256_Update (&sud->ctx, data, len);
}

static inline void update_hmac_from_pad (s4pp_userdata *sud, const char *pad, size_t len)
{
  SHA256_Update (&sud->ctx, pad, len);
}

static void init_hmac (s4pp_userdata *sud)
{
  SHA256_Init (&sud->ctx);
  make_hmac_pad (sud, 0x36);
  update_hmac (sud);
  lua_pop (sud->L, 1); // drop the pad
}


static void append_final_hmac_hex (s4pp_userdata *sud)
{
  uint8_t digest[SHA256_DIGEST_LENGTH*2];
  SHA256_Final (digest, &sud->ctx);
  SHA256_Init (&sud->ctx);
  make_hmac_pad (sud, 0x5c);
  update_hmac (sud);
  lua_pop (sud->L, 1); // drop the pad
  SHA256_Update (&sud->ctx, digest, SHA256_DIGEST_LENGTH);
  SHA256_Final (digest, &sud->ctx);
  crypto_encode_asciihex (digest, SHA256_DIGEST_LENGTH, digest);
  lstrbuffer_append (sud->buffer, digest, sizeof (digest));
}


static lua_State *push_callback (s4pp_userdata *sud)
{
  lua_rawgeti (sud->L, LUA_REGISTRYINDEX, sud->cb_ref);
  return sud->L;
}


static void cleanup (s4pp_userdata *sud)
{
  lua_State *L = sud->L;

  luaL_unref (L, LUA_REGISTRYINDEX, sud->cb_ref);
  luaL_unref (L, LUA_REGISTRYINDEX, sud->ntfy_ref);
  luaL_unref (L, LUA_REGISTRYINDEX, sud->progress_ref);
  luaL_unref (L, LUA_REGISTRYINDEX, sud->token_ref);
  luaL_unref (L, LUA_REGISTRYINDEX, sud->user_ref);
  luaL_unref (L, LUA_REGISTRYINDEX, sud->key_ref);
  luaL_unref (L, LUA_REGISTRYINDEX, sud->iter_ref);
  luaL_unref (L, LUA_REGISTRYINDEX, sud->dict_ref);
  luaL_unref (L, LUA_REGISTRYINDEX, sud->err_ref);

  espconn_delete (&sud->conn);

  strbuffer_free (sud->buffer);

  xfree (sud->conn.proto.tcp);
  xfree (sud->recv_buf);
  xfree (sud->base);
  xfree (sud);
}


static void abort_conn (s4pp_userdata *sud)
{
  sud->state = S4PP_ERRORED;
  sud->err_ref = luaL_ref (sud->L, LUA_REGISTRYINDEX);
  sud->funcs->disconnect (&sud->conn);
}


static void report_progress (s4pp_userdata *sud)
{
  lua_rawgeti (sud->L, LUA_REGISTRYINDEX, sud->progress_ref);
  lua_pushinteger (sud->L, sud->n_used);
  lua_call (sud->L, 1, 0);
}


static void prepare_seq_hmac (s4pp_userdata *sud)
{
  init_hmac (sud);
  lua_State *L = sud->L;
  lua_rawgeti (L, LUA_REGISTRYINDEX, sud->token_ref);
  update_hmac (sud);
  lua_pop (L, 1);
}



static inline uint8_t decodehexnibble(char h) {
  if (h >= '0' && h <= '9')
    return h - '0';
  if (h >= 'a' && h <= 'f')
    return h - 'a' + 10;
  else if (h >= 'A' && h < 'F')
    return h - 'A' + 10;
  else
    return 0;
}


static inline uint8_t decodehexbyte(const char *hex) {
  return (decodehexnibble(hex[0]) << 4) | decodehexnibble(hex[1]);
}


static void create_session_key (s4pp_userdata *sud, const char *token, uint16_t len)
{
  if (len > AES_128_BLOCK_SIZE * 2)
    len = AES_128_BLOCK_SIZE * 2;
  if (len & 1)
    --len; // don't attempt to decode half hex bytes

  uint8_t inbytes[AES_128_BLOCK_SIZE];
  for (int i = 0; i < AES_128_BLOCK_SIZE; ++i)
  {
    if (i < len)
      inbytes[i] = decodehexbyte(token+i*2);
    else
      inbytes[i] = '\n';
  }

  crypto_op_t enc;
  memset (&enc, 0, sizeof(enc));
  lua_rawgeti (sud->L, LUA_REGISTRYINDEX, sud->key_ref);
  enc.key = lua_tolstring (sud->L, -1, &enc.keylen);
  enc.data = inbytes;
  enc.datalen = AES_128_BLOCK_SIZE;
  enc.out = sud->session_key;
  enc.outlen = AES_128_BLOCK_SIZE;
  enc.op = OP_ENCRYPT;

  const crypto_mech_t *mech = crypto_encryption_mech("AES-CBC");
  if (!mech || mech->block_size != AES_128_BLOCK_SIZE || !mech->run (&enc))
  {
    sud->hide_wanted = false;
    return;
  }

  lua_pop (sud->L, 1); // release the shared key
}


// already padded
static void inplace_hide (s4pp_userdata *sud, char *str, uint16_t len)
{
  crypto_op_t enc;
  memset (&enc, 0, sizeof(enc));
  enc.key = sud->session_key;
  enc.keylen = AES_128_BLOCK_SIZE;
  enc.iv = sud->iv_last_block;
  enc.ivlen = AES_128_BLOCK_SIZE;
  enc.data = str;
  enc.datalen = len;
  enc.out = str;
  enc.outlen = len;
  enc.op = OP_ENCRYPT;
  const crypto_mech_t *mech = crypto_encryption_mech("AES-CBC");
  bool res = mech->run(&enc);
  os_memcpy(
    sud->iv_last_block, str + len - AES_128_BLOCK_SIZE, AES_128_BLOCK_SIZE);
}


static void handle_auth (s4pp_userdata *sud, char *token, uint16_t len)
{
  lua_State *L = sud->L;

  lua_checkstack (L, 5);
  lua_pushlstring (L, token, len);
  sud->token_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  lua_rawgeti (L, LUA_REGISTRYINDEX, sud->user_ref);
  lua_pushlstring (L, token, len);
  lua_concat (L, 2);
  size_t slen;
  const char *str = lua_tolstring (L, -1, &slen);

  lua_rawgeti (L, LUA_REGISTRYINDEX, sud->key_ref);
  size_t klen;
  const char *key = lua_tolstring (L, -1, &klen);
  const digest_mech_info_t *hmac256 = crypto_digest_mech ("SHA256");
  uint8_t digest[hmac256->digest_size * 2];
  os_memset (digest, 0, sizeof (digest));
  crypto_hmac (hmac256, str, slen, key, klen, digest);
  crypto_encode_asciihex (digest, hmac256->digest_size, digest);

  lua_pop (L, 2);
  lua_pushliteral (L, "AUTH:SHA256,");
  lua_rawgeti (L, LUA_REGISTRYINDEX, sud->user_ref);
  lua_pushliteral (L, ",");
  lua_pushlstring (L, digest, sizeof (digest));
  lua_pushliteral (L, "\n");
  int n = 5;
  if (sud->hide_supported && sud->hide_wanted)
  {
    create_session_key (sud, token, len);
    lua_pushliteral (L, "HIDE:AES-128-CBC\n");
    ++n;
  }
  lua_concat (L, n);
  size_t alen;
  const char *auth = lua_tolstring (L, -1, &alen);
  int err = sud->funcs->send (&sud->conn, (uint8_t *)auth, alen);
  lua_pop (L, 1);
  if (err)
    goto_err_with_msg (sud->L, "auth send failed: %d", err);
  sud->buffer_send_active++;
  sud->buffer_written_active++;
  sud->state = S4PP_AUTHED;
  prepare_seq_hmac (sud);

  if (sud->hide_supported && sud->hide_wanted)
  {
    uint8_t salt[17] = { 0, };
    int nrnd = (os_random() % 8) + 8;
    for (int i = 0; i < nrnd; ++i)
    {
      while ((salt[i] = (uint8_t)os_random()) == '\n')
        ;
    }
    salt[nrnd++] = '\n';
    lstrbuffer_append(sud->buffer, salt, nrnd);
    sud->buffer_salt = nrnd; // don't hmac over this!
  }

  return;
err:
  abort_conn (sud);
}


// top of stack = { name=... }
static int get_dict_idx (s4pp_userdata *sud)
{
  lua_State *L = sud->L;
  int ret;
  int top = lua_gettop (L);

  lua_rawgeti (L, LUA_REGISTRYINDEX, sud->dict_ref);
  lua_getfield (L, -2, "name");
  if (!lua_isstring (L, -1))
    ret = -2;
  else
  {
    lua_gettable (L, -2);
    if (lua_isnumber (L, -1))
      ret = lua_tonumber (L, -1);
    else
      ret = -1;
  }
  lua_settop (L, top);
  return ret;
}


static void get_optional_field (lua_State *L, int table, const char *key, const char *dfl)
{
  lua_getfield (L, table, key);
  if (lua_isnoneornil (L, -1))
  {
    lua_pop (L, 1);
    lua_pushstring (L, dfl);
  }
}


#ifdef LUA_USE_MODULES_FLASHFIFO
static char* my_strdup(const char* in)
{
  int len=strlen(in);
  char* out=xzalloc(len+1);
  if (out)
    memcpy(out,in,len+1);
  return out;
}

static int get_dict_index(s4pp_userdata *sud, uint32_t tag)
{
  for (int i=0;i<sud->next_idx;i++)
    if (sud->flashdict[i]==tag)
      return i;
  if (sud->next_idx>=MAX_TAGS)
    return -1;

  char buf[20];
  int len=c_sprintf(buf,"DICT:%u,,1,",sud->next_idx);
  lstrbuffer_append (sud->buffer, buf, len);
  lstrbuffer_append (sud->buffer, sud->base, sud->baselen);
  fifo_tag_to_string(tag,buf);
  lstrbuffer_append (sud->buffer, buf, strlen(buf));
  lstrbuffer_append (sud->buffer,"\n",1);
  sud->flashdict[sud->next_idx]=tag;
  return sud->next_idx++;
}

static void add_data(s4pp_userdata* sud, int idx, const sample_t* sample)
{
  int32_t dt=sample->timestamp-sud->lasttime;
  sud->lasttime=sample->timestamp;
  char buf[40];
  int len=c_sprintf(buf,"%u,%d,%d.",idx,dt,sample->value);

  if (sample->decimals && sample->value!=0) // No matter how much we shift 0, it's still 0
  {
    int dotpos=len-1; // currently in last place of the string

    int shift=sample->decimals;
    for (int i=0;i<shift;i++)
    {
      if (dotpos==len-1 && buf[dotpos-1]=='0') // Trailing zeros after a decimal point
      {
        buf[dotpos--]='\0';
        buf[dotpos]='.';
        len--;
      }
      else if (buf[dotpos-1]==',' || buf[dotpos-1]=='-') // Have run out of digits to shift, so insert 0 after dot
      {
        for (int j=len;j>dotpos;j--)
          buf[j+1]=buf[j];
        buf[dotpos+1]='0';
        len++;
      }
      else // move digit at dotpos-1 past the dot
      {
        buf[dotpos]=buf[dotpos-1];
        buf[--dotpos]='.';
      }
    }

    if (buf[dotpos-1]==',' || buf[dotpos-1]=='-') // Have run out of digits to the left of dot, so insert back a 0
    {
      for (int j=len;j>=dotpos;j--)
        buf[j+1]=buf[j];
      buf[dotpos]='0';
      dotpos++;
      len++;
    }
  }
  if (buf[len-1]=='.') // If we are trailing a dot, get rid of it
    --len;
  buf[len++]='\n';
  buf[len]='\0';
  lstrbuffer_append (sud->buffer, buf, len);
}
#endif


// top of stack = { name=..., unit=..., unitdiv=... }
static int prepare_dict (s4pp_userdata *sud)
{
  lua_State *L = sud->L;
  int sample_table = lua_gettop (L);
  lua_checkstack (L, 9);

  int idx = sud->next_idx++;
  lua_rawgeti (L, LUA_REGISTRYINDEX, sud->dict_ref);
  lua_getfield (L, sample_table, "name"); // we know this exists by now
  lua_pushinteger (L, idx);
  lua_settable (L, -3);
  lua_pop (L, 1); // drop dict from stack

  // TODO: optimise this into a c_sprintf() like prepare_data?

  lua_pushliteral (L, "DICT:");
  lua_pushinteger (L, idx);
  lua_pushliteral (L, ",");
  get_optional_field (L, sample_table, "unit", "");
  lua_pushliteral (L, ",");
  get_optional_field (L, sample_table, "unitdiv", "1");
  lua_pushliteral (L, ",");
  lua_getfield (L, sample_table, "name");
  lua_pushliteral (L, "\n");
  lua_concat (L, 9); // DICT:<idx>,<unit>,<unitdiv>,<name>\n
  size_t len;
  const char *str = lua_tolstring (L, -1, &len);

  lstrbuffer_append (sud->buffer, str, len);
  lua_pop (L, 1);
  return idx;
}


// top of stack = { time=..., value=... }
static bool prepare_data (s4pp_userdata *sud, int idx)
{
  lua_State *L = sud->L;
  int sample_table = lua_gettop (L);
  lua_checkstack (L, 2);

  lua_getfield (L, sample_table, "time");
  if (!lua_isnumber (L, -1))
    goto failed;

  uint32_t timestamp = lua_tonumber (L, -1);
  int delta_t = timestamp - sud->lasttime;
  sud->lasttime = timestamp;
  lua_pop (L, 1);

  lua_getfield (L, sample_table, "value");
  if (!lua_isnumber (L, -1))
    goto failed;
  const char *val = lua_tostring (L, -1);

  char tmp[55]; // TODO: verify sensibility of this size
  int n = c_sprintf (tmp, "%u,%d,%s\n", idx, delta_t, val);
  lua_pop (L, 1);

  if (n < 0 || n >= sizeof(tmp))
    goto failed;

  lstrbuffer_append (sud->buffer, tmp, n);
  return true;

failed:
  lua_settop (L, sample_table);
  return false;
}

static void progress_work (s4pp_userdata *sud)
{
  lua_State *L = sud->L;

  switch (sud->state)
  {
    case S4PP_AUTHED:
    {
      sud->next_idx = 0;
      sud->n_used = 0;
      sud->lasttime = 0;
      luaL_unref (L, LUA_REGISTRYINDEX, sud->dict_ref);
      lua_newtable (L);
      sud->dict_ref = luaL_ref (L, LUA_REGISTRYINDEX);
      sud->buffer_need_seq = true;
      sud->state = S4PP_BUFFERING;
      // fall through
    }
    case S4PP_BUFFERING:
    {
      if (!sud->buffer_full)
      {
        if (sud->buffer_need_seq)
          lstrbuffer_add (sud->buffer, "SEQ:%u,0,1,0\n", sud->next_seq++); // seq:N time:0 timediv:1 datafmt:0
        sud->buffer_need_seq = false;

        lua_State *L = sud->L;
        size_t sz_estimate;
        bool sig = false;
        while (strbuffer_str (sud->buffer, &sz_estimate) &&
               sz_estimate < PAYLOAD_LIMIT &&
               !sig)
        {
          if (!lua_checkstack (L, 1))
            goto_err_with_msg (L, "out of stack");

          if ((sud->n_used >= sud->n_max) ||
              (max_batch_size > 0) && (sud->n_used >= max_batch_size))
            sig = true;
          else
          {
            if (!sud->base)
            {
              lua_rawgeti (L, LUA_REGISTRYINDEX, sud->iter_ref);
              lua_call (L, 0, 1);
              if (lua_istable (L, -1))
              {
                // send dict and/or data
                int idx = get_dict_idx (sud);
                if (idx == -2)
                  goto_err_with_msg (L, "no 'name'");
                else if (idx == -1)
                  idx=prepare_dict (sud);
                if (!prepare_data (sud, idx))
                  goto_err_with_msg (L, "no 'time' or 'value'");
                ++sud->n_used;
                lua_pop (L, 1); // drop table
              }
              else if (lua_isnoneornil (L, -1))
              {
                sig = true;
                sud->end_of_data=true;
                lua_pop (L, 1);
              }
              else
                goto_err_with_msg (L, "iterator returned garbage");
            }
#ifdef LUA_USE_MODULES_FLASHFIFO
            else
            {
              bool stop = false;
              if ((sud->fifo_pos&511)==511)
              { // Time to extend the global timeout
                lua_rawgeti (L, LUA_REGISTRYINDEX, sud->iter_ref);
                lua_pushinteger (L, sud->n_committed);
                lua_call (L, 1, 1);
                stop = lua_isnoneornil(L, -1);
                lua_pop (L, 1);
              }

              sample_t sample;
              if (!stop && flash_fifo_peek_sample(&sample,sud->fifo_pos))
              {
                int idx=get_dict_index(sud,sample.tag);
                if (idx<0)
                  goto_err_with_msg (L, "dictionary overflowed");
                add_data(sud,idx,&sample);
                sud->fifo_pos++;
                sud->n_used++;
              }
              else
              {
                sig = true;
                sud->end_of_data=true;
              }
            }
#endif
          }
        }

        update_hmac_from_buffer (sud);
        if (sig)
        {
          lstrbuffer_add (sud->buffer, "SIG:");
          append_final_hmac_hex (sud);
          lstrbuffer_add (sud->buffer, "\n");
        }
        sud->buffer_full = true;
        sud->buffer_has_sig = sig;
        // Encrypt if supposed to
        if (sud->hide_wanted && sud->hide_supported)
        {
          size_t len;
          char *str = strbuffer_str (sud->buffer, &len);

          int pad = AES_128_BLOCK_SIZE - (len % AES_128_BLOCK_SIZE);
          if (pad != 0 && pad != AES_128_BLOCK_SIZE)
          {
            static const char* newlines="\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n";
            strbuffer_resize (sud->buffer, len + pad);
            strbuffer_append(sud->buffer,newlines,pad);
            update_hmac_from_pad(sud,    newlines,pad);
            str = strbuffer_str (sud->buffer, &len);
          }
          inplace_hide (sud, str, len);
        }
      }
      // Try sending the buffer. We know it's full, because it either already was, or we just filled it
      size_t len;
      char *str = strbuffer_str (sud->buffer, &len);
      int res = sud->funcs->send (&sud->conn, str, len);

      if (res == 0) // Actually did send. Synchronise state, and reset buffer
      {
        sud->buffer_send_active++;
        sud->buffer_written_active++;

        if (sud->buffer_has_sig)
          sud->state = S4PP_COMMITTING;

        if (sud->end_of_data)
          sud->all_data_sent = true;

        strbuffer_reset (sud->buffer);
        sud->buffer_full = false;
        sud->buffer_salt = 0;
      }
      if (res == ESPCONN_MAXNUM && sud->buffer_send_active) // That's OK
        res = 0;

      if (res != 0)
        goto_err_with_msg (L, "send failed: %d", res);
      break;
    }
    case S4PP_COMMITTING:
      break; // just waiting for OK/NOK now
    case S4PP_DONE:
      break; // The "OK" receive callback jumped in before the "sent" callback for the last packet. The SDK does not
             // necessarily work through callbacks in order...
    default:
      goto_err_with_msg (L, "bad state: %d", sud->state);
  }
  return;
err:
  abort_conn (sud);
}

static uint32_t system_time_diff(uint32_t first, uint32_t second)
{
  return (second-first)&0x7fffffff;
}

static void handle_notify (s4pp_userdata *sud, char *ntfy)
{
  if (sud->ntfy_ref == LUA_NOREF)
    return;

  lua_rawgeti (sud->L, LUA_REGISTRYINDEX, sud->ntfy_ref);

  char *nxtarg = strchr (ntfy, ',');
  if (nxtarg)
    *nxtarg++ = 0;

  unsigned code = strtoul (ntfy, NULL, 0);
  lua_pushinteger (sud->L, code);

  unsigned n_args = 1;
  while (nxtarg && (n_args + 1) < LUA_MINSTACK)
  {
    char *arg = nxtarg;
    nxtarg = strchr (arg, ',');
    if (nxtarg)
      *nxtarg++ = 0;

    lua_pushstring (sud->L, arg);
    ++n_args;
  }
  if (code==NTFY_TIME && n_args+3<LUA_MINSTACK)
  {
    uint32_t now=system_get_time();
    lua_pushinteger(sud->L, system_time_diff(sud->connection_initiate_time,sud->connect_time));
    lua_pushinteger(sud->L, system_time_diff(sud->connect_time,sud->hello_time));
    lua_pushinteger(sud->L, system_time_diff(sud->hello_time,now));
    n_args+=3;
  }
  lua_call (sud->L, n_args, 0);
}


static bool handle_line (s4pp_userdata *sud, char *line, uint16_t len)
{
  if (line[len -1] == '\n')
    line[len -1] = 0;
  else
    goto_err_with_msg (sud->L, "missing newline");
  if (strncmp ("S4PP/", line, 5) == 0)
  {
    // S4PP/x.y <algo,...> <max_samples> [hidealgo,...]
    if (sud->state > S4PP_INIT)
      goto_err_with_msg (sud->L, "unexpected S4pp hello");
    sud->hello_time=system_get_time();

    char *algos = strchr (line, ' ');
    if (!algos || !strstr (algos, "SHA256"))
      goto_err_with_msg (sud->L, "server does not support SHA256");

    char *maxn = strchr (algos + 1, ' ');
    if (maxn)
      sud->n_max = strtol (maxn, NULL, 0);
    if (!sud->n_max)
      goto_err_with_msg (sud->L, "bad hello");

    if (line[5] == '1' && line[7] >= '2') // "hide" support
    {
      algos = strchr (maxn + 1, ' ');
      if (algos && strstr (algos, "AES-128-CBC"))
        sud->hide_supported = true;
    }
    if (sud->hide_insisted && !sud->hide_supported)
      goto_err_with_msg (sud->L, "server does not support HIDE");

    sud->state = S4PP_HELLO;
  }
  else if (strncmp ("TOK:", line, 4) == 0)
  {
    if (sud->state == S4PP_HELLO)
      handle_auth (sud, line + 4, len - 5); // ditch \0
    else
      goto_err_with_msg (sud->L, "bad tok");
  }
  else if (strncmp ("REJ:", line, 4) == 0)
    goto_err_with_msg (sud->L, "protocol error: %s", line + 4);
  else if (strncmp ("NOK:", line, 4) == 0)
    // we don't pipeline, so don't need to check the seqno
    goto_err_with_msg (sud->L, "commit failed");
  else if (strncmp ("OK:", line, 3) == 0)
  {
    if (sud->progress_ref != LUA_NOREF)
      report_progress(sud);
#ifdef LUA_USE_MODULES_FLASHFIFO
    if (sud->base) {
      flash_fifo_drop_samples (sud->n_used);
      sud->fifo_pos = 0;
    }
#endif
    // again, we don't pipeline, so easy to keep track of n_committed
    sud->n_committed += sud->n_used;
    if (sud->all_data_sent)
    {
      sud->state = S4PP_DONE;
      sud->funcs->disconnect (&sud->conn);
    }
    else
    {
      sud->state = S4PP_AUTHED;
      prepare_seq_hmac (sud);
      progress_work (sud);
    }
  }
  else if (strncmp ("NTFY:", line, 5) == 0)
    handle_notify (sud, line + 5);
  else
    goto_err_with_msg (sud->L, "unexpected response: %s", line);
  return true;
err:
  abort_conn (sud);
  return false;
}


static void on_recv (void *arg, char *data, uint16_t len)
{
  if (!len)
    return;

  s4pp_userdata *sud = ((struct espconn *)arg)->reverse;

  char *nl = memchr (data, '\n', len);

  // deal with joining with previous chunk
  if (sud->recv_len)
  {
    char *end = nl ? nl : data + len -1;
    uint16_t dlen = (end - data)+1;
    uint16_t newlen = sud->recv_len + dlen;
    char *p = (char *)xrealloc (sud->recv_buf, newlen);
    if (!p)
    {
      xfree (sud->recv_buf);
      sud->recv_buf = 0;
      sud->recv_len = 0;
      goto_err_with_msg (sud->L, "no memory for recv buffer");
    }
    else
      sud->recv_buf = p;
    os_memcpy (sud->recv_buf + sud->recv_len, data, newlen - sud->recv_len);
    sud->recv_len = newlen;
    data += dlen;
    len -= dlen;

    if (nl)
    {
      if (!handle_line (sud, sud->recv_buf, sud->recv_len))
        return; // we've ditched the connection
      else
      {
        xfree (sud->recv_buf);
        sud->recv_buf = 0;
        sud->recv_len = 0;
        nl = memchr (data, '\n', len);
      }
    }
  }
  // handle full lines inside 'data'
  while (nl)
  {
    uint16_t dlen = (nl - data) +1;
    if (!handle_line (sud, data, dlen))
      return;

    data += dlen;
    len -= dlen;
    nl = memchr (data, '\n', len);
  }

  // deal with left-over pieces
  if (len)
  {
    sud->recv_buf = (char *)xmalloc (len);
    if (!sud->recv_buf)
      goto_err_with_msg (sud->L, "no memory for recv buffer");
    sud->recv_len = len;
    os_memcpy (sud->recv_buf, data, len);
  }
  return;

err:
  abort_conn (sud);
}


static void maybe_progress_work(s4pp_userdata *sud)
{
  if (sud->buffer_written_active == 0 &&
      sud->buffer_send_active < MAX_IN_FLIGHT)
    progress_work (sud);
}


static void on_written (void *arg)
{
  s4pp_userdata *sud = ((struct espconn *)arg)->reverse;
  sud->buffer_written_active--;
  maybe_progress_work (sud);
}

static void on_sent (void *arg)
{
  s4pp_userdata *sud = ((struct espconn *)arg)->reverse;
  sud->buffer_send_active--;
  maybe_progress_work (sud);
}


static void on_disconnect (void *arg)
{
  s4pp_userdata *sud = ((struct espconn *)arg)->reverse;
  lua_State *L = push_callback (sud);
  if (sud->state == S4PP_DONE)
  {
    lua_pushnil (L);
    lua_pushinteger (L, sud->n_committed);
  }
  else
  {
    if (sud->err_ref != LUA_NOREF)
      lua_rawgeti (L, LUA_REGISTRYINDEX, sud->err_ref);
    else
      lua_pushstring (L, "unexpected disconnect");
    lua_pushinteger (L, sud->n_committed);
  }
  cleanup (sud);
  lua_call (L, 2, 0);
}


static void on_reconnect (void *arg, int8_t err)
{
  s4pp_userdata *sud = ((struct espconn *)arg)->reverse;
  lua_State *L = push_callback (sud);
  lua_pushfstring (L, "error: %d", err);
  lua_pushinteger (L, sud->n_committed);
  cleanup (sud);
  lua_call (L, 2, 0);
}


static void on_dns_found (const char *name, ip_addr_t *ip, void *arg)
{
  (void)name;
  s4pp_userdata *sud = ((struct espconn *)arg)->reverse;
  lua_State *L = push_callback (sud);
  if (ip)
  {
    os_memcpy (&sud->conn.proto.tcp->remote_ip, ip, 4);
    int res = sud->funcs->connect (&sud->conn);
    if (res == 0)
    {
      sud->connection_initiate_time=system_get_time();
      lua_pop (L, 1);
      return;
    }
    else
      lua_pushfstring (L, "connect failed: %d", res);
  }
  else
    lua_pushliteral (L, "DNS failed: host not found");

  lua_pushinteger (L, sud->n_committed);
  cleanup (sud);
  lua_call (L, 2, 0);
}

static void on_connect(void* arg)
{
  struct espconn* conn=(struct espconn*)arg;
  s4pp_userdata *sud = conn->reverse;

  sud->connect_time=system_get_time();
  espconn_set_opt(conn,ESPCONN_REUSEADDR|ESPCONN_COPY|ESPCONN_NODELAY);
}


// s4pp.upload({server:, port:, secure:, user:, key:}, iterator, callback, ntfy)
static int s4pp_do_upload (lua_State *L)
{
  bool have_ntfy = false;
  bool have_progress = false;

  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkanyfunction (L, 2);
  luaL_checkanyfunction (L, 3);
  if (lua_gettop (L) >= 4)
  {
    luaL_checkanyfunction (L, 4);
    have_ntfy = true;
  }
  if (lua_gettop (L) >= 5)
  {
    luaL_checkanyfunction (L, 5);
    have_progress = true;
  }

  const char *err_msg = 0;
#define err_out(msg) do { err_msg = msg; goto err; } while (0)

  s4pp_userdata *sud = (s4pp_userdata *)xzalloc (sizeof (s4pp_userdata));
  if (!sud)
    err_out ("no memory");
  sud->buffer = strbuffer_create (PAYLOAD_LIMIT + 128); // A bit of headroom
  if (!sud->buffer)
    err_out ("no memory");
  sud->L = L;
  sud->cb_ref = sud->progress_ref = sud->ntfy_ref = sud->user_ref = sud->key_ref = sud->token_ref = sud->err_ref = sud->dict_ref = LUA_NOREF;

  lua_getfield (L, 1, "user");
  if (!lua_isstring (L, -1))
    err_out ("no 'user' cfg");
  sud->user_ref = luaL_ref (L, LUA_REGISTRYINDEX);

  lua_getfield (L, 1, "key");
  if (!lua_isstring (L, -1))
    err_out ("no 'key' cfg");
  sud->key_ref = luaL_ref (L, LUA_REGISTRYINDEX);

#ifdef LUA_USE_MODULES_FLASHFIFO
  lua_getfield (L, 1, "flashbase");
  if (lua_isstring (L, -1))
  {
    sud->base=my_strdup(lua_tolstring(L,-1,NULL));
    sud->baselen=strlen(sud->base);
  }
  lua_pop (L, 1);
#endif

  sud->conn.type = ESPCONN_TCP;
  sud->conn.proto.tcp = (esp_tcp *)xzalloc (sizeof (esp_tcp));
  if (!sud->conn.proto.tcp)
    err_out ("no memory");

  lua_getfield (L, 1, "port");
  if (lua_isnumber (L, -1))
    sud->conn.proto.tcp->remote_port = lua_tonumber (L, -1);
  else
    sud->conn.proto.tcp->remote_port = 22226;
  lua_pop (L, 1);

  sud->conn.reverse = sud;
  espconn_regist_disconcb  (&sud->conn, on_disconnect);
  espconn_regist_reconcb   (&sud->conn, on_reconnect);
  espconn_regist_recvcb    (&sud->conn, on_recv);
  espconn_regist_sentcb    (&sud->conn, on_sent);
  espconn_regist_connectcb (&sud->conn, on_connect);
  espconn_regist_write_finish(&sud->conn, on_written);

  lua_getfield (L, 1, "secure");
  bool secure = (lua_isnumber (L, -1) && lua_tonumber (L, -1) > 0);
  if (secure)
    sud->funcs = &esp_secure;
  else
    sud->funcs = &esp_plain;
  lua_pop (L, 1);

  lua_getfield (L, 1, "hide");
  if (lua_isnumber (L, -1))
  {
    switch (lua_tointeger (L, -1)) {
      case 0: sud->hide_wanted = false; break;
      default:
      case 1: sud->hide_wanted = true; break;
      case 2: sud->hide_wanted = true;
              sud->hide_insisted = true; break;
    }
  }
  else
    sud->hide_wanted = !secure; // only do HIDE if not already on TLS
  lua_pop (L, 1);

  lua_pushvalue (L, 2);
  sud->iter_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  lua_pushvalue (L, 3);
  sud->cb_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  if (have_ntfy)
  {
    lua_pushvalue (L, 4);
    sud->ntfy_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  }
  if (have_progress)
  {
    lua_pushvalue (L, 5);
    sud->progress_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  }

  lua_getfield (L, 1, "server");
  if (!lua_isstring (L, -1))
    err_out ("no 'server' cfg");
  int res = espconn_gethostbyname (
    &sud->conn, lua_tostring (L, -1), &sud->dns, on_dns_found);
  lua_pop (L, 1);
  switch (res)
  {
    case ESPCONN_OK: // already resolved!
    case ESPCONN_INPROGRESS: break;
    default:
     xfree (sud->conn.proto.tcp);
     xfree (sud->base);
     xfree (sud);
     return luaL_error (L, "DNS lookup error: %d", res);
  }

  if (res == ESPCONN_OK) // synthesize DNS callback
    on_dns_found (0, &sud->dns, &sud->conn);

  return 0;

err:
  if (sud)
  {
    xfree (sud->conn.proto.tcp);
    xfree (sud->base);
  }
  xfree (sud);
  return luaL_error (L, err_msg);
}


// oldsz = s4pp.batchsize([newsz])
static int s4pp_do_batchsize (lua_State *L)
{
  lua_pushinteger (L, max_batch_size);
  if (lua_isnumber (L, 1))
    max_batch_size = lua_tointeger (L, 1);
  return 1;
}


static const LUA_REG_TYPE s4pp_map[] =
{
  { LSTRKEY("upload"),        LFUNCVAL(s4pp_do_upload) },
  { LSTRKEY("batchsize"),     LFUNCVAL(s4pp_do_batchsize) },
  { LSTRKEY("NTFY_TIME"),     LNUMVAL(NTFY_TIME) },
  { LSTRKEY("NTFY_FIRMWARE"), LNUMVAL(NTFY_FIRMWARE) },
  { LSTRKEY("NTFY_FLAGS"),    LNUMVAL(NTFY_FLAGS) },
  XMEM_LUA_TABLE_ENTRY
  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(S4PP, "s4pp", s4pp_map, NULL);
