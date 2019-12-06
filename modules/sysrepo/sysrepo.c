/*  Copyright (C) 2019 CZ.NIC, z.s.p.o. <knot-resolver@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <lua.h>
#include <sysrepo.h>

#include "lib/module.h"
#include "callbacks.h"
#include "utils/common/sysrepo_conf.h"


typedef struct el_subscription_ctx el_subscription_ctx_t;
/** Callback for our sysrepo subscriptions */
typedef void (*el_subsription_cb)(el_subscription_ctx_t *el_subscr, int status);

/** Context for our sysrepo subscriptions.
 * might add some other fields in future */
struct el_subscription_ctx {
    sr_conn_ctx_t *connection;
    sr_session_ctx_t * session;
	sr_subscription_ctx_t *subscription;
	el_subsription_cb callback;
	uv_poll_t uv_handle;
};

void el_subscr_finish_closing(uv_handle_t *handle)
{
	el_subscription_ctx_t *el_subscr = handle->data;
	assert(el_subscr);
	free(el_subscr);
}

/** Free a event loop subscription. */
void el_subscription_free(el_subscription_ctx_t *el_subscr)
{
    sr_disconnect(el_subscr->connection);
	uv_close((uv_handle_t *)&el_subscr->uv_handle, el_subscr_finish_closing);  
}

static void el_subscr_cb_tramp(uv_poll_t *handle, int status, int events)
{
	el_subscription_ctx_t *el_subscr = handle->data;
	el_subscr->callback(el_subscr, status);
}

/** Start a new event loop subscription.  */
static el_subscription_ctx_t * el_subscription_new(sr_subscription_ctx_t *sr_subscr, el_subsription_cb el_callback)
{
	int fd;
	errno = sr_get_event_pipe(sr_subscr, &fd);
	if (errno != SR_ERR_OK) return NULL;
	el_subscription_ctx_t *el_subscr = malloc(sizeof(*el_subscr));
	if (!el_subscr) return NULL;
	errno = uv_poll_init(uv_default_loop(), &el_subscr->uv_handle, fd);
	if (errno) {
		free(el_subscr);
		return NULL;
	}
    el_subscr->subscription = sr_subscr;
    el_subscr->callback = el_callback;
	el_subscr->uv_handle.data = el_subscr;
	errno = uv_poll_start(&el_subscr->uv_handle, UV_READABLE, el_subscr_cb_tramp);
	if (errno) {
		el_subscription_free(el_subscr);
		return NULL;
	}
	return el_subscr;
}

static void el_subscr_cb(el_subscription_ctx_t *el_subscr, int status)
{
	if (status) {
		/* some error */
		return;
	}
	/* normal state */
    sr_process_events(el_subscr->subscription, el_subscr->session,NULL);
}

KR_EXPORT
int sysrepo_init(struct kr_module *module)
{
    char *xpath = NULL;
    int sr_err = SR_ERR_OK;
    sr_conn_ctx_t *sr_connection = NULL;
    sr_session_ctx_t *sr_session = NULL;
    sr_subscription_ctx_t *sr_subscription = NULL;

    sr_err = sr_connect(0, &sr_connection);
    if (sr_err != SR_ERR_OK) goto cleanup;

    //sr_err = sr_connection_recover(sr_connection);
    //if (sr_err != SR_ERR_OK) goto cleanup;

    sr_err = sr_session_start(sr_connection, SR_DS_RUNNING, &sr_session);
    if (sr_err != SR_ERR_OK) goto cleanup;

    /* read and set current running configuration */
    sr_err = conf_set_current(sr_session, YM_COMMON);
    if (sr_err != SR_ERR_OK) goto cleanup;

    /* setup sysrepo subscriptions */
    sr_err = sr_module_change_subscribe(sr_session, YM_COMMON, XPATH_BASE"/cache", conf_cache_cb, NULL, 0, SR_SUBSCR_NO_THREAD, &sr_subscription);
    if (sr_err != SR_ERR_OK) goto cleanup;

    /* additional sysrepo subscriptions will be here */
    
    /* add subscriptions to kres event loop */
    el_subscription_ctx_t *el_subscr = el_subscription_new(sr_subscription, el_subscr_cb);
    el_subscr->connection = sr_connection;
    el_subscr->session = sr_session;
    module->data = el_subscr;

    free(xpath);
    return kr_ok(); 

    cleanup:
        free(xpath);
        sr_disconnect(sr_connection);
        printf("Error (%s)\n", sr_strerror(sr_err));
        return kr_error(sr_err);
}

KR_EXPORT
int sysrepo_deinit(struct kr_module *module)
{
    el_subscription_ctx_t *el_subscr = module->data;
    el_subscription_free(el_subscr);  
    return kr_ok();
}

KR_MODULE_EXPORT(sysrepo)