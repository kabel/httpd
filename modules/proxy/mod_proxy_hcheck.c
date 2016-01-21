/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "mod_proxy.h"
#include "mod_watchdog.h"
#include "ap_slotmem.h"
#include "ap_expr.h"

module AP_MODULE_DECLARE_DATA proxy_hcheck_module;

#define HCHECK_WATHCHDOG_NAME ("_proxy_hcheck_")

typedef struct {
    char *name;
    hcmethod_t method;
    int passes;
    int fails;
    apr_interval_time_t interval;
    char *hurl;
    char *hcexpr;
} hc_template_t;

typedef struct {
    char *expr;
    ap_expr_info_t *pexpr;       /* parsed expression */
} hc_condition_t;

typedef struct {
    apr_pool_t *p;
    apr_bucket_alloc_t *ba;
    apr_array_header_t *templates;
    apr_table_t *conditions;
    ap_watchdog_t *watchdog;
    apr_hash_t *hcworkers;
    server_rec *s;
} sctx_t;

/* Used in the HC worker via the context field */
typedef struct {
    char *path;      /* The path of the original worker URL */
    char *req;       /* pre-formatted HTTP/AJP request */
    proxy_worker *w; /* Pointer to the actual worker */
} wctx_t;

static void *hc_create_config(apr_pool_t *p, server_rec *s)
{
    sctx_t *ctx = (sctx_t *) apr_palloc(p, sizeof(sctx_t));
    apr_pool_create(&ctx->p, p);
    ctx->ba = apr_bucket_alloc_create(p);
    ctx->templates = apr_array_make(ctx->p, 10, sizeof(hc_template_t));
    ctx->conditions = apr_table_make(ctx->p, 10);
    ctx->hcworkers = apr_hash_make(ctx->p);
    ctx->s = s;

    return ctx;
}

/*
 * This serves double duty by not only validating (and creating)
 * the health-check template, but also ties into set_worker_param()
 * which does the actual setting of worker params in shm.
 */
static const char *set_worker_hc_param(apr_pool_t *p,
                                    server_rec *s,
                                    proxy_worker *worker,
                                    const char *key,
                                    const char *val,
                                    void *v)
{
    int ival;
    hc_template_t *temp;
    sctx_t *ctx = (sctx_t *) ap_get_module_config(s->module_config,
                                                  &proxy_hcheck_module);
    if (!worker && !v) {
        return "Bad call to set_worker_hc_param()";
    }
    temp = (hc_template_t *)v;
    if (!strcasecmp(key, "hctemplate")) {
        hc_template_t *template;
        template = (hc_template_t *)ctx->templates->elts;
        for (ival = 0; ival < ctx->templates->nelts; ival++, template++) {
            if (!ap_casecmpstr(template->name, val)) {
                if (worker) {
                    worker->s->method = template->method;
                    worker->s->interval = template->interval;
                    worker->s->passes = template->passes;
                    worker->s->fails = template->fails;
                    PROXY_STRNCPY(worker->s->hcuri, template->hurl);
                    PROXY_STRNCPY(worker->s->hcexpr, template->hcexpr);
                } else {
                    temp->method = template->method;
                    temp->interval = template->interval;
                    temp->passes = template->passes;
                    temp->fails = template->fails;
                    temp->hurl = apr_pstrdup(p, template->hurl);
                    temp->hcexpr = apr_pstrdup(p, template->hcexpr);
                }
                return NULL;
            }
        }
        return apr_psprintf(p, "Unknown ProxyHCTemplate name: %s", val);
    }
    else if (!strcasecmp(key, "hcmethod")) {
        hcmethods_t *method = hcmethods;
        for (; method->name; method++) {
            if (!ap_casecmpstr(val, method->name)) {
                if (!method->implemented) {
                    return apr_psprintf(p, "Health check method %s not (yet) implemented",
                                        val);
                }
                if (worker) {
                    worker->s->method = method->method;
                } else {
                    temp->method = method->method;
                }
                return NULL;
            }
        }
        return "Unknown method";
    }
    else if (!strcasecmp(key, "hcinterval")) {
        ival = atoi(val);
        if (ival < HCHECK_WATHCHDOG_INTERVAL)
            return apr_psprintf(p, "Interval must be a positive value greater than %d seconds",
                                HCHECK_WATHCHDOG_INTERVAL);
        if (worker) {
            worker->s->interval = apr_time_from_sec(ival);
        } else {
            temp->interval = apr_time_from_sec(ival);
        }
    }
    else if (!strcasecmp(key, "hcpasses")) {
        ival = atoi(val);
        if (ival < 0)
            return "Passes must be a positive value";
        if (worker) {
            worker->s->passes = ival;
        } else {
            temp->passes = ival;
        }
    }
    else if (!strcasecmp(key, "hcfails")) {
        ival = atoi(val);
        if (ival < 0)
            return "Fails must be a positive value";
        if (worker) {
            worker->s->fails = ival;
        } else {
            temp->fails = ival;
        }
    }
    else if (!strcasecmp(key, "hcuri")) {
        if (strlen(val) >= sizeof(worker->s->hcuri))
            return apr_psprintf(p, "Health check uri length must be < %d characters",
                    (int)sizeof(worker->s->hcuri));
        if (worker) {
            PROXY_STRNCPY(worker->s->hcuri, val);
        } else {
            temp->hurl = apr_pstrdup(p, val);
        }
    }
    else if (!strcasecmp(key, "hcexpr")) {
        hc_condition_t *cond;
        cond = (hc_condition_t *)apr_table_get(ctx->conditions, val);
        if (!cond) {
            return apr_psprintf(p, "Unknown health check condition expr: %s", val);
        }
        /* This check is wonky... a known expr can't be this big. Check anyway */
        if (strlen(val) >= sizeof(worker->s->hcexpr))
            return apr_psprintf(p, "Health check uri length must be < %d characters",
                    (int)sizeof(worker->s->hcexpr));
        if (worker) {
            PROXY_STRNCPY(worker->s->hcexpr, val);
        } else {
            temp->hcexpr = apr_pstrdup(p, val);
        }
    }
  else {
        return "unknown Worker hcheck parameter";
    }
    return NULL;
}

static const char *set_hc_condition(cmd_parms *cmd, void *dummy, const char *arg)
{
    char *name = NULL;
    char *expr;
    sctx_t *ctx;
    hc_condition_t *cond;

    const char *err = ap_check_cmd_context(cmd, NOT_IN_HTACCESS);
    if (err)
        return err;
    ctx = (sctx_t *) ap_get_module_config(cmd->server->module_config,
                                          &proxy_hcheck_module);

    name = ap_getword_conf(cmd->temp_pool, &arg);
    if (!*name) {
        return apr_pstrcat(cmd->temp_pool, "Missing expression name for ",
                           cmd->cmd->name, NULL);
    }
    if (strlen(name) > (PROXY_WORKER_MAX_SCHEME_SIZE - 1)) {
        return apr_psprintf(cmd->temp_pool, "Expression name limited to %d characters",
                           (PROXY_WORKER_MAX_SCHEME_SIZE - 1));
    }
    /* get expr. Allow fancy new {...} quoting style */
    expr = ap_getword_conf2(cmd->temp_pool, &arg);
    if (!*expr) {
        return apr_pstrcat(cmd->temp_pool, "Missing expression for ",
                           cmd->cmd->name, NULL);
    }
    cond = apr_palloc(ctx->p, sizeof(hc_condition_t));
    cond->pexpr = ap_expr_parse_cmd(cmd, expr, 0, &err, NULL);
    if (err) {
        return apr_psprintf(cmd->temp_pool, "Could not parse expression \"%s\": %s",
                            expr, err);
    }
    cond->expr = apr_pstrdup(cmd->pool, expr);
    apr_table_setn(ctx->conditions, name, (void *)cond);
    expr = ap_getword_conf(cmd->temp_pool, &arg);
    if (*expr) {
        return "error: extra parameter(s)";
    }

    return NULL;
}

static const char *set_hc_template(cmd_parms *cmd, void *dummy, const char *arg)
{
    char *name = NULL;
    char *word, *val;
    hc_template_t *template;
    sctx_t *ctx;

    const char *err = ap_check_cmd_context(cmd, NOT_IN_HTACCESS);
    if (err)
        return err;
    ctx = (sctx_t *) ap_get_module_config(cmd->server->module_config,
                                          &proxy_hcheck_module);

    name = ap_getword_conf(cmd->temp_pool, &arg);
    if (!*name) {
        return apr_pstrcat(cmd->temp_pool, "Missing template name for ",
                           cmd->cmd->name, NULL);
    }

    template = (hc_template_t *)apr_array_push(ctx->templates);

    template->name = apr_pstrdup(ctx->p, name);
    template->method = template->passes = template->fails = 1;
    template->interval = apr_time_from_sec(HCHECK_WATHCHDOG_DEFAULT_INTERVAL);
    template->hurl = NULL;
    while (*arg) {
        word = ap_getword_conf(cmd->pool, &arg);
        val = strchr(word, '=');
        if (!val) {
            return "Invalid ProxyHCTemplate parameter. Parameter must be "
                   "in the form 'key=value'";
        }
        else
            *val++ = '\0';
        err = set_worker_hc_param(ctx->p, ctx->s, NULL, word, val, template);

        if (err) {
            /* get rid of recently pushed (bad) template */
            apr_array_pop(ctx->templates);
            return apr_pstrcat(cmd->temp_pool, "ProxyHCTemplate: ", err, " ", word, "=", val, "; ", name, NULL);
        }
        /* No error means we have a valid template */
    }

    return NULL;
}

/*
 * Create a dummy request rec, simply so we can use ap_expr.
 * Use our short-lived poll for bucket_alloc
 */
static request_rec *create_request_rec(apr_pool_t *p1, conn_rec *conn, const char *method)
{
    request_rec *r;
    apr_pool_t *p;
    apr_bucket_alloc_t *ba;
    apr_pool_create(&p, p1);
    apr_pool_tag(p, "request");
    r = apr_pcalloc(p, sizeof(request_rec));
    ba = apr_bucket_alloc_create(p);
    r->pool            = p;
    r->connection      = conn;
    r->connection->bucket_alloc = ba;
    r->server          = conn->base_server;

    r->user            = NULL;
    r->ap_auth_type    = NULL;

    r->allowed_methods = ap_make_method_list(p, 2);

    r->headers_in      = apr_table_make(r->pool, 25);
    r->trailers_in     = apr_table_make(r->pool, 5);
    r->subprocess_env  = apr_table_make(r->pool, 25);
    r->headers_out     = apr_table_make(r->pool, 12);
    r->err_headers_out = apr_table_make(r->pool, 5);
    r->trailers_out    = apr_table_make(r->pool, 5);
    r->notes           = apr_table_make(r->pool, 5);

    r->kept_body       = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    r->request_config  = ap_create_request_config(r->pool);
    /* Must be set before we run create request hook */

    r->proto_output_filters = conn->output_filters;
    r->output_filters  = r->proto_output_filters;
    r->proto_input_filters = conn->input_filters;
    r->input_filters   = r->proto_input_filters;
    r->per_dir_config  = r->server->lookup_defaults;

    r->sent_bodyct     = 0;                      /* bytect isn't for body */

    r->read_length     = 0;
    r->read_body       = REQUEST_NO_BODY;

    r->status          = HTTP_OK;  /* Until further notice */
    r->header_only     = 1;
    r->the_request     = NULL;

    /* Begin by presuming any module can make its own path_info assumptions,
     * until some module interjects and changes the value.
     */
    r->used_path_info = AP_REQ_DEFAULT_PATH_INFO;

    r->useragent_addr = conn->client_addr;
    r->useragent_ip = conn->client_ip;


    /* Time to populate r with the data we have. */
    r->method = method;
    /* Provide quick information about the request method as soon as known */
    r->method_number = ap_method_number_of(r->method);
    if (r->method_number == M_GET && r->method[0] == 'G') {
        r->header_only = 0;
    }

    r->protocol = (char*)"HTTP/1.1";
    r->proto_num = HTTP_VERSION(1, 1);

    r->hostname = NULL;

    return r;
}

static proxy_worker *hc_get_hcworker(sctx_t *ctx, proxy_worker *worker,
                                     apr_pool_t *p)
{
    proxy_worker *hc = NULL;
    const char* wptr;

    wptr = apr_psprintf(ctx->p, "%pp", worker);
    hc = (proxy_worker *)apr_hash_get(ctx->hcworkers, wptr, APR_HASH_KEY_STRING);
    if (!hc) {
        apr_uri_t uri;
        apr_status_t rv;
        const char *url = worker->s->name;
        apr_port_t port;
        wctx_t *wctx = apr_pcalloc(ctx->p, sizeof(wctx_t));
        port = (worker->s->port ? worker->s->port : ap_proxy_port_of_scheme(worker->s->scheme));

        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ctx->s, APLOGNO(03248)
                     "Creating hc worker %s for %s://%s:%d",
                     wptr, worker->s->scheme, worker->s->hostname,
                     (int)port);

        ap_proxy_define_worker(ctx->p, &hc, NULL, NULL, worker->s->name, 0);
        PROXY_STRNCPY(hc->s->name,     wptr);
        PROXY_STRNCPY(hc->s->hostname, worker->s->hostname);
        PROXY_STRNCPY(hc->s->scheme,   worker->s->scheme);
        hc->hash.def = hc->s->hash.def = ap_proxy_hashfunc(hc->s->name, PROXY_HASHFUNC_DEFAULT);
        hc->hash.fnv = hc->s->hash.fnv = ap_proxy_hashfunc(hc->s->name, PROXY_HASHFUNC_FNV);
        hc->s->port = port;
        /* Do not disable worker in case of errors */
        hc->s->status |= PROXY_WORKER_IGNORE_ERRORS;
        /* Mark as the "generic" worker */
        hc->s->status |= PROXY_WORKER_GENERIC;
        ap_proxy_initialize_worker(hc, ctx->s, ctx->p);
        hc->s->is_address_reusable = worker->s->is_address_reusable;
        hc->s->disablereuse = worker->s->disablereuse;
        hc->s->method = worker->s->method;
        rv = apr_uri_parse(p, url, &uri);
        if (rv == APR_SUCCESS) {
            wctx->path = apr_pstrdup(ctx->p, uri.path);
        }
        wctx->w = worker;
        hc->context = wctx;
        apr_hash_set(ctx->hcworkers, wptr, APR_HASH_KEY_STRING, hc);
    }
    /* This *could* have changed via the Balancer Manager */
    if (hc->s->method != worker->s->method) {
        wctx_t *wctx = hc->s->context;
        hc->s->method = worker->s->method;
        wctx->req = NULL;
        apr_hash_set(ctx->hcworkers, wptr, APR_HASH_KEY_STRING, hc);
    }
    return hc;
}

static int hc_determine_connection(sctx_t *ctx, proxy_worker *worker) {
    apr_status_t rv = APR_SUCCESS;
    int will_reuse = worker->s->is_address_reusable && !worker->s->disablereuse;
    /*
     * normally, this is done in ap_proxy_determine_connection().
     * TODO: Look at using ap_proxy_determine_connection() with a
     * fake request_rec
     */
    if (!worker->cp->addr || !will_reuse) {
        rv = apr_sockaddr_info_get(&(worker->cp->addr), worker->s->hostname, APR_UNSPEC,
                                   worker->s->port, 0, ctx->p);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ctx->s, APLOGNO(03249)
                         "DNS lookup failure for: %s:%d",
                         worker->s->hostname, (int)worker->s->port);
        }
    }
    return (rv == APR_SUCCESS ? OK : !OK);
}

static apr_status_t hc_init_worker(sctx_t *ctx, proxy_worker *worker) {
    apr_status_t rv = APR_SUCCESS;
    /*
     * Since this is the watchdog, workers never actually handle a
     * request here, and so the local data isn't initialized (of
     * course, the shared memory is). So we need to bootstrap
     * worker->cp. Note, we only need do this once.
     */
    if (!worker->cp) {
        rv = ap_proxy_initialize_worker(worker, ctx->s, ctx->p);
        if (rv != APR_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, rv, ctx->s, APLOGNO(03250) "Cannot init worker");
            return rv;
        }
        rv = (hc_determine_connection(ctx, worker) == OK ? APR_SUCCESS : APR_EGENERAL);
    }
    return rv;
}

static apr_status_t backend_cleanup(const char *proxy_function, proxy_conn_rec *backend,
                                    server_rec *s, int status)
{
    if (backend) {
        backend->close = 1;
        ap_proxy_release_connection(proxy_function, backend, s);
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, APLOGNO(03251)
                     "Health check %s Status (%d) for %s.",
                     ap_proxy_show_hcmethod(backend->worker->s->method),
                     status,
                     backend->worker->s->name);
    if (status != OK) {
        return APR_EGENERAL;
    }
    return APR_SUCCESS;
}

static int hc_get_backend(apr_pool_t *p, const char *proxy_function, proxy_conn_rec **backend,
                          proxy_worker *hc, sctx_t *ctx)
{
    int status;
    status = ap_proxy_acquire_connection(proxy_function, backend, hc, ctx->s);
    if (status == OK) {
        (*backend)->addr = hc->cp->addr;
        (*backend)->pool = ctx->p;
        (*backend)->hostname = hc->s->hostname;
        if (strcmp(hc->s->scheme, "https") == 0) {
            if (!ap_proxy_ssl_enable(NULL)) {
                ap_log_error(APLOG_MARK, APLOG_WARNING, 0, ctx->s, APLOGNO(03252)
                              "mod_ssl not configured?");
                return !OK;
            }
            (*backend)->is_ssl = 1;
        }

    }
    status = hc_determine_connection(ctx, hc);
    if (status == OK) {
        (*backend)->addr = hc->cp->addr;
    }
    return status;
}

static apr_status_t hc_check_tcp(sctx_t *ctx, apr_pool_t *p, proxy_worker *worker)
{
    int status;
    proxy_conn_rec *backend = NULL;
    proxy_worker *hc;

    hc = hc_get_hcworker(ctx, worker, p);

    status = hc_get_backend(p, "HCTCP", &backend, hc, ctx);
    if (status == OK) {
        backend->addr = hc->cp->addr;
        status = ap_proxy_connect_backend("HCTCP", backend, hc, ctx->s);
        if (status == OK) {
            status = (ap_proxy_is_socket_connected(backend->sock) ? OK : !OK);
        }
    }
    return backend_cleanup("HCTCP", backend, ctx->s, status);
}

static void hc_send(sctx_t *ctx, apr_pool_t *p, const char *out, proxy_conn_rec *backend)
{
    apr_bucket_brigade *tmp_bb = apr_brigade_create(p, ctx->ba);
    ap_log_error(APLOG_MARK, APLOG_TRACE7, 0, ctx->s, "%s", out);
    APR_BRIGADE_INSERT_TAIL(tmp_bb, apr_bucket_pool_create(out, strlen(out), p,
                            ctx->ba));
    APR_BRIGADE_INSERT_TAIL(tmp_bb, apr_bucket_flush_create(ctx->ba));
    ap_pass_brigade(backend->connection->output_filters, tmp_bb);
    apr_brigade_destroy(tmp_bb);
}

static int hc_read_headers(sctx_t *ctx, request_rec *r)
{
    char buffer[HUGE_STRING_LEN];
    int len;

    len = ap_getline(buffer, sizeof(buffer), r, 1);
    if (len <= 0) {
        return !OK;
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ctx->s, APLOGNO(03254)
            "%s", buffer);
    /* for the below, see ap_proxy_http_process_response() */
    if (apr_date_checkmask(buffer, "HTTP/#.# ###*")) {
        int major, minor;
        char keepchar;
        int proxy_status = OK;
        const char *proxy_status_line = NULL;

        major = buffer[5] - '0';
        minor = buffer[7] - '0';
        if ((major != 1) || (len >= sizeof(buffer)-1)) {
            return !OK;
        }

        keepchar = buffer[12];
        buffer[12] = '\0';
        proxy_status = atoi(&buffer[9]);
        if (keepchar != '\0') {
            buffer[12] = keepchar;
        } else {
            buffer[12] = ' ';
            buffer[13] = '\0';
        }
        proxy_status_line = apr_pstrdup(r->pool, &buffer[9]);
        r->status = proxy_status;
        r->status_line = proxy_status_line;
    } else {
        return !OK;
    }
    /* OK, 1st line is OK... scarf in the headers */
    while ((len = ap_getline(buffer, sizeof(buffer), r, 1)) > 0) {
        char *value, *end;
        if (!(value = strchr(buffer, ':'))) {
            return !OK;
        }
        ap_log_error(APLOG_MARK, APLOG_TRACE7, 0, ctx->s, "%s", buffer);
        *value = '\0';
        ++value;
        while (apr_isspace(*value))
            ++value;            /* Skip to start of value   */
        for (end = &value[strlen(value)-1]; end > value && apr_isspace(*end); --end)
            *end = '\0';
        apr_table_add(r->headers_out, buffer, value);
    }
    return OK;
}

static int hc_read_body (sctx_t *ctx, request_rec *r)
{
    apr_status_t rv = APR_SUCCESS;
    apr_bucket_brigade *bb;
    int seen_eos = 0;

    bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    do {
        apr_bucket *bucket, *cpy;
        apr_size_t len = HUGE_STRING_LEN;

        rv = ap_get_brigade(r->proto_input_filters, bb, AP_MODE_READBYTES,
                            APR_BLOCK_READ, len);

        if (rv != APR_SUCCESS) {
            if (APR_STATUS_IS_TIMEUP(rv) || APR_STATUS_IS_EOF(rv)) {
                rv = APR_SUCCESS;
                break;
            }
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rv, ctx->s, APLOGNO(03300)
                          "Error reading response body");
            break;
        }

        for (bucket = APR_BRIGADE_FIRST(bb);
             bucket != APR_BRIGADE_SENTINEL(bb);
             bucket = APR_BUCKET_NEXT(bucket))
        {
            if (APR_BUCKET_IS_EOS(bucket)) {
                seen_eos = 1;
                break;
            }
            if (APR_BUCKET_IS_FLUSH(bucket)) {
                continue;
            }
            rv =  apr_bucket_copy(bucket, &cpy);
            if (rv != APR_SUCCESS) {
                break;
            }
            APR_BRIGADE_INSERT_TAIL(r->kept_body, cpy);
        }
        apr_brigade_cleanup(bb);
    }
    while (!seen_eos);
    return (rv == APR_SUCCESS ? OK : !OK);
}

/*
 * Send the HTTP OPTIONS, HEAD or GET request to the backend
 * server associated w/ worker. If we have Conditions,
 * then apply those to the resulting response, otherwise
 * any status code 2xx or 3xx is considered "passing"
 */
static apr_status_t hc_check_http(sctx_t *ctx, apr_pool_t *p, proxy_worker *worker)
{
    int status;
    proxy_conn_rec *backend = NULL;
    proxy_worker *hc;
    conn_rec c;
    request_rec *r;
    wctx_t *wctx;
    hc_condition_t *cond;
    const char *method;

    hc = hc_get_hcworker(ctx, worker, p);
    wctx = (wctx_t *)hc->context;

    if ((status = hc_get_backend(p, "HCOH", &backend, hc, ctx)) != OK) {
        return backend_cleanup("HCOH", backend, ctx->s, status);
    }
    if ((status = ap_proxy_connect_backend("HCOH", backend, hc, ctx->s)) != OK) {
        return backend_cleanup("HCOH", backend, ctx->s, status);
    }

    if (!backend->connection) {
        if ((status = ap_proxy_connection_create("HCOH", backend, &c, ctx->s)) != OK) {
            return backend_cleanup("HCOH", backend, ctx->s, status);
        }
    }
    switch (hc->s->method) {
        case OPTIONS:
            if (!wctx->req) {
                wctx->req = apr_psprintf(ctx->p,
                                   "OPTIONS * HTTP/1.0\r\nHost: %s:%d\r\n\r\n",
                                    hc->s->hostname, (int)hc->s->port);
            }
            method = "OPTIONS";
            break;

        case HEAD:
            if (!wctx->req) {
                wctx->req = apr_psprintf(ctx->p,
                                   "HEAD %s%s%s HTTP/1.0\r\nHost: %s:%d\r\n\r\n",
                                   (wctx->path ? wctx->path : ""),
                                   (wctx->path && *hc->s->hcuri ? "/" : "" ),
                                   (*hc->s->hcuri ? hc->s->hcuri : ""),
                                   hc->s->hostname, (int)hc->s->port);
            }
            method = "HEAD";
            break;

        case GET:
            if (!wctx->req) {
                wctx->req = apr_psprintf(ctx->p,
                                   "GET %s%s%s HTTP/1.0\r\nHost: %s:%d\r\n\r\n",
                                   (wctx->path ? wctx->path : ""),
                                   (wctx->path && *hc->s->hcuri ? "/" : "" ),
                                   (*hc->s->hcuri ? hc->s->hcuri : ""),
                                   hc->s->hostname, (int)hc->s->port);
            }
            method = "GET";
            break;

        default:
            return backend_cleanup("HCOH", backend, ctx->s, !OK);
            break;
    }

    hc_send(ctx, p, wctx->req, backend);

    r = create_request_rec(p, backend->connection, method);
    if ((status = hc_read_headers(ctx, r)) != OK) {
        return backend_cleanup("HCOH", backend, ctx->s, status);
    }
    if (hc->s->method == GET) {
        if ((status = hc_read_body(ctx, r)) != OK) {
            return backend_cleanup("HCOH", backend, ctx->s, status);
        }
    }

    if (*worker->s->hcexpr &&
            (cond = (hc_condition_t *)apr_table_get(ctx->conditions, worker->s->hcexpr)) != NULL) {
        const char *err;
        int ok = ap_expr_exec(r, cond->pexpr, &err);
        if (ok > 0) {
            status = OK;
            ap_log_error(APLOG_MARK, APLOG_TRACE2, 0, ctx->s,
                         "Success checking condition %s", worker->s->hcexpr);
        } else if (ok < 0 || err) {
            status = !OK;
            ap_log_error(APLOG_MARK, APLOG_INFO, 0, ctx->s, APLOGNO(03301)
                         "Error on checking condition %s: %s", worker->s->hcexpr,
                         err);
        } else {
            ap_log_error(APLOG_MARK, APLOG_TRACE2, 0, ctx->s,
                         "Failure checking condition %s", worker->s->hcexpr);
            status = !OK;
        }
    } else if (r->status < 200 || r->status > 399) {
        status = !OK;
    }
    return backend_cleanup("HCOH", backend, ctx->s, status);
}


static void hc_check(sctx_t *ctx, apr_pool_t *p, apr_time_t now,
                     proxy_worker *worker)
{
    server_rec *s = ctx->s;
    apr_status_t rv;
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, APLOGNO(03256)
                 "Health checking %s", worker->s->name);

    switch (worker->s->method) {
        case TCP:
            rv = hc_check_tcp(ctx, p, worker);
            break;

        case OPTIONS:
        case HEAD:
        case GET:
             rv = hc_check_http(ctx, p, worker);
             break;

        default:
            rv = APR_ENOTIMPL;
            break;
    }
    if (rv == APR_ENOTIMPL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, APLOGNO(03257)
                         "Somehow tried to use unimplemented hcheck method: %d", (int)worker->s->method);
        return;
    }
    /* what state are we in ? */
    if (PROXY_WORKER_IS_HCFAILED(worker)) {
        if (rv == APR_SUCCESS) {
            worker->s->pcount += 1;
            if (worker->s->pcount >= worker->s->passes) {
                ap_proxy_set_wstatus(PROXY_WORKER_HC_FAIL_FLAG, 0, worker);
                worker->s->pcount = 0;
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, s, APLOGNO(03302)
                             "Health check ENABLING %s", worker->s->name);

            }
        }
    } else {
        if (rv != APR_SUCCESS) {
            worker->s->error_time = now;
            worker->s->fcount += 1;
            if (worker->s->fcount >= worker->s->fails) {
                ap_proxy_set_wstatus(PROXY_WORKER_HC_FAIL_FLAG, 1, worker);
                worker->s->fcount = 0;
                ap_log_error(APLOG_MARK, APLOG_INFO, 0, s, APLOGNO(03303)
                             "Health check DISABLING %s", worker->s->name);
            }
        }
    }
    worker->s->updated = now;
}

static apr_status_t hc_watchdog_callback(int state, void *data,
                                         apr_pool_t *pool)
{
    apr_status_t rv = APR_SUCCESS;
    apr_time_t now = apr_time_now();
    proxy_balancer *balancer;
    sctx_t *ctx = (sctx_t *)data;
    server_rec *s = ctx->s;
    proxy_server_conf *conf;
    apr_pool_t *p;
    switch (state) {
        case AP_WATCHDOG_STATE_STARTING:
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, APLOGNO(03258)
                         "%s watchdog started.",
                         HCHECK_WATHCHDOG_NAME);
            break;

        case AP_WATCHDOG_STATE_RUNNING:
            /* loop thru all workers */
            ap_log_error(APLOG_MARK, APLOG_TRACE2, 0, s,
                         "Run of %s watchdog.",
                         HCHECK_WATHCHDOG_NAME);
            if (s) {
                int i;
                apr_pool_create(&p, pool);
                conf = (proxy_server_conf *) ap_get_module_config(s->module_config, &proxy_module);
                balancer = (proxy_balancer *)conf->balancers->elts;
                for (i = 0; i < conf->balancers->nelts; i++, balancer++) {
                    int n;
                    proxy_worker **workers;
                    proxy_worker *worker;
                    /* Have any new balancers or workers been added dynamically? */
                    ap_proxy_sync_balancer(balancer, s, conf);
                    workers = (proxy_worker **)balancer->workers->elts;
                    for (n = 0; n < balancer->workers->nelts; n++) {
                        worker = *workers;
                        ap_log_error(APLOG_MARK, APLOG_TRACE2, 0, s,
                                     "Checking %s worker: %s  [%d] (%pp)", balancer->s->name,
                                     worker->s->name, worker->s->method, worker);
                        if ((worker->s->method != NONE) && (now > worker->s->updated + worker->s->interval)) {
                            if ((rv = hc_init_worker(ctx, worker)) != APR_SUCCESS) {
                                return rv;
                            }
                            hc_check(ctx, p, now, worker);
                        }
                        workers++;
                    }
                }
                apr_pool_destroy(p);
                /* s = s->next; */
            }
            break;

        case AP_WATCHDOG_STATE_STOPPING:
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, APLOGNO(03261)
                         "stopping %s watchdog.",
                         HCHECK_WATHCHDOG_NAME);
            break;
    }
    return rv;
}

static int hc_post_config(apr_pool_t *p, apr_pool_t *plog,
                       apr_pool_t *ptemp, server_rec *s)
{
    apr_status_t rv;
    sctx_t *ctx;

    APR_OPTIONAL_FN_TYPE(ap_watchdog_get_instance) *hc_watchdog_get_instance;
    APR_OPTIONAL_FN_TYPE(ap_watchdog_register_callback) *hc_watchdog_register_callback;

    hc_watchdog_get_instance = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_get_instance);
    hc_watchdog_register_callback = APR_RETRIEVE_OPTIONAL_FN(ap_watchdog_register_callback);
    if (!hc_watchdog_get_instance || !hc_watchdog_register_callback) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, APLOGNO(03262)
                     "mod_watchdog is required");
        return !OK;
    }
    ctx = (sctx_t *) ap_get_module_config(s->module_config,
                                          &proxy_hcheck_module);

    rv = hc_watchdog_get_instance(&ctx->watchdog,
                                  HCHECK_WATHCHDOG_NAME,
                                  0, 1, p);
    if (rv) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s, APLOGNO(03263)
                     "Failed to create watchdog instance (%s)",
                     HCHECK_WATHCHDOG_NAME);
        return !OK;
    }
    rv = hc_watchdog_register_callback(ctx->watchdog,
            apr_time_from_sec(HCHECK_WATHCHDOG_INTERVAL),
            ctx,
            hc_watchdog_callback);
    if (rv) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, rv, s, APLOGNO(03264)
                     "Failed to register watchdog callback (%s)",
                     HCHECK_WATHCHDOG_NAME);
        return !OK;
    }
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s, APLOGNO(03265)
                 "watchdog callback registered (%s)", HCHECK_WATHCHDOG_NAME);
    return OK;
}

static void hc_show_exprs(request_rec *r)
{
    const apr_table_entry_t *elts;
    const apr_array_header_t *hdr;
    int i;
    sctx_t *ctx = (sctx_t *) ap_get_module_config(r->server->module_config,
                                                  &proxy_hcheck_module);
    if (apr_is_empty_table(ctx->conditions))
        return;

    ap_rputs("\n\n<table>"
             "<tr><th colspan=\"2\">Health check cond. expressions:</th></tr>\n"
             "<tr><th>Expr name</th><th>Expression</th></tr>\n", r);

    hdr = apr_table_elts(ctx->conditions);
    elts = (const apr_table_entry_t *) hdr->elts;
    for (i = 0; i < hdr->nelts; ++i) {
        hc_condition_t *cond;
        if (!elts[i].key) {
            continue;
        }
        cond = (hc_condition_t *)elts[i].val;
        ap_rprintf(r, "<tr><td>%s</td><td>%s</td></tr>\n", elts[i].key,
                   cond->expr);
    }
    ap_rputs("</table><hr/>\n", r);
}

static const char *hc_get_body(request_rec *r)
{
    apr_off_t length;
    apr_size_t len;
    apr_status_t rv;
    char *buf;

    if (!r || !r->kept_body)
        return "";

    rv = apr_brigade_length(r->kept_body, 1, &length);
    len = (apr_size_t)length;
    if (rv != APR_SUCCESS || len == 0)
        return "";

    buf = apr_palloc(r->pool, len + 1);
    rv = apr_brigade_flatten(r->kept_body, buf, &len);
    if (rv != APR_SUCCESS)
        return "";
    buf[len] = '\0'; /* ensure */
    return (const char*)buf;
}

static const char *hc_expr_var_fn(ap_expr_eval_ctx_t *ctx, const void *data)
{
    char *var = (char *)data;

    if (var && *var && ctx->r && ap_casecmpstr(var, "BODY") == 0) {
        return hc_get_body(ctx->r);
    }
    return NULL;
}

static const char *hc_expr_func_fn(ap_expr_eval_ctx_t *ctx, const void *data,
                                const char *arg)
{
    char *var = (char *)arg;

    if (var && *var && ctx->r && ap_casecmpstr(var, "BODY") == 0) {
        return hc_get_body(ctx->r);
    }
    return NULL;
}

static int hc_expr_lookup(ap_expr_lookup_parms *parms)
{
    switch (parms->type) {
    case AP_EXPR_FUNC_VAR:
        /* for now, we just handle everything that starts with HC_.
         */
        if (strncasecmp(parms->name, "HC_", 3) == 0) {
            *parms->func = hc_expr_var_fn;
            *parms->data = parms->name + 3;
            return OK;
        }
        break;
    case AP_EXPR_FUNC_STRING:
        /* Function HC() is implemented by us.
         */
        if (strcasecmp(parms->name, "HC") == 0) {
            *parms->func = hc_expr_func_fn;
<<<<<<< ed5428fb47013d766021b6e21a354a1f66502635
            *parms->data = parms->name;
=======
            *parms->data = parms->arg;
>>>>>>> Adjust to pass arg
            return OK;
        }
        break;
    }
    return DECLINED;
}

static const command_rec command_table[] = {
    AP_INIT_RAW_ARGS("ProxyHCTemplate", set_hc_template, NULL, OR_FILEINFO,
                     "Health check template"),
    AP_INIT_RAW_ARGS("ProxyHCExpr", set_hc_condition, NULL, OR_FILEINFO,
                     "Define a health check condition ruleset expression"),
    { NULL }
};

static void hc_register_hooks(apr_pool_t *p)
{
    static const char *const aszPre[] = { "mod_proxy_balancer.c", "mod_proxy.c", NULL};
    static const char *const aszSucc[] = { "mod_watchdog.c", NULL};
    APR_REGISTER_OPTIONAL_FN(set_worker_hc_param);
    APR_REGISTER_OPTIONAL_FN(hc_show_exprs);
    ap_hook_post_config(hc_post_config, aszPre, aszSucc, APR_HOOK_LAST);
    ap_hook_expr_lookup(hc_expr_lookup, NULL, NULL, APR_HOOK_MIDDLE);
}

/* the main config structure */

AP_DECLARE_MODULE(proxy_hcheck) =
{
    STANDARD20_MODULE_STUFF,
    NULL,              /* create per-dir config structures */
    NULL,              /* merge  per-dir config structures */
    hc_create_config,  /* create per-server config structures */
    NULL,              /* merge  per-server config structures */
    command_table,     /* table of config file commands */
    hc_register_hooks  /* register hooks */
};
