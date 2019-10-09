/*
 * Copyright 2019 Dius Computing Pty Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the
 *   distribution.
 * - Neither the name of the copyright holders nor the names of
 *   its contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @author Johny Mattsson <jmattsson@dius.com.au>
 */
#include "module.h"
#include "s4pp.h"
#include "task/task.h"
#include "lauxlib.h"
#include <lwip/ip_addr.h>
#include <lwip/dns.h>
#include <lwip/api.h>
#include <esp_log.h>
#include <string.h>
#include <lwip/ip.h>
#include <lwip/tcp.h>

#define S4PP_TABLE_INSTANCE "s4pp.instance"
// Reserved for reporting simple DNS failures, in case we stop implementing iterative in here and punt it to LUA
#define S4PP_DNS_FAILED                (256+1) // Lives in the same space as s4pp_error_t, so make sure we don't collide
// For reporting DNS failures that persist even after rotating the DNS servers
#define S4PP_DNS_FAILED_COMPLETELY     (256+2) // Lives in the same space as s4pp_error_t, so make sure we don't collide

#define free_and_clear(p) do { free(p); p = NULL; } while (0)
#define unref_and_clear(L, r) \
  do { luaL_unref(L, LUA_REGISTRYINDEX, r); r = LUA_NOREF; } while(0)

typedef struct s4pp_conn {
  ip_addr_t resolved_ip;
  uint16_t port;
  struct netconn *netconn;
  unsigned left_to_send;
  struct s4pp_conn *next;
} s4pp_conn_t;

struct s4pp_state;

struct s4pp_server
{
  char *hostname;
  uint16_t port;

  struct s4pp_state *state;
};

typedef struct s4pp_state
{
  s4pp_auth_t auth;
  s4pp_server_t server;

  s4pp_ctx_t *ctx;
  s4pp_conn_t *conn;

  int userdata_ref;
  unsigned pending_evts;
  uint8_t dns_shuffle_count;

  int64_t timestamps[3];
#if CONFIG_LUA_MODULE_FLASHFIFO
  uint32_t fifo_consumed;
  int fifo_max; // -1 = no limit
#endif

  struct s4pp_state *next;
} s4pp_state_t;

// bit field events for pending_evts in userdata
enum { SUBMIT_DONE_EVT = 0x1 };

typedef struct s4pp_userdata
{
  s4pp_state_t *state;

  int notify_ref;
  int commit_ref;
  int error_ref;
  int submit_ref;
  int submit_done_ref;
  int submit_idx;
} s4pp_userdata_t;


typedef struct netconn_bounce_event {
  union {
    struct netconn *netconn;
    s4pp_conn_t *conn; // for dns event
  };
  enum { CONN_EVT_RECV, CONN_EVT_SENT, CONN_EVT_ERR, CONN_EVT_DNS } evt;
  union {
    uint16_t len; // for sent event
    ip_addr_t addr;
  };
  int64_t timestamp;
} netconn_bounce_event_t;

static netconn_bounce_event_t* allocate_bounce_event(void)
{
  netconn_bounce_event_t* e=malloc(sizeof(netconn_bounce_event_t));
  if (!e)
    return NULL;
  e->timestamp=esp_timer_get_time();
  return e;
}

static s4pp_state_t *active_s4pps;
static s4pp_conn_t *active_conns;

static task_handle_t s4pp_task;
static task_handle_t conn_task;



// --- forward decls ------------------------------------------------

static bool conn_is_active(s4pp_conn_t *conn);
static void free_connection(s4pp_conn_t *conn);
static s4pp_userdata_t *userdata_from_state(lua_State *L, s4pp_state_t *state);
static void report_error_real(lua_State *L, s4pp_state_t *state, int errcode, int line);
#define report_error(L,state,code) report_error_real(L,state,code,__LINE__)

// --- active_s4pps -------------------------------------------------

static s4pp_state_t *new_s4pp_state(void)
{
  s4pp_state_t *state = calloc(1, sizeof(s4pp_state_t));
  if (!state)
    return NULL;

  state->userdata_ref = LUA_NOREF;

  state->next = active_s4pps;
  active_s4pps = state;
  return state;
}

static bool state_is_active(s4pp_state_t *state)
{
  for (s4pp_state_t *i = active_s4pps; i; i = i->next)
    if (i == state)
      return true;
  return false;
}

static s4pp_state_t *state_by_conn(s4pp_conn_t *conn)
{
  for (s4pp_state_t *i = active_s4pps; i; i = i->next)
    if (i->conn == conn)
      return i;
  return NULL;
}

static void free_s4pp_state(lua_State *L, s4pp_state_t *state)
{
  if (!state)
    return;

  for (s4pp_state_t **i = &active_s4pps; *i; i = &(*i)->next)
  {
    if (*i == state)
    {
      *i = state->next;
      break;
    }
  }

  if (state->ctx)
  {
    s4pp_destroy(state->ctx);
    state->ctx = NULL;
  }
  if (state->conn)
  {
    free_connection(state->conn);
    state->conn = NULL;
  }
  free_and_clear(state->auth.key_id);
  free_and_clear(state->auth.key_bytes);
  free_and_clear(state->server.hostname);

  unref_and_clear(L, state->userdata_ref);

  free(state);
}



// --- active_conns ------------------------------------------------

static s4pp_conn_t *new_connection(void)
{
  s4pp_conn_t *conn = calloc(1, sizeof(s4pp_conn_t));
  if (!conn)
    return NULL;

  conn->next = active_conns;
  active_conns = conn;
  return conn;
}


static bool conn_is_active(s4pp_conn_t *conn)
{
  for (s4pp_conn_t **i = &active_conns; *i; i = &(*i)->next)
    if (*i == conn)
      return true;
  return false;
}


static s4pp_conn_t *conn_by_netconn(struct netconn *nc)
{
  for (s4pp_conn_t *i = active_conns; i; i = i->next)
    if (i->netconn == nc)
      return i;
  return NULL;
}


static void free_connection(s4pp_conn_t *conn)
{
  if (!conn || !conn_is_active(conn))
    return;

  for (s4pp_conn_t **i = &active_conns; *i; i = &(*i)->next)
  {
    if (*i == conn)
    {
      *i = conn->next;
      break;
    }
  }

  if (conn->netconn)
  {
    netconn_delete(conn->netconn);
    conn->netconn = NULL;
  }

  // Report the destruction of this conn, if not already done
  s4pp_state_t *state = state_by_conn(conn);
  if (state)
  {
    state->conn = NULL;
    if (state->ctx)
      s4pp_on_recv(state->ctx, NULL, 0);
  }

  free(conn);
}



// --- lwIP RTOS task handlers -----------------------------------------

// Caution: This handler runs in the lwIP RTOS task, possibly 2nd core
static void on_netconn_evt(struct netconn *nc, enum netconn_evt evt, uint16_t len)
{
  switch (evt)
  {
    case NETCONN_EVT_SENDPLUS:
    case NETCONN_EVT_RCVPLUS:
    case NETCONN_EVT_ERROR: break;
    default: return;
  }

  netconn_bounce_event_t *nbe = allocate_bounce_event();
  if (!nbe)
    goto err;
  nbe->netconn = nc;
  nbe->len = 0;

  switch (evt)
  {
    case NETCONN_EVT_SENDPLUS:
      nbe->evt = CONN_EVT_SENT;
      nbe->len = len;
      if (len == 0)
      {
        // The connection event needs to be high priority, so it doesn't get reordered with the first receive
        // event. The first receive event will have the time info from S4PP, and thus will cause an attempt to
        // set the time. We *need* to have seen the connection timestamp before that happens!
        if (!task_post_high(conn_task, (task_param_t)nbe))
          goto err;
      }
      else
      {
        if (!task_post_medium(conn_task, (task_param_t)nbe))
          goto err;
      }
      break;
    case NETCONN_EVT_RCVPLUS:
      nbe->evt = CONN_EVT_RECV;
      if (len == 0) // EOF
      {
        if (!task_post_medium(conn_task, (task_param_t)nbe))
          goto err;
      }
      else
      {
        if (!task_post_high(conn_task, (task_param_t)nbe))
          goto err;
      }
      break;
    case NETCONN_EVT_ERROR:
      nbe->evt = CONN_EVT_ERR;
      if (!task_post_medium(conn_task, (task_param_t)nbe))
        goto err;
      break;
    default: break;
  }
  return;
err:
  ESP_LOGE("s4pp", "lost network event due to lack of memory/queue");
  free(nbe);
}


// Caution: This handler runs in the lwIP RTOS task, possibly 2nd core
static void dns_resolved(const char *name, const ip_addr_t *ipaddr, void *arg)
{
  netconn_bounce_event_t *nbe = allocate_bounce_event();
  if (!nbe)
    goto err;
  nbe->conn = (s4pp_conn_t *)arg;
  nbe->evt = CONN_EVT_DNS;
  nbe->addr = ipaddr ? *ipaddr : ip_addr_any;
  if (task_post_medium(conn_task, (task_param_t)nbe))
    return;
err:
  ESP_LOGE("s4pp", "lost dns event due to lack of memory/queue");
  free(nbe);
}


static bool rotate_dns_servers(uint8_t rotations_done)
{
  ip_addr_t dns0=dns_getserver(0);

  int from;
  for (from=1;from<DNS_MAX_SERVERS;from++)
  {
    ip_addr_t tmp=dns_getserver(from);
    if (ip_addr_isany_val(tmp))
      break;
    dns_setserver(from-1,&tmp);
  }
  // 'from' now holds how many DNS servers we have
  if (from==1) // Only one server, no rotation done
    return false;
  dns_setserver(from-1,&dns0);

  return rotations_done<from;
}


// --- bounced event handling ---------------------------------------

static void handle_conn(task_param_t param, task_prio_t prio)
{
  (void)prio;
  netconn_bounce_event_t *nbe = (netconn_bounce_event_t *)param;

  s4pp_conn_t *conn =
    (nbe->evt == CONN_EVT_DNS) ? nbe->conn : conn_by_netconn(nbe->netconn);
  if (!conn || !conn_is_active(conn)) // active check needed for dns case
    goto done;

  s4pp_state_t *state = state_by_conn(conn);
  if (nbe->evt == CONN_EVT_ERR)
    goto network_err;

  if (!state || !state->ctx)
    goto err;

  switch (nbe->evt)
  {
    case CONN_EVT_SENT:
      if (nbe->len > conn->left_to_send)
      {
        ESP_LOGE("s4pp", "excessive netconn send events, %d vs %d",
          nbe->len, conn->left_to_send);
        goto network_err;
      }
      conn->left_to_send -= nbe->len;
      if (nbe->len && conn->left_to_send == 0)
        s4pp_on_sent(state->ctx);
      // If wanted, we could catch nbe->len == 0 for on-connect notification
      if (nbe->len==0)
        state->timestamps[1]=nbe->timestamp;
      break;
    case CONN_EVT_RECV:
    {
      if (state->timestamps[2]==0)
        state->timestamps[2]=nbe->timestamp;
      struct netbuf *nb = NULL;
      err_t res = netconn_recv(conn->netconn, &nb);
      if (res != ERR_OK || !nb)
        goto network_err;
      netbuf_first(nb);
      do {
        void *payload;
        uint16_t len;
        netbuf_data(nb, &payload, &len);
        // fwrite(payload, 1, len, stdout);
        s4pp_on_recv(state->ctx, payload, len);
      } while (netbuf_next(nb) != -1);
      netbuf_delete(nb);
      break;
    }
    case CONN_EVT_DNS:
      if (ip_addr_isany_val(nbe->addr))
      { // This DNS lookup failed. Let's see whether we should rotate DNS servers and try again
        bool try_again=rotate_dns_servers(state->dns_shuffle_count++);
        if (try_again)
        {
          err_t err = dns_gethostbyname(state->server.hostname, &conn->resolved_ip, dns_resolved, conn);
          if (err == ERR_OK)
            dns_resolved(state->server.hostname, &conn->resolved_ip, conn);
          else if (err != ERR_INPROGRESS)
            goto network_err;
        }
        else
        {
          report_error(lua_getstate(), state, S4PP_DNS_FAILED_COMPLETELY);
          goto err;
        }
      }
      else
      {
        conn->resolved_ip = nbe->addr;
        conn->netconn = netconn_new_with_callback(NETCONN_TCP, on_netconn_evt);
        if (!conn->netconn)
          goto network_err;
        netconn_set_nonblocking(conn->netconn, 1);
        ip_set_option(conn->netconn->pcb.tcp, SOF_KEEPALIVE);

        state->timestamps[0]=esp_timer_get_time();
        netconn_connect(conn->netconn, &conn->resolved_ip, conn->port);
      }
      break;

    default: break;
  }
  goto done;
network_err:
  report_error(lua_getstate(), state, S4PP_NETWORK_ERROR);
err:
  free_connection(conn);
done:
  free(nbe);
}



// --- s4pp I/Os -----------------------------------------------------

static s4pp_conn_t *io_connect(const s4pp_server_t *server)
{
  s4pp_conn_t *conn = new_connection();
  if (!conn)
    return NULL;

  conn->port = server->port;

  if (server->state->conn)
  {
    s4pp_conn_t *old = server->state->conn;
    server->state->conn = NULL;
    free_connection(old);
  }
  server->state->conn = conn;

  err_t err = dns_gethostbyname(
    server->hostname, &conn->resolved_ip, dns_resolved, conn);
  if (err == ERR_OK)
  {
    dns_resolved(server->hostname, &conn->resolved_ip, conn);
    return conn;
  }
  else if (err == ERR_INPROGRESS)
    return conn;
  else
  {
    free_connection(conn);
    report_error(lua_getstate(), server->state, S4PP_NETWORK_ERROR);
    return NULL;
  }
}


static void io_disconnect(s4pp_conn_t *conn)
{
  s4pp_state_t *state = state_by_conn(conn);
  if (state)
  {
    state->conn = NULL; // avoid triggering s4pp_on_recv() in the below free
    free_connection(conn); // free before we invoke an error callback

    int errcode = state->ctx ? s4pp_last_error(state->ctx) : S4PP_OK;
    if (errcode != S4PP_OK)
      report_error(lua_getstate(), state, errcode);
  }
  else
    free_connection(conn);
}


static bool io_send(s4pp_conn_t *conn, const char *data, uint16_t len)
{
  if (!conn_is_active(conn))
  {
    ESP_LOGE("s4pp", "io_send() on non-active conn?!");
    return false;
  }
  // fwrite(data, 1, len, stdout);

  size_t written = 0;
  err_t res =
    netconn_write_partly(conn->netconn, data, len, NETCONN_COPY, &written);
  if (res != ERR_OK || written != len)
    return false; // this will result in a disconnect

  conn->left_to_send += len;
  return true;
}



// --- Lua / s4pp glue -----------------------------------------------

static void report_error_real(lua_State *L, s4pp_state_t *state, int errcode, int line)
{
  int top = lua_gettop(L);
  s4pp_userdata_t *sud = userdata_from_state(L, state);
  if (sud->error_ref != LUA_NOREF)
  {
    lua_rawgeti(L, LUA_REGISTRYINDEX, sud->error_ref);
    lua_pushinteger(L, errcode);
    lua_pushinteger(L, line);
    lua_call(L, 2, 0);
  }
  lua_settop(L, top);
}


static s4pp_userdata_t *userdata_from_state(lua_State *L, s4pp_state_t *state)
{
  lua_checkstack(L, 10);
  lua_rawgeti(L, LUA_REGISTRYINDEX, state->userdata_ref);
  luaL_checkudata(L, -1, S4PP_TABLE_INSTANCE);
  return (s4pp_userdata_t *)lua_touserdata(L, -1);
}


static void s4pp_handle_event(task_param_t param, task_prio_t prio)
{
  (void)prio;
  s4pp_state_t *state = (s4pp_state_t *)param;
  if (!state_is_active(state))
    return;

  if (state->pending_evts & SUBMIT_DONE_EVT)
  {
    state->pending_evts ^= SUBMIT_DONE_EVT;
    lua_State *L = lua_getstate();
    int top = lua_gettop(L);
    s4pp_userdata_t *sud = userdata_from_state(L, state);

    unref_and_clear(L, sud->submit_ref);

    if (sud->submit_done_ref != LUA_NOREF)
    {
      lua_rawgeti(L, LUA_REGISTRYINDEX, sud->submit_done_ref);
      unref_and_clear(L, sud->submit_done_ref);
      lua_call(L, 0, 0);
    }
    lua_settop(L, top);
  }
}



// --- Lua interface ----------------------------------------------------

static s4pp_userdata_t *get_userdata(lua_State *L)
{
  luaL_checkudata(L, 1, S4PP_TABLE_INSTANCE);
  return (s4pp_userdata_t *)lua_touserdata(L, 1);
}


static bool on_pull(s4pp_ctx_t *ctx, s4pp_sample_t *sample)
{
  s4pp_state_t *state = (s4pp_state_t *)s4pp_user_arg(ctx);
  if (!state_is_active(state))
    return false;

  lua_State *L = lua_getstate();
  int top = lua_gettop(L);
  s4pp_userdata_t *sud = userdata_from_state(L, state);

  if (sud->submit_ref == LUA_NOREF)
  {
    lua_settop(L, top);
    return false;
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, sud->submit_ref);
  lua_rawgeti(L, -1, sud->submit_idx++); // get array entry
  if (lua_isnil(L, -1))
  {
    lua_settop(L, top);
    return false;
  }

  int entry = lua_gettop(L);

  lua_getfield(L, entry, "time");
  sample->timestamp = luaL_checkinteger(L, -1);

  lua_getfield(L, entry, "span");
  sample->span = luaL_optint(L, -1, 0);

  lua_getfield(L, entry, "name");
  sample->name = luaL_checkstring(L, -1);

  lua_getfield(L, entry, "value");
  if (lua_isnumber(L, -1))
  {
    sample->val.numeric = luaL_checknumber(L, -1);
    sample->type = S4PP_NUMERIC;
  }
  else
  {
    sample->val.formatted = luaL_checkstring(L, -1);
    sample->type = S4PP_FORMATTED;
  }

  // printf("||| %lu %u %s %s\n", sample->timestamp, sample->span, sample->name, sample->val.formatted);

  lua_settop(L, top);
  return true;
}


static void on_pull_done(s4pp_ctx_t *ctx)
{
  s4pp_state_t *state = (s4pp_state_t *)s4pp_user_arg(ctx);
  if (!state_is_active(state))
    return;

  // we task post so we don't have to deal with nested s4pp_pull calls
  state->pending_evts |= SUBMIT_DONE_EVT;
  task_post_medium(s4pp_task, (task_param_t)state);
}


// client:submit({ { time=, span=, name=, value= }, ... }, done_fn)
static int ls4pp_submit(lua_State *L)
{
  s4pp_userdata_t *sud = get_userdata(L);
  if (!state_is_active(sud->state) || !sud->state->ctx)
    return luaL_error(L, "s4pp submit after close");

  luaL_checkanytable(L, 2);
  luaL_checkanyfunction(L, 3);
  lua_settop(L, 3);

  if (sud->submit_ref != LUA_NOREF)
    return luaL_error(L, "submit already in progress");

  luaL_unref(L, LUA_REGISTRYINDEX, sud->submit_done_ref);
  sud->submit_done_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  sud->submit_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  sud->submit_idx = 1;

  s4pp_pull(sud->state->ctx, on_pull, on_pull_done);
  return 0;
}


#if CONFIG_LUA_MODULE_FLASHFIFO
extern int flash_fifo_fill_s4pp_sample(s4pp_sample_t *sample, uint32_t idx);
extern bool flash_fifo_drop_samples(uint32_t from_top);

static bool on_fifo_pull(s4pp_ctx_t *ctx, s4pp_sample_t *sample)
{
  s4pp_state_t *state = (s4pp_state_t *)s4pp_user_arg(ctx);
  if (!state_is_active(state))
    return false;

  if (state->fifo_max != -1 && state->fifo_consumed >= state->fifo_max)
    return false;

  int n = flash_fifo_fill_s4pp_sample(sample, state->fifo_consumed);
  if (n < 0)
  {
    state->fifo_consumed += -n;
    return false;
  }
  else if (n > 0)
  {
    state->fifo_consumed += n;
    return true;
  }
  else
    return false;
}


// Lua: client:submit_flash_fifo(done_fn, max_n)
static int ls4pp_submit_flash_fifo(lua_State *L)
{
  s4pp_userdata_t *sud = get_userdata(L);
  if (!state_is_active(sud->state) || !sud->state->ctx)
    return luaL_error(L, "s4pp submit after close");

  if (sud->submit_ref != LUA_NOREF)
    return luaL_error(L, "submit already in progress");

  luaL_checkanyfunction(L, 2);
  sud->state->fifo_max = luaL_optint(L, 3, -1);
  lua_settop(L, 2); // toss the max_n, if any

  luaL_unref(L, LUA_REGISTRYINDEX, sud->submit_done_ref);
  sud->submit_done_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  // note: we only use this as a guard against concurrent submits
  sud->submit_ref = LUA_REFNIL;

  sud->state->fifo_consumed = 0;

  s4pp_pull(sud->state->ctx, on_fifo_pull, on_pull_done);

  return 0;
}
#endif


static void on_commit(s4pp_ctx_t *ctx, bool success, unsigned num_items)
{
  s4pp_state_t *state = (s4pp_state_t *)s4pp_user_arg(ctx);

#if CONFIG_LUA_MODULE_FLASHFIFO
  if (success && state->fifo_consumed)
  {
    flash_fifo_drop_samples(state->fifo_consumed);
    state->fifo_consumed = 0;
  }
#endif

  lua_State *L = lua_getstate();
  int top = lua_gettop(L);
  s4pp_userdata_t *sud = userdata_from_state(L, state);

  if (success && sud->commit_ref != LUA_NOREF)
  {
    lua_rawgeti(L, LUA_REGISTRYINDEX, sud->commit_ref);
    lua_pushinteger(L, num_items);
    lua_call(L, 1, 0);
  }
  else if (!success && sud->error_ref != LUA_NOREF)
    report_error(L, state, s4pp_last_error(ctx));

  lua_settop(L, top);
}


static int ls4pp_commit(lua_State *L)
{
  s4pp_userdata_t *sud = get_userdata(L);
  if (!state_is_active(sud->state) || !sud->state->ctx)
    return luaL_error(L, "s4pp commit after close");

  s4pp_flush(sud->state->ctx);

  return 0;
}


static int ls4pp_status(lua_State *L)
{
  s4pp_userdata_t *sud = get_userdata(L);
  if (!state_is_active(sud->state) || !sud->state->ctx)
    return luaL_error(L, "already closed");

  lua_pushinteger(L, s4pp_last_error(sud->state->ctx));
  return 1;
}


// Note: also used for client:close()
static int ls4pp_gc(lua_State *L)
{
  s4pp_userdata_t *sud = get_userdata(L);

  unref_and_clear(L, sud->notify_ref);
  unref_and_clear(L, sud->commit_ref);
  unref_and_clear(L, sud->error_ref);
  unref_and_clear(L, sud->submit_ref);
  unref_and_clear(L, sud->submit_done_ref);

  if (state_is_active(sud->state))
  {
    s4pp_state_t *state = sud->state;
    sud->state = NULL;
    free_s4pp_state(L, state);
  }

  return 0;
}


static void on_notify(s4pp_ctx_t *ctx, unsigned code, unsigned nargs, const char *args[])
{
  s4pp_state_t *state = (s4pp_state_t *)s4pp_user_arg(ctx);

  lua_State *L = lua_getstate();
  int top = lua_gettop(L);
  s4pp_userdata_t *sud = userdata_from_state(L, state);

  if (sud->notify_ref != LUA_NOREF)
  {
    lua_checkstack(L, nargs + 2);
    lua_rawgeti(L, LUA_REGISTRYINDEX, sud->notify_ref);
    lua_pushinteger(L, code);
    for (unsigned i = 0; i < nargs; ++i)
      lua_pushstring(L, args[i]);
    if (code==0)
    {
      int64_t now=esp_timer_get_time();

      lua_pushnumber(L, (lua_Number)(state->timestamps[0] & 0x1FFFFFFFFFFFFFull));
      lua_pushnumber(L, (lua_Number)(state->timestamps[1] & 0x1FFFFFFFFFFFFFull));
      lua_pushnumber(L, (lua_Number)(state->timestamps[2] & 0x1FFFFFFFFFFFFFull));
      lua_pushnumber(L, (lua_Number)(now & 0x1FFFFFFFFFFFFFull));
      nargs+=4;
    }
    lua_call(L, nargs + 1, 0);
  }
  lua_settop(L, top);
}


// inst:on('notify', fn)
static int ls4pp_on(lua_State *L)
{
  s4pp_userdata_t *sud = get_userdata(L);

  static const char *cbs[] = {
    "notify",
    "commit",
    "error",
    NULL,
  };
  int opt = luaL_checkoption(L, 2, NULL, cbs);
  luaL_checkanyfunction(L, 3);
  lua_settop(L, 3);

  switch (opt)
  {
    case 0: // notify
      luaL_unref(L, LUA_REGISTRYINDEX, sud->notify_ref);
      sud->notify_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      break;
    case 1: // commit
      luaL_unref(L, LUA_REGISTRYINDEX, sud->commit_ref);
      sud->commit_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      break;
    case 2: // error
      luaL_unref(L, LUA_REGISTRYINDEX, sud->error_ref);
      sud->error_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      break;
    default:
      return luaL_error(L, "inconceivable!"); // luaL_checkopt() should prevent
  }

  return 0;
}


// s4pp.create({ server=, port=, user=, key=, hide=0/nil/1, format=0/1 })
static int ls4pp_create(lua_State *L)
{
  luaL_checkanytable(L, 1);
  lua_settop(L, 1);

  s4pp_userdata_t *sud =
    (s4pp_userdata_t *)lua_newuserdata(L, sizeof(s4pp_userdata_t));
  memset(sud, 0, sizeof(*sud));
  luaL_getmetatable(L, S4PP_TABLE_INSTANCE);
  lua_setmetatable(L, -2);

  s4pp_state_t *state = new_s4pp_state();
  if (!state)
    return luaL_error(L, "out of memory");
  sud->state = state;

  lua_pushvalue(L, -1);
  state->userdata_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  sud->notify_ref = LUA_NOREF;
  sud->commit_ref = LUA_NOREF;
  sud->error_ref  = LUA_NOREF;
  sud->submit_ref = LUA_NOREF;
  sud->submit_done_ref = LUA_NOREF;
  sud->submit_idx = 0;

  lua_getfield(L, 1, "user");
  state->auth.key_id = strdup(luaL_checkstring(L, -1));
  lua_getfield(L, 1, "key");
  size_t len;
  const char *key_bytes = luaL_checklstring(L, -1, &len);
  state->auth.key_bytes = malloc(len);
  if (!state->auth.key_bytes)
    return luaL_error(L, "out of memory");
  memcpy(state->auth.key_bytes, key_bytes, len);
  state->auth.key_len = len;

  lua_getfield(L, 1, "server");
  state->server.hostname = strdup(luaL_checkstring(L, -1));
  lua_getfield(L, 1, "port");
  state->server.port = luaL_optint(L, -1, 22226);
  state->server.state = state;

  lua_getfield(L, 1, "hide");
  s4pp_hide_mode_t hide = S4PP_HIDE_PREFERRED;
  switch (luaL_optint(L, -1, -1))
  {
    case 0: hide = S4PP_HIDE_DISABLED; break;
    case 1: hide = S4PP_HIDE_MANDATORY; break;
    default: break;
  }

  lua_getfield(L, 1, "format");
  int data_format = luaL_optint(L, -1, 0);

  static const s4pp_io_t ios = {
    .connect = io_connect,
    .disconnect = io_disconnect,
    .send = io_send,
    .max_payload = 1400
  };

  state->ctx = s4pp_create_glued(
    &ios, &state->auth, &state->server, hide, data_format, state);

  s4pp_set_notification_handler(state->ctx, on_notify);
  s4pp_set_commit_handler(state->ctx, on_commit);

  lua_settop(L, 2); // discard back to our userdata
  return 1;
}


static int ls4pp_sessions(lua_State *L)
{
  size_t n = 0;
  for (s4pp_state_t *i = active_s4pps; i; i = i->next)
    ++n;

  lua_settop(L, 0);
  lua_createtable(L, n, 0);

  for (s4pp_state_t *i = active_s4pps; i; i = i->next)
  {
    lua_rawgeti(L, LUA_REGISTRYINDEX, i->userdata_ref);
    lua_rawseti(L, 1, n--);
  }
  return 1;
}


LROT_BEGIN(s4pp_instance)
  LROT_FUNCENTRY( on,                 ls4pp_on )
#if CONFIG_LUA_MODULE_FLASHFIFO
  LROT_FUNCENTRY( submit_flash_fifo,  ls4pp_submit_flash_fifo )
#endif
  LROT_FUNCENTRY( submit,             ls4pp_submit )
  LROT_FUNCENTRY( commit,             ls4pp_commit )
  LROT_FUNCENTRY( close,              ls4pp_gc )
  LROT_FUNCENTRY( status,             ls4pp_status )
  LROT_FUNCENTRY( __gc,               ls4pp_gc )
  LROT_TABENTRY(  __index,            s4pp_instance )
LROT_END(s4pp_instance, NULL, 0)


LROT_BEGIN(s4pp)
  LROT_FUNCENTRY( create,   ls4pp_create )
  LROT_FUNCENTRY( sessions, ls4pp_sessions )
LROT_END(s4pp, NULL, 0)


static int luaopen_s4pp(lua_State *L)
{
  luaL_rometatable(L, S4PP_TABLE_INSTANCE, (void *)s4pp_instance_map);

  s4pp_task = task_get_id(s4pp_handle_event);
  conn_task = task_get_id(handle_conn);

  return 0;
}

NODEMCU_MODULE(S4PP, "s4pp", s4pp, luaopen_s4pp);
