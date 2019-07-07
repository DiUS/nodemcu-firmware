#ifndef _S4PP_COMPONENT_H_
#define _S4PP_COMPONENT_H_

#include "../s4pp-client/s4pp.h"

s4pp_ctx_t *s4pp_create_glued(const s4pp_io_t *io, const s4pp_auth_t *auth, const s4pp_server_t *server, s4pp_hide_mode_t hide_mode, int data_format, void *user_arg);

#endif
