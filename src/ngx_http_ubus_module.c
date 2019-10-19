
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <ubus_utility.h>

static void* ngx_http_ubus_create_loc_conf(ngx_conf_t *cf);

static char* ngx_http_ubus_merge_loc_conf(ngx_conf_t *cf,
		void *parent, void *child);

static char *ngx_http_ubus(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_command_t  ngx_http_ubus_commands[] = {
		{ ngx_string("ubus_interpreter"),
			NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
			ngx_http_ubus,
			NGX_HTTP_LOC_CONF_OFFSET,
			0,
			NULL },

		{ ngx_string("ubus_socket_path"),
			NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
			ngx_conf_set_str_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, socket_path),
			NULL },

		{ ngx_string("ubus_cors"),
			NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
			ngx_conf_set_flag_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, cors),
			NULL },

		{ ngx_string("ubus_script_timeout"),
			NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
			ngx_conf_set_num_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, script_timeout),
			NULL },

		{ ngx_string("ubus_noauth"),
			NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
			ngx_conf_set_flag_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, noauth),
			NULL },

			ngx_null_command
};

static ngx_http_module_t  ngx_http_ubus_module_ctx = {
		NULL,   /* preconfiguration */
		NULL,  /* postconfiguration */

		NULL,                          /* create main configuration */
		NULL,                          /* init main configuration */

		NULL,                          /* create server configuration */
		NULL,                          /* merge server configuration */

		ngx_http_ubus_create_loc_conf,  /* create location configuration */
		ngx_http_ubus_merge_loc_conf /* merge location configuration */
};


ngx_module_t  ngx_http_ubus_module = {
		NGX_MODULE_V1,
		&ngx_http_ubus_module_ctx, /* module context */
		ngx_http_ubus_commands,   /* module directives */
		NGX_HTTP_MODULE,               /* module type */
		NULL,                          /* init master */
		NULL,                          /* init module */
		NULL,                          /* init process */
		NULL,                          /* init thread */
		NULL,                          /* exit thread */
		NULL,                          /* exit process */
		NULL,                          /* exit master */
		NGX_MODULE_V1_PADDING
};

struct cors_data {
	char* ORIGIN;
	char* ACCESS_CONTROL_REQUEST_METHOD;
	char* ACCESS_CONTROL_REQUEST_HEADERS;
};

static void ubus_single_error(ngx_http_request_t *r, enum rpc_error type);
static ngx_int_t ngx_http_ubus_send_body(ngx_http_request_t *r, ngx_http_ubus_loc_conf_t  *cglcf);
static ngx_int_t append_to_output_chain(ngx_http_request_t *r, ngx_http_ubus_loc_conf_t *cglcf, const char* str);

static ngx_int_t set_custom_headers_out(ngx_http_request_t *r, const char *key_str, const char *value_str) {
	ngx_table_elt_t   *h;
	ngx_str_t key;
	ngx_str_t value;

	char * tmp;
	int len;

	len = strlen(key_str);
	tmp = ngx_palloc(r->pool,len + 1);
	ngx_memcpy(tmp,key_str,len);

	key.data = tmp;
	key.len = len;

	len = strlen(value_str);
	tmp = ngx_palloc(r->pool,len + 1);
	ngx_memcpy(tmp,value_str,len);

	value.data = tmp;
	value.len = len;

	h = ngx_list_push(&r->headers_out.headers);
	if (h == NULL) {
		return NGX_ERROR;
	}

	h->key = key;
	h->value = value;
	h->hash = 1;

	return NGX_OK;
}

static void parse_cors_from_header(ngx_http_request_t *r, struct cors_data *cors) {
	ngx_list_part_t            *part;
	ngx_table_elt_t            *h;
	ngx_uint_t                  i;

	ngx_uint_t found_count = 0;

	part = &r->headers_in.headers.part;
	h = part->elts;

	for (i = 0; /* void */ ; i++) {
		if ( found_count == 3 )
			break;

		if (i >= part->nelts) {
			if (part->next == NULL) {
				break;
			}

			part = part->next;
			h = part->elts;
			i = 0;
		}

		if (ngx_strcmp("origin", h[i].key.data)) {
			cors->ORIGIN = h[i].key.data;
			found_count++;
		}
		else if (ngx_strcmp("access-control-request-method", h[i].key.data)) {
			cors->ACCESS_CONTROL_REQUEST_METHOD = h[i].key.data;
			found_count++;
		}
		else if (ngx_strcmp("access-control-request-headers", h[i].key.data)) {
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ok3");
			cors->ACCESS_CONTROL_REQUEST_HEADERS = h[i].key.data;
			found_count++;
		}

	}
}

static void ubus_add_cors_headers(ngx_http_request_t *r)
{
	struct cors_data *cors;

	cors = ngx_pcalloc(r->pool,sizeof(struct cors_data));
	parse_cors_from_header(r,cors);

	char* req;

	if (!cors->ORIGIN)
		return;

	if (cors->ACCESS_CONTROL_REQUEST_METHOD)
	{
		char *req = cors->ACCESS_CONTROL_REQUEST_METHOD;
		if (strcmp(req, "POST") && strcmp(req, "OPTIONS"))
			return;
	}

	set_custom_headers_out(r,"Access-Control-Allow-Origin",cors->ORIGIN);

	if (cors->ACCESS_CONTROL_REQUEST_HEADERS)
		set_custom_headers_out(r,"Access-Control-Allow-Headers",cors->ACCESS_CONTROL_REQUEST_HEADERS);

	set_custom_headers_out(r,"Access-Control-Allow-Methods","POST, OPTIONS");
	set_custom_headers_out(r,"Access-Control-Allow-Credentials","true");

	ngx_pfree(r->pool,cors);
}

static ngx_int_t ngx_http_ubus_send_header(
	ngx_http_request_t *r, ngx_http_ubus_loc_conf_t  *cglcf, ngx_int_t status, ngx_int_t post_len)
{
	r->headers_out.status = status;
	r->headers_out.content_type.len = sizeof("application/json") - 1;
	r->headers_out.content_type.data = (u_char *) "application/json";
	r->headers_out.content_length_n = post_len;

	if (cglcf->cors)
		ubus_add_cors_headers(r);

	return ngx_http_send_header(r);
	
}

static void ubus_single_error(ngx_http_request_t *r, enum rpc_error type)
{
	void *c;
	char *str;
	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);

	struct dispatch_ubus *du = cglcf->ubus;

	ubus_init_response(cglcf->buf,du);

	c = blobmsg_open_table(cglcf->buf, "error");
	blobmsg_add_u32(cglcf->buf, "code", json_errors[type].code);
	blobmsg_add_string(cglcf->buf, "message", json_errors[type].msg);
	blobmsg_close_table(cglcf->buf, c);

	str = blobmsg_format_json(cglcf->buf->head, true);
	append_to_output_chain(r,cglcf,str);

	ngx_http_ubus_send_header(r,cglcf,NGX_HTTP_OK,strlen(str));
	ngx_http_ubus_send_body(r,cglcf);
}

static ngx_int_t append_to_output_chain(ngx_http_request_t *r, ngx_http_ubus_loc_conf_t *cglcf, const char* str)
{
	ngx_int_t len = strlen(str);

	char* data = ngx_pcalloc(r->pool, len + 1);
	ngx_memcpy(data,str,len);

	ngx_buf_t *b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
	b->pos = data;
	b->last = data + len;
	b->memory = 1;
	cglcf->res_len += len;

	if (!cglcf->out_chain) {
			cglcf->out_chain = (ngx_chain_t *) ngx_palloc(r->pool, sizeof(ngx_chain_t*));
			cglcf->out_chain->buf = b;
			cglcf->out_chain->next = NULL;
			cglcf->out_chain_start = cglcf->out_chain;
	} else {
			ngx_chain_t* out_aux = (ngx_chain_t *) ngx_palloc(r->pool, sizeof(ngx_chain_t*));
			out_aux->buf = b;
			out_aux->next = NULL;
			cglcf->out_chain->next = out_aux;
			cglcf->out_chain = out_aux;
	}
}

static ngx_int_t ngx_http_ubus_send_body(ngx_http_request_t *r, ngx_http_ubus_loc_conf_t  *cglcf)
{
	cglcf->out_chain->buf->last_buf = 1;
	cglcf->ubus->jsobj = NULL;
	cglcf->ubus->jstok = json_tokener_new();

	return ngx_http_output_filter(r, cglcf->out_chain_start);
}

static bool ubus_allowed(ngx_http_ubus_loc_conf_t  *cglcf, const char *sid, const char *obj, const char *fun)
{
	uint32_t id;
	bool allow = false;
	static struct blob_buf req;

	if (ubus_lookup_id(cglcf->ctx, "session", &id))
		return false;

	blob_buf_init(&req, 0);
	blobmsg_add_string(&req, "ubus_rpc_session", sid);
	blobmsg_add_string(&req, "object", obj);
	blobmsg_add_string(&req, "function", fun);
	ubus_invoke(cglcf->ctx, id, "access", req.head, ubus_allowed_cb, &allow, cglcf->script_timeout * 500);

	return allow;
}

static ngx_int_t ubus_send_request(ngx_http_request_t *r, json_object *obj, const char *sid, struct blob_attr *args)
{
	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);

	struct dispatch_ubus *du = cglcf->ubus;
	struct blob_attr *cur;
	static struct blob_buf req;
	int ret, rem;

	char *str;

	blob_buf_init(&req, 0);

	ubus_init_response(cglcf->buf,du);

	blobmsg_for_each_attr(cur, args, rem) {
		if (!strcmp(blobmsg_name(cur), "ubus_rpc_session")) {
			ubus_single_error(r, ERROR_PARAMS);
			return NGX_ERROR;
		}
		blobmsg_add_blob(&req, cur);
	}

	blobmsg_add_string(&req, "ubus_rpc_session", sid);

	blob_buf_init(&du->buf, 0);
	memset(&du->req, 0, sizeof(du->req));

	ubus_invoke(cglcf->ctx, du->obj, du->func, req.head, ubus_request_cb, cglcf, cglcf->script_timeout * 1000);

	str = blobmsg_format_json(cglcf->buf->head, true);
	append_to_output_chain(r,cglcf,str);
	free(str);

	return NGX_OK;
}

static ngx_int_t ubus_send_list(ngx_http_request_t *request, json_object *obj, struct blob_attr *params)
{
	struct blob_attr *cur, *dup;

	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(request, ngx_http_ubus_module);

	struct dispatch_ubus *du = cglcf->ubus;

	struct list_data data = { .buf = &du->buf, .verbose = false };
	void *r;
	int rem;

	char *str;

	blob_buf_init(data.buf, 0);

	ubus_init_response(cglcf->buf,du);

	if (!params || blob_id(params) != BLOBMSG_TYPE_ARRAY) {
		r = blobmsg_open_array(data.buf, "result");
		ubus_lookup(cglcf->ctx, NULL, ubus_list_cb, &data);
		blobmsg_close_array(data.buf, r);
	}
	else {
		r = blobmsg_open_table(data.buf, "result");
		dup = blob_memdup(params);
		if (dup)
		{
			rem = blobmsg_data_len(dup);
			data.verbose = true;
			__blob_for_each_attr(cur, blobmsg_data(dup), rem)
				ubus_lookup(cglcf->ctx, blobmsg_data(cur), ubus_list_cb, &data);
			free(dup);
		}
		blobmsg_close_table(data.buf, r);
	}

	blobmsg_add_blob(cglcf->buf, blob_data(data.buf->head));

	str = blobmsg_format_json(cglcf->buf->head, true);
	append_to_output_chain(request,cglcf,str);
	free(str);

	return NGX_OK;
}

static ngx_int_t ubus_process_object(ngx_http_request_t *r, struct json_object *obj)
{
	ngx_int_t rc = NGX_OK;
	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);

	struct dispatch_ubus *du = cglcf->ubus;
	struct rpc_data data = {};
	enum rpc_error err = ERROR_PARSE;

	if (json_object_get_type(obj) != json_type_object)
		goto error;

	du->jsobj_cur = obj;
	blob_buf_init(cglcf->buf, 0);
	if (!blobmsg_add_object(cglcf->buf, obj))
		goto error;

	if (!parse_json_rpc(&data, cglcf->buf->head))
		goto error;

	if (!strcmp(data.method, "call")) {
		if (!data.sid || !data.object || !data.function || !data.data)
			goto error;

		du->func = data.function;
		if (ubus_lookup_id(cglcf->ctx, data.object, &du->obj)) {
			err = ERROR_OBJECT;
			goto error;
		}

		if (!cglcf->noauth && !ubus_allowed(cglcf, data.sid, data.object, data.function)) {
			err = ERROR_ACCESS;
			goto error;
		}

		rc = ubus_send_request(r, obj, data.sid, data.data);
		goto out;
	}
	else if (!strcmp(data.method, "list")) {
		rc = ubus_send_list(r, obj, data.params);
		goto out;
	}
	else {
		err = ERROR_METHOD;
		goto error;
	}

error:
	ubus_single_error(r, err);
	rc = NGX_ERROR;
out:
	if (data.params)
		free(data.params);

	return rc;
}

static ngx_int_t ubus_process_array(ngx_http_request_t *r, json_object *obj)
{
	ngx_http_ubus_loc_conf_t  *cglcf;
	ngx_int_t rc;
	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);

	int len = json_object_array_length(obj);
	int index;

	for (index = 0 ; index < len ; index++ ) {
		struct json_object *obj_tmp = json_object_array_get_idx(obj, index );

		if ( index > 0 )
			append_to_output_chain(r,cglcf,",");
		
		rc = ubus_process_object(r, obj_tmp);

		free(obj_tmp);

		if ( rc != NGX_OK )
			return rc;
	}
			
	append_to_output_chain(r,cglcf,"]");
	return NGX_OK;
}

static ngx_int_t ngx_http_ubus_elaborate_req(ngx_http_request_t *r)
{
	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);
	struct dispatch_ubus *du = cglcf->ubus;
	struct json_object *obj = du->jsobj;

	switch (obj ? json_object_get_type(obj) : json_type_null) {
		case json_type_object:
			return ubus_process_object(r, obj);
		case json_type_array:
			append_to_output_chain(r,cglcf,"[");
			return ubus_process_array(r, obj);
		default:
			ubus_single_error(r, ERROR_PARSE);
			return NGX_ERROR;
	}
}

static void ngx_http_ubus_req_handler(ngx_http_request_t *r)
{
	ngx_int_t     rc;
	off_t pos = 0;
	off_t len;
	ngx_chain_t  *in;
	ngx_http_ubus_loc_conf_t  *cglcf;
	struct dispatch_ubus *du;
	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);
	char *buffer = ngx_pcalloc(r->pool, r->headers_in.content_length_n + 1);
	
	cglcf->ubus = ngx_pcalloc(r->pool,sizeof(struct dispatch_ubus));
	cglcf->buf = ngx_pcalloc(r->pool,sizeof(struct blob_buf));

	cglcf->ubus->jsobj = NULL;
	cglcf->ubus->jstok = json_tokener_new();

	blob_buf_init(cglcf->buf, 0);

	if (cglcf->ubus->jsobj || !cglcf->ubus->jstok) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Error ubus struct not ok");
		ubus_single_error(r, ERROR_PARSE);
		ngx_http_finalize_request(r, NGX_HTTP_OK);
		return NGX_HTTP_OK;
	}

	du = cglcf->ubus;

	for (in = r->request_body->bufs; in; in = in->next) {

		len = ngx_buf_size(in->buf);
		ngx_memcpy(buffer + pos,in->buf->pos,len);
		pos += len;

		if (pos > UBUS_MAX_POST_SIZE) {
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Error max post size for ubus socket");
			ubus_single_error(r, ERROR_PARSE);
			ngx_pfree(r->pool,buffer);
			ngx_http_finalize_request(r, NGX_HTTP_OK);
			return;
		}
	}

	if ( pos != r->headers_in.content_length_n ) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Readed buffer differ from header request len");
		ubus_single_error(r, ERROR_PARSE);
		ngx_pfree(r->pool,buffer);
		ngx_http_finalize_request(r, NGX_HTTP_OK);
		return;
	}

	du->jsobj = json_tokener_parse_ex(du->jstok, buffer, pos);
	ngx_pfree(r->pool,buffer);

	rc = ngx_http_ubus_elaborate_req(r);
	if (rc == NGX_ERROR) {
		// With ngx_error we are sending json error 
		// and we say that the request is ok
		ngx_http_finalize_request(r, NGX_HTTP_OK);
		return;
	}

	rc = ngx_http_ubus_send_header(r,cglcf,NGX_HTTP_OK,cglcf->res_len);
	if (rc == NGX_ERROR || rc > NGX_OK) {
		ngx_http_finalize_request(r, rc);
		return;
	}

	ngx_pfree(r->pool,cglcf->ubus);
	ngx_pfree(r->pool,cglcf->buf);
	rc = ngx_http_ubus_send_body(r,cglcf);

	ngx_http_finalize_request(r, rc);
}



static ngx_int_t
ngx_http_ubus_handler(ngx_http_request_t *r)
{
	ngx_int_t     rc;

	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);
	
	switch (r->method)
	{
		case NGX_HTTP_OPTIONS:
			r->header_only = 1;
			ngx_http_ubus_send_header(r,cglcf,NGX_HTTP_OK,0);
			ngx_http_finalize_request(r,NGX_HTTP_OK);
			return NGX_DONE;

		case NGX_HTTP_POST:

			cglcf->out_chain = NULL;
			cglcf->res_len = 0;

			rc = ngx_http_read_client_request_body(r, ngx_http_ubus_req_handler);
			if (rc >= NGX_HTTP_SPECIAL_RESPONSE)
				return rc;

			return NGX_DONE;

		default:
			return NGX_HTTP_BAD_REQUEST;
	}
}

static char *
ngx_http_ubus(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
		ngx_http_core_loc_conf_t  *clcf;
		ngx_http_ubus_loc_conf_t *cglcf = conf;

		clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
		clcf->handler = ngx_http_ubus_handler;

		cglcf->enable = 1;

		return NGX_CONF_OK;
}

static void *
ngx_http_ubus_create_loc_conf(ngx_conf_t *cf)
{
		ngx_http_ubus_loc_conf_t  *conf;

		conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_ubus_loc_conf_t));
		if (conf == NULL) {
				return NGX_CONF_ERROR;
		}

		conf->socket_path.data = NULL;
		conf->socket_path.len = -1;

		conf->cors = NGX_CONF_UNSET;
		conf->noauth = NGX_CONF_UNSET;
		conf->script_timeout = NGX_CONF_UNSET_UINT;
		conf->enable = NGX_CONF_UNSET;
		return conf;
}

static char *
ngx_http_ubus_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
		ngx_http_ubus_loc_conf_t *prev = parent;
		ngx_http_ubus_loc_conf_t *conf = child;

		// Skip merge of other, if we don't have a socket to connect...
		// We don't init the module at all.
		if (conf->socket_path.data == NULL)
				return NGX_CONF_OK;

		ngx_conf_merge_value(conf->cors, prev->cors, 0);
		ngx_conf_merge_value(conf->noauth, prev->noauth, 0);
		ngx_conf_merge_uint_value(conf->script_timeout, prev->script_timeout, 60);
		ngx_conf_merge_value(conf->enable, prev->enable, 0);

		if (conf->script_timeout == 0 ) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "ubus script timeout must be greater than zero"); 
				return NGX_CONF_ERROR;
		}

		if (conf->enable) {
			conf->ctx = ubus_connect(conf->socket_path.data);
			if (!conf->ctx) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Unable to connect to ubus socket: %s", conf->socket_path.data);
				return NGX_CONF_ERROR;
			}
		}

		return NGX_CONF_OK;
}