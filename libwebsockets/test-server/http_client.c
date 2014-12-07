#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>

#include <string.h>

#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <poll.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>

#ifdef CMAKE_BUILD
#include "lws_config.h"
#endif

#include "../lib/libwebsockets.h"

#include "http_client.h"
#include "cJSON.h"

//First, This file only process http packets, but now
// It process http/tunnel/tcp forward streams.
// TODO This file will be rewriten better in the feature

// declare funcs TODO use static functions
void http_context_release(http_mgmt* mgmt, http_context* ctx);
int http_context_connect(http_mgmt* mgmt, http_context* ctx);
int http_context_write(http_mgmt* mgmt, http_context* ctx);
int http_context_read(http_mgmt* mgmt, http_context* ctx);
static http_buf_info* next_buf_info(struct list_head* list);
http_buf_info* alloc_buf(http_mgmt* mgmt);
int http_mgmt_toserver(http_mgmt* mgmt);
static void release_prepare(http_mgmt* mgmt);
int tcp_server_init(http_mgmt* mgmt, tcp_server_context* tcp_server);
int tcp_server_run(http_mgmt* mgmt);
int tcp_client_release(http_mgmt* mgmt, tcp_client_context* tcp_client);
int tcp_client_write(http_mgmt* mgmt, tcp_client_context* tcp_client);
int tcp_client_read(http_mgmt* mgmt, tcp_client_context* tcp_client);
int process_tunnel_req(http_mgmt* mgmt);
int process_tunnel_resp(http_mgmt* mgmt);
int process_tcp_forward(http_mgmt* mgmt);
int tcp_forward_create(http_mgmt* mgmt, uint16_t seq, char* host, uint16_t port);
int tcp_forward_release(http_mgmt* mgmt, tcp_forward_context* tcp_forward);
int tcp_forward_read(http_mgmt* mgmt, tcp_forward_context* tcp_forward);
int tcp_forward_write(http_mgmt* mgmt, tcp_forward_context* tcp_forward);
int process_tcp_forwardresp(http_mgmt *mgmt);
int tcp_forward_closing(http_mgmt* mgmt, tcp_forward_context* tcp_forward);

extern in_addr_t inet_addr(const char *cp);

// Utils functions. move to the other file?
static uint32_t name_resolve(char *host_name)
{
    struct in_addr addr;
    struct hostent *host_ent;

    if((addr.s_addr = inet_addr(host_name)) == (unsigned)-1) {
        host_ent = gethostbyname(host_name);
        if(NULL == host_ent) {
            return (-1);
        }

        memcpy((char *)&addr.s_addr, host_ent->h_addr, host_ent->h_length);
    }
    return (addr.s_addr);
}

int http_mgmt_param_init(http_mgmt* mgmt)
{
    assert(NULL != mgmt);

    mgmt->buf_prepare.curr = NULL;
    mgmt->buf_prepare.len = 0;
    mgmt->buf_prepare.total_len = 0;

    mgmt->buf_toserver.curr = NULL;
    mgmt->buf_toserver.len = 0;
    mgmt->buf_toserver.total_len = 0;

    mgmt->sync_prepare = 0;         // Wait for sync
    mgmt->toserver = 0;
    mgmt->shakehand = 0;
    mgmt->total_process = 0;
    mgmt->client_id = 0;

    time(&mgmt->last_alive_time);

    return 0;
}

int http_mgmt_init(http_mgmt* mgmt)
{
    int i;
    long ptmp;
    http_context* ctx;
    http_buf_info* buf_info;

    assert(NULL != mgmt);

    lwsl_info("mgmt init\n");

    memset(mgmt, 0, sizeof(http_mgmt));

    for(i = 0; i < CALLING_PRIO_COUNT; i++)
    {
        INIT_LIST_HEAD(&mgmt->list_ready[i]);
    }
    INIT_LIST_HEAD(&mgmt->list_timeout);

    mgmt->max_fds = getdtablesize();
    mgmt->pollfds = (struct pollfd*)malloc(mgmt->max_fds * sizeof(struct pollfd));
    mgmt->fd_lookup = (int*)malloc(mgmt->max_fds * sizeof(int));
    mgmt->http_lookup = (common_context**)calloc(mgmt->max_fds, sizeof(common_context*));
    if(NULL == mgmt->pollfds || NULL == mgmt->fd_lookup)
    {
        lwsl_err("mgmt init error, Out of memory\n");
        return -1;
    }

    ctx = (http_context*)calloc(CALLER_CACHES, sizeof(http_context));
    if(NULL == ctx) {
        lwsl_err("mgmt init error, Out of memory\n");
        return -1; // malloc error
    }
    INIT_LIST_HEAD(&mgmt->ctx_caches);
    for(i = 0; i < CALLER_CACHES; i++) {
        list_add(&ctx[i].node, &mgmt->ctx_caches);
    }

    buf_info = (http_buf_info*)calloc(HTTPBUF_CACHES, sizeof(http_buf_info));
    if(NULL == buf_info)
    {
        lwsl_err("mgmt init error, Out of memory\n");
        return -1;
    }
    INIT_LIST_HEAD(&mgmt->http_buf_caches);
    for(i = 0; i < HTTPBUF_CACHES; i++)
    {
        ptmp = (long)(buf_info[i].real_buf + LWS_SEND_BUFFER_PRE_PADDING);
        if(ptmp & 0x7) {
            ptmp = ((ptmp+7)/8) * 8;
        }
        buf_info[i].buf = (char*)ptmp;
        list_add(&buf_info[i].node, &mgmt->http_buf_caches);
    }

    //prepare
    INIT_LIST_HEAD(&mgmt->buf_prepare.list_todo);

    //buf_toserver
    INIT_LIST_HEAD(&mgmt->buf_toserver.list_todo);

    if(0 != tcp_server_init(mgmt, &mgmt->tcp_server)) {
        return -1;
    }

    //tcp forwards
    INIT_LIST_HEAD(&mgmt->list_forward);

    //return http_mgmt_param_init(mgmt);
    return 0;
}

static void mgmt_show_statistics(http_mgmt* mgmt)
{
    int i = 0, cnt;
    http_context* ctx;
    http_buf_info* buf_info;

    fprintf(stderr, "The ready_len=%d\n", mgmt->ready_len);

    for(i = 0; i < CALLING_PRIO_COUNT; i++)
    {
        cnt = 0;
        list_for_each_entry(ctx, &mgmt->list_ready[i], node) {
            cnt++;
        }
        fprintf(stderr, "The count of ready[%d]=%d\n", i, cnt);
    }

    cnt = 0;
    list_for_each_entry(ctx, &mgmt->list_timeout, node_time) {
        cnt++;
    }
    fprintf(stderr, "The count of timeout_list=%d\n", cnt);

    cnt = 0;
    list_for_each_entry(ctx, &mgmt->ctx_caches, node) {
        cnt++;
    }
    fprintf(stderr, "The count of ctx_caches=%d\n", cnt);

    cnt = 0;
    list_for_each_entry(buf_info, &mgmt->http_buf_caches, node) {
        cnt++;
    }
    fprintf(stderr, "The count of http_buf_caches=%d\n", cnt);

    cnt = 0;
    list_for_each_entry(buf_info, &mgmt->buf_prepare.list_todo, node) {
        cnt++;
    }
    fprintf(stderr, "The list_todo=%d\n", cnt);

    fprintf(stderr, " The count of pollfds=%d\n", mgmt->count_pollfds);
    for(i = 0; i < mgmt->max_fds; i++) {
        if(NULL != mgmt->http_lookup[i]) {
            fprintf(stderr, "http_lookup[%d] is not NULL\n", i);
        }
    }

    fprintf(stderr, " The tcp forward len = %d\n", mgmt->len_forward);
}

int http_mgmt_release_all(http_mgmt* mgmt)
{
    http_context* ctx, *n;

    assert(NULL != mgmt);

    release_prepare(mgmt);

    list_for_each_entry_safe(ctx, n, &mgmt->list_timeout, node_time)
    {
        // remove from list_ready
        if(CALLING_READY == ctx->status)
        {
            list_del(&ctx->node);
            mgmt->ready_len--;
        }

        //Force release
        http_context_release(mgmt, ctx);
    }

    mgmt_show_statistics(mgmt);
    return 0;
}

int http_mgmt_isshake(http_mgmt* mgmt)
{
    return mgmt->shakehand;
}

int http_mgmt_handshake(http_mgmt* mgmt)
{
    http_buf_info* buf_info;
    http_c_header header;
    char shake[512];
    int len;

    shake[0] = '\0';
    len = sprintf(shake,
            "{\"username\":\"%s\",\"host\":\"%s\",\"port\":%d}"
            , mgmt->username, mgmt->local_host, mgmt->local_port);

    len += HTTP_C_HEADER_LEN;

    header.magic = htonl(HTTP_C_MAGIC);
    header.version = HTTP_C_VERSION;
    header.type = HTTP_C_HAND;
    header.seq = 0;
    header.length = htonl(len);
    header.reserved = 0;

    buf_info = alloc_buf(mgmt);
    memcpy(buf_info->buf, &header, HTTP_C_HEADER_LEN);
    memcpy(buf_info->buf+HTTP_C_HEADER_LEN
            , shake, len-HTTP_C_HEADER_LEN);

    buf_info->start = 0;
    buf_info->len = len;
    buf_info->total_len = len;

    // TODO do better for it. Move to server lists and wait for writable
    list_add(&buf_info->node, &mgmt->buf_toserver.list_todo);
    mgmt->buf_toserver.curr = next_buf_info(&mgmt->buf_toserver.list_todo);
    mgmt->toserver = 1;

    return 0;
}

struct pollfd* mgmt_add_fd(http_mgmt* mgmt, int fd, int events)
{
    if(fd >= mgmt->max_fds)
    {
        lwsl_err("mgmt_add_fd error, the fd is out of range\n");
        return NULL;
    }

    mgmt->fd_lookup[fd] = mgmt->count_pollfds;
    mgmt->pollfds[mgmt->count_pollfds].fd = fd;
    mgmt->pollfds[mgmt->count_pollfds].events = events;
    mgmt->pollfds[mgmt->count_pollfds++].revents = 0;

    return (&mgmt->pollfds[mgmt->count_pollfds-1]);
}

int mgmt_del_fd(http_mgmt* mgmt, int fd)
{
    int m;
    if (!--mgmt->count_pollfds) {
        return -1;
    }
    m = mgmt->fd_lookup[fd]; // The slot of fd
    /* have the last guy take up the vacant slot */
    mgmt->pollfds[m] = mgmt->pollfds[mgmt->count_pollfds];
    mgmt->fd_lookup[mgmt->pollfds[mgmt->count_pollfds].fd] = m;

    return 0;
}

http_context* alloc_context(http_mgmt* mgmt)
{
    http_context* ctx = NULL;

    assert(NULL != mgmt);

    if(!list_empty(&mgmt->ctx_caches))
    {
        list_for_each_entry(ctx, &mgmt->ctx_caches, node) {
            break;
        }
        list_del(&ctx->node);
    }

    return ctx;
}

void free_context(http_mgmt* mgmt, http_context* ctx)
{
    assert(NULL != mgmt);

    if(NULL != ctx)
    {
        list_add(&ctx->node, &mgmt->ctx_caches);
    }
}

http_buf_info* alloc_buf(http_mgmt* mgmt)
{
    http_buf_info* buf_info = NULL;
    assert(NULL != mgmt);

    if(!list_empty(&mgmt->http_buf_caches))
    {
        list_for_each_entry(buf_info, &mgmt->http_buf_caches, node) {
            break;
        }
        list_del(&buf_info->node);

        buf_info->len = 0;
        buf_info->start = 0;
        buf_info->total_len = 0;
    }

    return buf_info;
}

void free_buf(http_mgmt* mgmt, http_buf_info* buf)
{
    assert(NULL != mgmt);

    if(NULL != buf)
    {
        list_add(&buf->node, &mgmt->http_buf_caches);
    }
}

static void release_prepare(http_mgmt* mgmt)
{
    if(NULL != mgmt->buf_prepare.curr) {
        free_buf(mgmt, mgmt->buf_prepare.curr);
    }
    mgmt->buf_prepare.curr = NULL;
    mgmt->buf_prepare.len = 0;
    mgmt->buf_prepare.total_len = 0;
    mgmt->buf_prepare.type = 0;

    list_splice_init(&mgmt->buf_prepare.list_todo, &mgmt->http_buf_caches);
}

static void save_prepare(http_mgmt* mgmt, http_buf_info* buf_info)
{
    buf_info->len = buf_info->start;
    buf_info->total_len = buf_info->start;
    mgmt->buf_prepare.len += buf_info->start;
    list_add_tail(&buf_info->node, &mgmt->buf_prepare.list_todo);
    buf_info->start = 0;
    mgmt->buf_prepare.curr = NULL;
}

void prepare_process(http_mgmt* mgmt)
{
    int client_id;
    http_buf_info* buf_info;
    http_param param = {0};

    lwsl_info("%s type=%d\n", __FUNCTION__, mgmt->buf_prepare.type);

    switch(mgmt->buf_prepare.type)
    {
    case HTTP_C_HAND:
        buf_info = next_buf_info(&mgmt->buf_prepare.list_todo);
        if(NULL == buf_info) {
            goto THE_END;
        }

        memcpy(&client_id, buf_info->buf, 4);
        mgmt->client_id = htonl(client_id);
        mgmt->shakehand = 1;
        release_prepare(mgmt);
        lwsl_info("get the client id=%d\n", mgmt->client_id);
        break;
    case HTTP_C_TUNNELREQ:
        /* Server send event to clients */
        if(0 != process_tunnel_req(mgmt)) {
            release_prepare(mgmt);
        }
        break;
    case HTTP_C_TUNNELRESP:
        /* Client request and the server response to server */
        if(0 != process_tunnel_resp(mgmt)) {
            lwsl_warn("process tunnel resp error\n");
            release_prepare(mgmt);
        }
        break;
    case HTTP_C_REQ:
        param.pbuf = &mgmt->buf_prepare;
        param.timeout = 10;
        if(0 != http_context_create(mgmt, &param)) {
            release_prepare(mgmt);
        }
        break;
    case HTTP_C_FORWARD:
        if(0 != process_tcp_forward(mgmt)) {
            release_prepare(mgmt);
        }
        break;
    case HTTP_C_FORWARD_REQ:       //The client send forward req
        if(0 != process_tcp_forwardresp(mgmt)) {
            release_prepare(mgmt);
        }
    default:
        release_prepare(mgmt);
        break;
    }

THE_END:
    mgmt->buf_prepare.len = 0;
    mgmt->buf_prepare.total_len = 0;
    mgmt->buf_prepare.type = 0;

    //Reset the buf_prepare
    mgmt->sync_prepare = 0;
    time(&mgmt->sync_time);
}

int http_mgmt_prepare(http_mgmt* mgmt, char* buf, int len)
{
    int i, tmp_len;
    unsigned short seq;
    unsigned int magic, length, const_magic = htonl(HTTP_C_MAGIC);
    unsigned char version, type;
    http_buf_info* buf_info = mgmt->buf_prepare.curr;

    assert(len < HTTP_BUF_SIZE);

    lwsl_notice("mgmt prepare got len=%d\n", len);

    if(!mgmt->sync_prepare) {
        /* process the header and exclude from bufs*/
        if(len < 16) {
            return 10;
        }
        i = 0;
        memcpy(&magic, buf, 4);
        i += 4;

        memcpy(&version, buf+i, 1);
        i += 1;

        memcpy(&type, buf+i, 1);
        i += 1;

        memcpy(&seq, buf+i, 2);
        seq = htons(seq);
        i+= 2;

        memcpy(&length, buf+i, 4);
        i += 4;

        if((magic != const_magic)
                || (version != HTTP_C_VERSION) ) {
            lwsl_warn("get error magic from server\n");
            return 11;
        }
        release_prepare(mgmt);
        length = htonl(length);
        if(length > HTTP_IN_MAX) {
            lwsl_warn("the total len=%d is too big, ignore it\n", length);
            return 12;
        }
        mgmt->sync_prepare = 1;
        time(&mgmt->sync_time);
        buf += HTTP_C_HEADER_LEN;
        len -= HTTP_C_HEADER_LEN;
        length -= HTTP_C_HEADER_LEN;
        mgmt->buf_prepare.total_len = length;
        mgmt->buf_prepare.type = type;
        mgmt->buf_prepare.seq = seq;

        lwsl_info("sync: seq=%d alllength=%u total_len=%d\n"
                , seq, length+16, mgmt->buf_prepare.total_len);
    }

    if(0 == len) {
        return 0;
    }

    // Process new buffer
    for(;;) {
        if(NULL == buf_info) {
            buf_info = alloc_buf(mgmt);
            if(NULL == buf_info) {
                release_prepare(mgmt);
                lwsl_warn("Cannot alloc buf_info for prepare, Out of memory\n");
                return 4;
            }

            mgmt->buf_prepare.curr = buf_info;
        }

        if((buf_info->start + len) > HTTP_BUF_SIZE) {
            lwsl_info("begin to save start=%d len=%d\n", buf_info->start, len);
            save_prepare(mgmt, buf_info);
            buf_info = mgmt->buf_prepare.curr;
            continue;
        }

        memcpy(buf_info->buf+buf_info->start, buf, len);
        buf_info->start += len;
        tmp_len = buf_info->start + mgmt->buf_prepare.len;
        lwsl_notice("got all len=%d\n", tmp_len);

        if(tmp_len >= mgmt->buf_prepare.total_len) {
            // Check the length
            buf_info->start -= tmp_len - mgmt->buf_prepare.total_len;

            //make a new request now
            save_prepare(mgmt, buf_info);
            buf_info = mgmt->buf_prepare.curr;
            prepare_process(mgmt);
        }

        break;
    }

    return 0;
}

static void insert_timeout(http_mgmt* mgmt, http_context* new)
{
    assert(NULL != mgmt);

    http_context *entry;

    list_for_each_entry(entry, &mgmt->list_timeout, node_time)
    {
        if(new->timeout < entry->timeout)
        {
            list_add_tail(&new->node_time, &entry->node_time);
            return;
        }
    }

    list_add_tail(&new->node_time, &mgmt->list_timeout);
}

static void insert_ready_prio(http_mgmt* mgmt, http_context* ctx)
{
    struct list_head* plist;
    int prio = ctx->prio;
    if((prio < 0) || (prio >= CALLING_PRIO_COUNT)) {
        prio = CALLING_PRIO_LOW;
        ctx->prio = prio;
    }
    plist = &mgmt->list_ready[prio];
    list_add(&ctx->node, plist);
    mgmt->ready_len++;
}

static void generate_error(http_mgmt* mgmt, http_context* ctx)
{
    unsigned short tmp_len = 0;
    http_buf_info* buf_info;
    http_c_header header;
    buf_info = alloc_buf(mgmt);
    if(NULL == buf_info) {
        return;
    }

    //Init the header
    header.magic = htonl(HTTP_C_MAGIC);
    header.version = HTTP_C_VERSION;
    header.type = HTTP_C_RESP;
    header.seq = htons(ctx->seq);
    header.reserved = 0;

    tmp_len = (unsigned short)sprintf(buf_info->buf + HTTP_C_HEADER_LEN,
    "HTTP/1.1 404 Not Found\x0d\x0a"
    "Content-Length: 1635\x0d\x0a"
    "Content-Type: text/html\x0d\x0a"
    "Connection: Close\x0d\x0a");

    tmp_len += HTTP_C_HEADER_LEN;
    header.length = htons(tmp_len);
    memcpy(buf_info->buf, &header, HTTP_C_HEADER_LEN);

    buf_info->start = 0;
    buf_info->len = tmp_len;
    buf_info->total_len = tmp_len;

    lwsl_warn("generate error to seq = %d len=%d errno=%d \n", ctx->seq, tmp_len, ctx->errcode);
    //Add to last
    list_add_tail(&buf_info->node, &mgmt->buf_toserver.list_todo);
    http_mgmt_toserver(mgmt);
}

// change state to be ready and execute
int http_context_execute(http_mgmt* mgmt, http_context* ctx)
{
    CALLER_STATUS status;

    if(CALLING_READY == ctx->status) {
        list_del(&ctx->node);
        mgmt->ready_len--;
    }

    status = (*ctx->func_run)(mgmt, ctx);
    if(CALLER_FINISH == status) {
        if(0 != ctx->errcode) {
            //Generate 404 message
            generate_error(mgmt, ctx);
        }
        http_context_release(mgmt, ctx);
    }
    else if(CALLER_CONTINUE == status) {
        insert_ready_prio(mgmt, ctx);
    }
    else {
        ctx->status = CALLING_PENDING;
    }

    return 0;
}

int http_mgmt_service(http_mgmt* mgmt, struct pollfd* pfd)
{
    common_context* common_ctx;
    CALLER_STATUS caller_status;
    http_context* ctx = NULL;
    tcp_server_context* tcp_server;
    tcp_client_context* tcp_client;
    tcp_forward_context* tcp_forward;

    if(0 == pfd->revents)
    {
        return -1; // params error
    }

    lwsl_notice("revents = %x\n", pfd->revents);

    common_ctx = mgmt->http_lookup[pfd->fd];
    if(NULL == common_ctx) {
        lwsl_warn("service sock error, the context has been released\n");
        return 0;
    }

    switch(common_ctx->context_type) {
    case CONTEXT_TYPE_TCPSERVER:
        tcp_server = (tcp_server_context*) common_ctx;
        tcp_server->status = CALLING_READY;
        if(CALLER_CONTINUE == (*tcp_server->func_run)(mgmt, tcp_server)) {
            tcp_server->status = CALLING_READY;
        }
        else {
            tcp_server->status = CALLING_PENDING;
        }
        break;
    case CONTEXT_TYPE_HTTP:
        ctx = (http_context*)common_ctx;
        if(pfd->revents & (POLLERR|POLLHUP)){
            lwsl_warn("release context because of socket error\n");
            //TODO to better hear
            if(CALLING_READY == ctx->status) {
                list_del(&ctx->node);
                mgmt->ready_len--;
            }
            http_context_release(mgmt, ctx);
        }
        else {
            ctx->revents = pfd->revents;
            http_context_execute(mgmt, ctx);
        }
        break;
    case CONTEXT_TYPE_TCPCLIENT:
        /* Client */
        tcp_server = &mgmt->tcp_server;
        tcp_client = (tcp_client_context*)common_ctx;
        if(pfd->revents & (POLLERR|POLLHUP)) {
            lwsl_warn("release client context because of socket error\n");
            tcp_client_release(mgmt, tcp_client);
        }
        else {
            caller_status = (*tcp_client->func_run)(mgmt, tcp_client);
            if(CALLER_CONTINUE == caller_status) {
                tcp_client->status = CALLING_READY;
            }
            else if(CALLER_FINISH == caller_status) {
                tcp_client_release(mgmt, tcp_client);
            } else {
                tcp_client->status = CALLING_PENDING;
            }
        }
        break;
    case CONTEXT_TYPE_FORWARD:
        tcp_forward = (tcp_forward_context*)common_ctx;
        caller_status = (*tcp_forward->func_run)(mgmt, tcp_forward);
        if(CALLER_CONTINUE == caller_status) {
            tcp_forward->status = CALLING_READY;
        } else if(CALLER_FINISH == caller_status) {
            tcp_forward_release(mgmt, tcp_forward);
        } else {
            tcp_forward->status = CALLING_PENDING;
        }
        break;
    default:
        break;
    }

    pfd->revents = 0;

    return 0;
}

int http_mgmt_run_timeout(http_mgmt* mgmt)
{
    http_context* ctx, *n;
    time_t now;

    time(&now);

    //Check for timeout first
    list_for_each_entry_safe(ctx, n, &mgmt->list_timeout, node_time)
    {
        if(ctx->timeout > now)
        {
            break;
        }

        // remove from list_ready
        if(CALLING_READY == ctx->status)
        {
            list_del(&ctx->node);
            mgmt->ready_len--;
        }

        //Force release
        http_context_release(mgmt, ctx);
    }

    return 0;
}

int http_mgmt_run(http_mgmt* mgmt)
{
    int i;
    http_context* ctx, *n;
    time_t now;
    struct list_head lists[CALLING_PRIO_COUNT];

    for(i = 0; i < CALLING_PRIO_COUNT; i++)
    {
        INIT_LIST_HEAD(&lists[i]);
        list_splice_init(&mgmt->list_ready[i], &lists[i]);
    }

    ctx = NULL;
    for(i = 0; i < CALLING_PRIO_COUNT; i++)
    {
        list_for_each_entry_safe(ctx, n, &lists[i], node)
        {
            http_context_execute(mgmt, ctx);
        }
    }

    tcp_server_run(mgmt);

    // Set the prepare buffer timeout
    time(&now);
    if(mgmt->sync_prepare) {
        if((now - mgmt->sync_time) > HTTP_TIMEOUT_SECS) {
            lwsl_info("reset the sync to zero\n");
            mgmt->sync_prepare = 0;
        }
    }

    if((now - mgmt->last_alive_time) > 4*HTTP_TIMEOUT_SECS) {
        lwsl_info("Websocket timeout, reconnecting\n");
        websocket_closed();
    }

    return 0;
}

/* #define SERVERIP "127.0.0.1"
#define SERVERPORT 8060
#define MAXDATASIZE 1024 */

int http_context_connect(http_mgmt* mgmt, http_context* ctx)
{
    int rc;
    struct sockaddr_in server_addr;
    struct pollfd* pfd;

    pfd = (struct pollfd*)ctx->pfd;

    ccrBegin(ctx);

    while(POLLOUT != pfd->revents)
    {
        memset(&server_addr, 0, sizeof(struct sockaddr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(ctx->port);
        server_addr.sin_addr.s_addr = name_resolve(ctx->hostname);
        lwsl_notice("before connect ctx->seq=%d\n", ctx->seq);
        rc = connect(ctx->sockfd, (struct sockaddr*) &server_addr, sizeof(struct sockaddr));
        if(rc >= 0)
        {
            break;
        }

        if((EALREADY == errno) || (EINPROGRESS == errno))
        {
            pfd->events &= ~POLLIN;
            pfd->events |= POLLOUT;
            lwsl_notice("still connecting ctx->seq=%d\n", ctx->seq);
            ccrReturn(ctx, CALLER_CONTINUE);
        }
        else
        {
            lwsl_warn("connect fail\n");
            ctx->errcode = 3; //connected fail
            ccrReturn(ctx, CALLER_FINISH);
        }
    }
    // Clear pollout flag
    // pfd->events &= ~POLLOUT;
    lwsl_info("connected ok\n");

    // TODO do better. Change state to write
    ctx->state = CALLER_STATE_WRITE;
    ctx->func_run = &http_context_write;
    memset(&ctx->context, 0, sizeof(ctx->context));
    ccrFinish(ctx, CALLER_CONTINUE);
}

int http_context_write(http_mgmt* mgmt, http_context* ctx)
{
    int n = 0;
    struct pollfd* pfd;
    http_buf_info* buf_info;

    pfd = (struct pollfd*)ctx->pfd;
    buf_info = ctx->buf_write.curr;

    ccrBegin(ctx);

    // Get but not deleted
    ctx->buf_write.curr = next_buf_info(&ctx->buf_write.list_todo);
    buf_info = ctx->buf_write.curr;
    if(NULL == buf_info) {
        lwsl_warn("buf_info is null hear\n");
        ctx->errcode = 4; //out of memory
        ccrReturn(ctx, CALLER_FINISH);
    }

    // Now write bufs
    while(buf_info != NULL)
    {
        lwsl_info("req=%s\n", buf_info->buf);
        n = write(ctx->sockfd, buf_info->buf + buf_info->start, buf_info->len);
        if(n < 0)
        {
            if(errno == EINTR || errno == EAGAIN) {
                ccrReturn(ctx, CALLER_PENDING);
            }
            else {
                lwsl_warn("write sock error, seq=%d\n", ctx->seq);
                ccrReturn(ctx, CALLER_FINISH);
            }
        }
        if ((buf_info->len -= n) > 0) {
            buf_info->start += n;
            ccrReturn(ctx, CALLER_PENDING);
        } else {
            //Finished write
            list_del(&buf_info->node);
            free_buf(mgmt, buf_info);
            ctx->buf_write.curr = next_buf_info(&ctx->buf_write.list_todo);
            buf_info = ctx->buf_write.curr;
        }
    }

    // Now write done, change to read state
    pfd->events &= ~POLLOUT;
    pfd->events |= POLLIN;
    ctx->state = CALLER_STATE_READ;
    memset(&ctx->context, 0, sizeof(ctx->context));
    ctx->func_run = &http_context_read;

    //free the write bufs now
    list_splice_init(&ctx->buf_write.list_todo, &mgmt->http_buf_caches);

    ccrFinish(ctx, CALLER_PENDING);
}

int http_mgmt_toserver(http_mgmt* mgmt)
{
    if(!mgmt->toserver)
    {
        mgmt->buf_toserver.curr = next_buf_info(&mgmt->buf_toserver.list_todo);
        if(NULL != mgmt->buf_toserver.curr) {
            websocket_go_writable();
            mgmt->toserver = 1;
        }
    }

    return 0;
}

static void context_save_read(http_context* ctx, http_buf_info* buf_info)
{
    buf_info->len = buf_info->start;
    buf_info->total_len = buf_info->start;
    ctx->buf_read.len += buf_info->start;
    buf_info->start = 0;
    list_add_tail(&buf_info->node, &ctx->buf_read.list_todo);
    ctx->buf_read.curr = NULL;
    lwsl_info("save read seq=%d len=%d\n", ctx->seq, buf_info->len);
}

int http_chunk_check(http_mgmt* mgmt, http_context* ctx, http_buf_info* buf_info)
{
    char *p, *lrln = "\r\n\r\n", *transfer = "Transfer-Encoding:", *chunked = "chunked";
    int offset = 0;
    http_buf_info* tmp_buf = NULL;
    int lrln_len = strlen(lrln);

    assert((mgmt != NULL) && (NULL != ctx) && (NULL != buf_info));

    buf_info->buf[buf_info->len] = '\0';    //TODO should do better for this
    p = strstr(buf_info->buf, lrln);
    if(NULL != p) {
        /* header found */
        offset = p - buf_info->buf + lrln_len;

        tmp_buf = alloc_buf(mgmt);
        if(NULL == tmp_buf) {
            ctx->errcode = 11;  //Parse header error
            lwsl_warn("parse header, out of memory\n");
            return -1;
        }

        memcpy(tmp_buf->buf, buf_info->buf, offset);
        tmp_buf->buf[offset] = '\0';
        p = strcasestr(tmp_buf->buf, transfer);
        if((NULL != p)
            && (NULL != strcasestr(p, chunked))) {
            ctx->is_chunk = 1;
        }

        free_buf(mgmt, tmp_buf);
    }

    return offset;
}

typedef enum _chunk_state {
    CHUNK_BEGIN = 0,
    CHUNK_LRLN,
    CHUNK_GET_SIZE
} chunk_state;

int http_parse(http_mgmt* mgmt, http_context* ctx)
{
    int rc, offset, chunk_size, i, j, last_i, new_total = 0;
    char chunk_str[HTTP_CHUNK_MAX+1];
    http_buf_info *buf_info, *last_buf;
    struct list_head list;
    struct list_head* plist = &list;
    chunk_state state = CHUNK_BEGIN;

    INIT_LIST_HEAD(plist);

    //First check is it's a chunk response, Get the first buf but not delete
    buf_info = next_buf_info(&ctx->buf_read.list_todo);
    if(NULL == buf_info) {
        ctx->errcode = 15;
        return -1;
    }

    rc = http_chunk_check(mgmt, ctx, buf_info);
    if(rc < 0) {
        return -1;
    }
    if(!ctx->is_chunk) {
        return 0;   /* not the chunk response */
    }
    offset = rc;
    last_i = offset;
    new_total = offset;
    last_buf = buf_info;
    list_del(&buf_info->node);     /* delete it first */
    j = 0;

    // 0 means ok, < 0 means error, > 0 means continue
    while(rc > 0) {
        switch(state) {
        case CHUNK_BEGIN:
            for(i = offset; (i < buf_info->len) && (j < HTTP_CHUNK_MAX); i++) {
                // Ignore till found
                if(buf_info->buf[i] == '\r') {
                    break;
                }
                chunk_str[j++] = buf_info->buf[i];
            }
            if(j == HTTP_CHUNK_MAX) {
                //Found chunk size failed
                ctx->errcode = 12;
                rc = -1;
                lwsl_warn("found chunk size failed seq=%d\n", ctx->seq);
                break;
            }
            if(i == buf_info->len) {
                if(buf_info != last_buf) {
                    /* Not the first buf, free it */
                    free_buf(mgmt, buf_info);
                }
                buf_info = next_buf_info(&ctx->buf_read.list_todo);
                if(NULL == buf_info) {
                    rc = -1;
                    lwsl_warn("parse chunk, but the buf is null, state begain, seq=%d\n", ctx->seq);
                    break;
                }
                list_del(&buf_info->node);
                offset = 0;
                break;
            }
            chunk_str[j] = '\0';
            offset = i+1;
            state = CHUNK_LRLN;
            j = 0;      /* reset j */
            break;
        case CHUNK_LRLN:
            if(offset == buf_info->len) {
                if(buf_info != last_buf) {
                    /* Not the first buf, free it */
                    free_buf(mgmt, buf_info);
                }
                buf_info = next_buf_info(&ctx->buf_read.list_todo);
                if(NULL == buf_info) {
                    rc = -1;
                    lwsl_warn("parse chunk, but the buf is null, state lr, seq=%d\n", ctx->seq);
                    break;
                }
                list_del(&buf_info->node);
                offset = 0;
            }
            if(buf_info->buf[offset] != '\n') {
                rc = -1;
                lwsl_warn("parse chunk size error, seq=%d\n", ctx->seq);
                break;
            }
            offset++;
            j = 0;      /* reset j */

            if(chunk_str[0] == '\0') {
                //Just ignore the \r\n, return to begin state
                state = CHUNK_BEGIN;
                break;
            }

            state = CHUNK_GET_SIZE;
            sscanf(chunk_str, "%x", &chunk_size);
            lwsl_info("get chunksize=%d\n", chunk_size);

            if(0 == chunk_size) {
                // Got all chunk hear
                rc = 0;
                if(buf_info != last_buf) {
                    free_buf(mgmt, buf_info);
                    buf_info = NULL;
                }
                if(last_i > 0) {
                    last_buf->len = last_i;
                    last_buf->total_len = last_i;
                    list_add_tail(&last_buf->node, plist);
                } else {
                    free_buf(mgmt, last_buf);
                }
                //free the others
                list_splice_init(&ctx->buf_read.list_todo, &mgmt->http_buf_caches);
                list_splice(plist, &ctx->buf_read.list_todo);
                ctx->buf_read.len = new_total;
                plist = NULL;
                last_buf = NULL;
                buf_info = NULL;
                rc = 0;
            }
            break;
        case CHUNK_GET_SIZE:
            for(i = offset; (i < buf_info->len) && (j < chunk_size); i++, j++) {
                last_buf->buf[last_i++] = buf_info->buf[i];
                new_total++;
                if(last_i >= HTTP_BUF_SIZE) {
                    //Save the last
                    last_buf->len = last_i;
                    last_buf->start = 0;
                    last_buf->total_len = 0;
                    list_add_tail(&last_buf->node, plist);

                    /* Realloc last_buf */
                    last_buf = alloc_buf(mgmt);
                    assert(NULL != last_buf);
                    last_i = 0;
                }
            }
            if(i == buf_info->len) {
                if(buf_info != last_buf) {
                    /* Not the first buf, free it */
                    free_buf(mgmt, buf_info);
                }
                buf_info = next_buf_info(&ctx->buf_read.list_todo);
                if(NULL == buf_info) {
                    lwsl_warn("parse chunk size error, state=getsize, seq=%d\n", ctx->seq);
                    rc = -1;
                }
                list_del(&buf_info->node);
                offset = 0;
                break;
            }
            if(j == chunk_size) {
                // Chunk finished
                state = CHUNK_BEGIN;
                offset = i;
                j = 0;      /* reset j */
            }
            break;
        default:
            lwsl_err("parse chunk, should never got hear\n");
            break;
        }
    }

    if(rc < 0) {
        //free the bufs
        if(NULL != last_buf) {
            free_buf(mgmt, last_buf);
        }
        if((NULL != buf_info) && (buf_info != last_buf)) {
            free_buf(mgmt, buf_info);
        }
        if(NULL != plist) {
            list_splice(plist, &mgmt->http_buf_caches);
        }
        last_buf = NULL;
        buf_info = NULL;
    }
    return rc;
}

int http_context_read(http_mgmt* mgmt, http_context* ctx)
{
    int n, left;
    struct pollfd* pfd;
    http_buf_info* buf_info;
    http_c_header header;

    /* Never used like this
     ccrBeginContext
        int error_code;
    ccrEndContext(ctx); */

    pfd = (struct pollfd*)ctx->pfd;

    ccrBegin(ctx);

    for(;;) {
        buf_info = ctx->buf_read.curr;
        if(NULL == buf_info) {
            //Alloc but not add to list
            buf_info = alloc_buf(mgmt);
            ctx->buf_read.curr = buf_info;
        }
        left = HTTP_BUF_SIZE - buf_info->start;
        assert(left > 0);

        n = read(ctx->sockfd, buf_info->buf+buf_info->start, left);
        //Ignore the n < 0 state
        //assert(n >= 0);

        if (n > 0) {
            buf_info->start += n;

            if(buf_info->start >= HTTP_BUF_SIZE) {
                //Got a full one
                context_save_read(ctx, buf_info);
                buf_info = ctx->buf_read.curr;
            }
        } else if ((!n) || (errno != EINTR && errno != EAGAIN)) {
            //Read finished
            if(0 == buf_info->start) {
                //Just free it
                free_buf(mgmt, buf_info);
                buf_info = NULL;
                ctx->buf_read.curr = NULL;
            } else {
                context_save_read(ctx, buf_info);
                buf_info = ctx->buf_read.curr;
            }

            if(0 != http_parse(mgmt, ctx)) {
                ccrReturn(ctx, CALLER_FINISH);
            }

            //Init the header
            header.magic = htonl(HTTP_C_MAGIC);
            header.version = HTTP_C_VERSION;
            header.type = HTTP_C_RESP;
            ctx->buf_read.len += HTTP_C_HEADER_LEN;
            header.length = htonl(ctx->buf_read.len);
            header.seq = htons(ctx->seq);
            header.reserved = 0;

            buf_info = alloc_buf(mgmt);
            memcpy(buf_info->buf, &header, HTTP_C_HEADER_LEN);
            buf_info->start = 0;
            buf_info->len = HTTP_C_HEADER_LEN;
            buf_info->total_len = HTTP_C_HEADER_LEN;
            // Add the header at first
            list_add(&buf_info->node, &ctx->buf_read.list_todo);

            lwsl_info("send total_len=%d seq=%d\n"
                    , ctx->buf_read.len, ctx->seq);

            // Move to server lists
            list_splice_tail_init(&ctx->buf_read.list_todo
                    , &mgmt->buf_toserver.list_todo);
            ctx->buf_read.len = 0;
            http_mgmt_toserver(mgmt);

            ccrReturn(ctx, CALLER_FINISH);
        }
        ccrReturn(ctx, CALLER_PENDING);
    }

    ccrFinish(ctx, CALLER_FINISH);
}

// Already delete from the list_ready, but still in the list_timeout
void http_context_release(http_mgmt* mgmt, http_context* ctx)
{
    ctx->status = CALLING_FINISH;
    if(ctx->sockfd > 0)
    {
        close(ctx->sockfd);
        mgmt_del_fd(mgmt, ctx->sockfd);
        mgmt->http_lookup[ctx->sockfd] = NULL;
        ctx->sockfd = -1;
    }

    // Free bufs but not use free_buf hear
    if(NULL != ctx->buf_read.curr) {
        free_buf(mgmt, ctx->buf_read.curr);
        ctx->buf_read.curr = NULL;
    }
    if(NULL != ctx->buf_write.curr) {
        free_buf(mgmt, ctx->buf_write.curr);
        ctx->buf_read.curr = NULL;
    }
    list_splice_init(&ctx->buf_write.list_todo, &mgmt->http_buf_caches);
    list_splice_init(&ctx->buf_read.list_todo, &mgmt->http_buf_caches);

    list_del(&ctx->node_time);
    free_context(mgmt, ctx);
    mgmt->total_process++;

    //Just show the message
    lwsl_notice("release the seq=%d\n", ctx->seq);
}

int http_context_init(http_mgmt* mgmt, http_context* ctx)
{
    ctx->context_type = CONTEXT_TYPE_HTTP;
    ctx->errcode = 0; //no error
    ctx->state = CALLER_STATE_CONNECT;
    ctx->func_run = &http_context_connect;
    ctx->status = CALLING_READY;
    memset(&ctx->context, 0, sizeof(ctx->context));
    insert_ready_prio(mgmt, ctx);
    insert_timeout(mgmt, ctx);
    mgmt->total_add++;

    ctx->pfd = mgmt_add_fd(mgmt, ctx->sockfd, (POLLIN | POLLERR | POLLHUP) );
    mgmt->http_lookup[ctx->sockfd] = (common_context*)ctx;

    return 0;
}

//Get buf but not delete it
static http_buf_info* next_buf_info(struct list_head* list)
{
    http_buf_info* buf_info = NULL;

    if(!list_empty(list))
    {
        list_for_each_entry(buf_info, list, node) {
            break;
        }
        //list_del(&buf_info->node);
    }

    return buf_info;
}

static void str_strip(char* s)
{
    int i, j;
    for (i=0, j=0; s[i] != '\0'; i++)
    {
        if(!isspace(s[i])) {
            s[j++] = s[i];
        }
    }
    s[j] = '\0';
}

static int get_host_from_buf(http_context* ctx, char* buf)
{
    char hostname[128];
    char *tok1, *tok2, *sep = "\r\n", *host="host:";
    int n = strlen(host);

    lwsl_parser("host before:\n%s", buf);

    tok1 = strstr(buf, host);
    if(NULL == tok1) {
        return 1;
    }
    tok2 = strstr(tok1+n, sep);
    if(NULL == tok2) {
        return 1;
    }
    strncpy(hostname, tok1+n, tok2-tok1-n);
    hostname[tok2-tok1-n] = '\0';
    lwsl_parser("origin hostname=%s\n", hostname);

    tok2 = strstr(hostname, ":");
    if(NULL != tok2) {
        strncpy(ctx->hostname, hostname, tok2-hostname);
        ctx->port = atoi(tok2+1);
    } else {
        strcpy(ctx->hostname, hostname);
        ctx->port = 80;
    }
    str_strip(ctx->hostname);
    lwsl_info("old:%s %s %d\n", hostname, ctx->hostname, ctx->port);

    return 0;
}

int host_init(http_context* ctx, http_param* param)
{
    http_buf_info* buf_info;
    assert(param->pbuf != NULL);

    buf_info = next_buf_info(&param->pbuf->list_todo);
    return get_host_from_buf(ctx, buf_info->buf);
}

int http_context_create(http_mgmt* mgmt, http_param* param)
{
    int sockfd, rc = 0, optval = 0;
    http_context* ctx = NULL;

    do
    {
        ctx = alloc_context(mgmt);
        if(NULL == ctx)
        {
            break;
        }

        memset(ctx, 0, sizeof(http_context));
        rc = host_init(ctx, param);
        if(0 != rc) {
            lwsl_err("host_init error\n");
            break;
        }

        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            lwsl_err("create socket error\n");
            rc = -1;    // Error
            break;
        }
        ctx->sockfd = sockfd;
        lwsl_info("created socket\n");
        //http://stackoverflow.com/questions/855544/is-there-a-way-to-flush-a-posix-socket
        setsockopt(ctx->sockfd, SOL_TCP, TCP_NODELAY, (const void *)&optval, sizeof(optval));
        fcntl(sockfd, F_SETFL, O_NONBLOCK);
        lwsl_info("set unblocked sock\n");

        // Init all list
        INIT_LIST_HEAD(&ctx->buf_read.list_todo);
        INIT_LIST_HEAD(&ctx->buf_write.list_todo);

        // Just test hear
        time(&ctx->timeout);
        ctx->timeout += param->timeout;
        ctx->seq = param->pbuf->seq;
        list_splice_init(&param->pbuf->list_todo, &ctx->buf_write.list_todo);

        http_context_init(mgmt, ctx);
    } while(0);

    if(rc != 0)
    {
        if(NULL == ctx)
        {
            //Just free the ctx, but not release
            free_context(mgmt, ctx);
            ctx = NULL;
        }
    }

    return rc;
}

int http_mgmt_writable(void* context, void* wsi, http_mgmt* mgmt)
{
    int n, rc = 0;
    http_buf_info* buf_info;

    assert(mgmt->toserver > 0);

    do {
        buf_info = mgmt->buf_toserver.curr;
        if(NULL == buf_info) {
            rc = -1;
            lwsl_err("websocket writing but the buf_info is null\n");
            break;
        }

        n = websocket_write(wsi, buf_info->buf, buf_info->len);
        lwsl_info("websocket write len=%d\n", buf_info->len);
        if((n < 0) || (n < buf_info->len)) {
            rc = -1;
            lwsl_err("websocket writed but n=%d\n", n);
            break;
        }

        list_del(&buf_info->node);
        free_buf(mgmt, buf_info);
    } while(0);

    //Get but not free
    mgmt->buf_toserver.curr = next_buf_info(&mgmt->buf_toserver.list_todo);
    if(NULL != mgmt->buf_toserver.curr) {
        websocket_write_again(context, wsi);
    }
    else {
        mgmt->toserver = 0;
    }

    if(0 != rc) {
        // Free the buf_info
        list_del(&buf_info->node);
        free_buf(mgmt, buf_info);
    }

    return rc;
}

int process_tunnel_req(http_mgmt* mgmt)
{
    http_c_header h, *header;
    tcp_client_context* tcp_client;
    http_buf_info* buf_info, *new_buf;
    http_buf* pbuf = &mgmt->buf_prepare;
    tcp_server_context* tcp_server = &mgmt->tcp_server;
    header = &h;

    if(pbuf->len > (HTTP_BUF_SIZE-HTTP_C_HEADER_LEN)) {
        lwsl_warn("the server event packet is too big\n");
        return -1;
    }

    buf_info = next_buf_info(&pbuf->list_todo);

    list_for_each_entry(tcp_client, &tcp_server->list_client, node) {
        /* TODO do better for this */
        header->magic = htonl(HTTP_C_MAGIC);
        header->version = HTTP_C_VERSION;
        header->type = HTTP_C_REQ;
        header->length = htonl(pbuf->len + HTTP_C_HEADER_LEN);
        header->seq = htons(tcp_client->req_seq & 0xFF);     /* Low 8 Bytes is client seq */
        header->reserved = 0;

        new_buf = alloc_buf(mgmt);
        memcpy(new_buf->buf, header, HTTP_C_HEADER_LEN);
        memcpy(new_buf->buf+HTTP_C_HEADER_LEN, buf_info->buf, buf_info->len);
        new_buf->start = 0;
        new_buf->len = buf_info->len + HTTP_C_HEADER_LEN;
        new_buf->total_len = new_buf->len;

        list_add(&new_buf->node, &tcp_client->buf_write.list_todo);

        if(tcp_client->idle) {
            /* Switch to write state and run */
            memset(&tcp_client->context, 0, sizeof(tcp_client->context));
            tcp_client->pfd->events |= POLLOUT;
            tcp_client->pfd->events &= ~POLLIN;
            tcp_client->func_run = &tcp_client_write;
            tcp_client->status = CALLING_READY;
            tcp_client->idle = 0;
        }
    }

    release_prepare(mgmt);
    return 0;
}

int process_tunnel_resp(http_mgmt* mgmt)
{
    http_c_header h, *header;
    http_buf_info* buf_info;
    tcp_client_context* tcp_client = NULL;
    http_buf* pbuf = &mgmt->buf_prepare;
    tcp_server_context* tcp_server = &mgmt->tcp_server;
    header = &h;

    list_for_each_entry(tcp_client, &tcp_server->list_client, node) {
        if(pbuf->seq == tcp_client->req_seq) {
            break;
        }
    }

    if((NULL == tcp_client)
            || (&tcp_server->list_client == &tcp_client->node)) {
        /* Not found */
        return -1;
    }

    header->magic = htonl(HTTP_C_MAGIC);
    header->version = HTTP_C_VERSION;
    header->type = HTTP_C_RESP;
    header->length = htonl(pbuf->len + HTTP_C_HEADER_LEN);
    header->seq = htons(tcp_client->req_seq & 0xFF);     /* Low 8 Bytes is client seq */
    header->reserved = 0;

    buf_info = alloc_buf(mgmt);
    memcpy(buf_info->buf, header, HTTP_C_HEADER_LEN);
    buf_info->start = 0;
    buf_info->len = HTTP_C_HEADER_LEN;
    buf_info->total_len = HTTP_C_HEADER_LEN;
    list_add(&buf_info->node, &pbuf->list_todo);

    /* Add list to buf_write */
    list_splice_tail_init(&pbuf->list_todo, &tcp_client->buf_write.list_todo);

    if(tcp_client->idle) {
        /* Switch to write state and run */
        memset(&tcp_client->context, 0, sizeof(tcp_client->context));
        tcp_client->pfd->events |= POLLOUT;
        tcp_client->pfd->events &= ~POLLIN;
        tcp_client->func_run = &tcp_client_write;
        tcp_client->status = CALLING_READY;
        tcp_client->idle = 0;
    }

    return 0;
}

static void tcp_client_save_read(tcp_client_context* tcp_client, http_buf_info* buf_info)
{
    buf_info->len = buf_info->start;
    buf_info->total_len = buf_info->start;
    tcp_client->buf_read.len += buf_info->start;
    buf_info->start = 0;
    list_add_tail(&buf_info->node, &tcp_client->buf_read.list_todo);
    tcp_client->buf_read.curr = NULL;
}

int tcp_client_parse_req(http_mgmt* mgmt, tcp_client_context* tcp_client)
{
    /* Send to server */
    list_splice_tail_init(&tcp_client->buf_read.list_todo, &mgmt->buf_toserver.list_todo);
    tcp_client->buf_read.len = 0;
    tcp_client->buf_read.total_len = 0;
    http_mgmt_toserver(mgmt);

    lwsl_info("client do request to server seq=%d", tcp_client->req_seq);

    return 0;
}

unsigned int hash(unsigned int x)
{
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x);
    return x;
    //hash(i)=i*2654435761 mod 2^32
}

void generate_key(uint32_t* k)
{
    int i = 3, n = 60;
    time_t t;

    time(&t);
    k[i] = (uint32_t)t;
    k[i] = (k[i]+n-1) / n;
    k[i] = hash(k[i]);
}

void encry(uint32_t* v, uint32_t* k)
{
    uint32_t v0=v[0], v1=v[1], sum=0, i;           /* set up */
    uint32_t delta=0x9e3779b9;                     /* a key schedule constant */
    uint32_t k0=k[0], k1=k[1], k2=k[2], k3=k[3];   /* cache key */
    for (i=0; i < 32; i++) {                       /* basic cycle start */
        sum += delta;
        v0 += ((v1<<4) + k0) ^ (v1 + sum) ^ ((v1>>5) + k1);
        v1 += ((v0<<4) + k2) ^ (v0 + sum) ^ ((v0>>5) + k3);
    }                                              /* end cycle */
    v[0]=v0; v[1]=v1;
}

void decry(uint32_t* v, uint32_t* k) {
    uint32_t v0=v[0], v1=v[1], sum=0xC6EF3720, i;  /* set up */
    uint32_t delta=0x9e3779b9;                     /* a key schedule constant */
    uint32_t k0=k[0], k1=k[1], k2=k[2], k3=k[3];   /* cache key */
    for (i=0; i<32; i++) {                         /* basic cycle start */
        v1 -= ((v0<<4) + k2) ^ (v0 + sum) ^ ((v0>>5) + k3);
        v0 -= ((v1<<4) + k0) ^ (v1 + sum) ^ ((v1>>5) + k1);
        sum -= delta;
    }                                              /* end cycle */
    v[0]=v0; v[1]=v1;
}

void encry2(uint32_t* v, int len)
{
    uint32_t k[] = {0x53726438, 0x89742910, 0x47492018, 0x0};
    int i;

    assert(len % 2 == 0);

    generate_key(k);

    for(i = 0; i < len/2; i++)
    {
        encry(v+i*2, k);
    }
}

void decry2(uint32_t* v, int len)
{
    uint32_t k[] = {0x53726438, 0x89742910, 0x47492018, 0x0};
    int i = 0;

    assert(len % 2 == 0);
    generate_key(k);
    fprintf(stderr, "k3 = %d\n", k[3]);

    for(i = 0; i < len/2; i++)
    {
        decry(v+i*2, k);
    }
}

int tcp_client_read(http_mgmt* mgmt, tcp_client_context* tcp_client)
{
    int n, left;
    http_buf_info* buf_info;
    http_c_header h, *header;
    tcp_server_context* tcp_server = &mgmt->tcp_server;
    header = &h;

    /* Set to busy first */
    tcp_client->idle = 0;
    buf_info = tcp_client->buf_read.curr;

    lwsl_info("tcp_client_read\n");

    ccrBegin(tcp_client);

    for(;;)
    {
        buf_info = tcp_client->buf_read.curr;
        if(NULL == buf_info) {
            buf_info = alloc_buf(mgmt);
            tcp_client->buf_read.curr = buf_info;
        }

        left = HTTP_BUF_SIZE - buf_info->start;
        assert(left > 0);

        n = read(tcp_client->client_fd, buf_info->buf+buf_info->start, left);
        if(n > 0) {
            buf_info->start += n;
            if(buf_info->start >= HTTP_BUF_SIZE) {
                tcp_client_save_read(tcp_client, buf_info);
                buf_info = tcp_client->buf_read.curr;
            }
            lwsl_info("Tcp client got n=%d\n", n);

            if((!tcp_client->header_parsed)
                    && (buf_info->start >= HTTP_C_HEADER_LEN)) {
                /* TODO Parse header */
                decry2((uint32_t*)buf_info->buf, HTTP_C_HEADER_LEN/4);

                memcpy(header, buf_info->buf, HTTP_C_HEADER_LEN);
                tcp_server->seq = (tcp_server->seq+1) & 0xFF;
                tcp_client->req_seq = (ntohs(header->seq) | (tcp_server->seq<<8));
                header->seq = htons(tcp_client->req_seq);
                header->type = HTTP_C_TUNNELREQ;
                memcpy(buf_info->buf, header, HTTP_C_HEADER_LEN);
                tcp_client->buf_read.total_len = ntohl(header->length);
                tcp_client->header_parsed = 1;
            }

            assert(tcp_client->buf_read.total_len % 8 == 0);

            if((tcp_client->buf_read.len + buf_info->start)
                    >= tcp_client->buf_read.total_len) {

                decry2((uint32_t*)(buf_info->buf+HTTP_C_HEADER_LEN)
                        , (tcp_client->buf_read.total_len-HTTP_C_HEADER_LEN)/4);

                tcp_client_save_read(tcp_client, buf_info);
                buf_info = tcp_client->buf_read.curr;

                //OK, Parse the packet and send buf to server
                list_splice_tail_init(&tcp_client->buf_read.list_todo, &mgmt->buf_toserver.list_todo);
                tcp_client->buf_read.len = 0;
                tcp_client->buf_read.total_len = 0;
                tcp_client->header_parsed = 0;
                http_mgmt_toserver(mgmt);

                /* Set to idle state */
                tcp_client->idle = 1;
            }
        }
        else if((!n) || ((errno != EINTR) && (errno != EAGAIN))) {
            // Read finished
            if(0 == buf_info->start) {
                free_buf(mgmt, buf_info);
                buf_info = NULL;
                tcp_client->buf_read.curr = NULL;
                lwsl_notice("tcp client read finished but got null buf\n");
            }
            else {
                tcp_client_save_read(tcp_client, buf_info);
                buf_info = tcp_client->buf_read.curr;
            }
            lwsl_warn("The tcp client has closed\n");
            ccrReturn(tcp_client, CALLER_FINISH);
        }
        /* At last, just pending */
        ccrReturn(tcp_client, CALLER_PENDING);
    }

    ccrFinish(tcp_client, CALLER_PENDING);
}

int tcp_client_write(http_mgmt* mgmt, tcp_client_context* tcp_client)
{
    int n = 0;
    http_buf_info* buf_info = tcp_client->buf_write.curr;
    struct pollfd* pfd = tcp_client->pfd;

    /* Set to busy */
    tcp_client->idle = 0;

    lwsl_info("tcp_client_write\n");

    ccrBegin(tcp_client);

    /* Only run once */
    tcp_client->buf_write.curr = next_buf_info(&tcp_client->buf_write.list_todo);
    buf_info = tcp_client->buf_write.curr;


    while(buf_info != NULL)
    {
        buf_info->len = ((buf_info->len+7)/8) * 8;
        encry2((uint32_t*)buf_info->buf, buf_info->len/4);

        n = write(tcp_client->client_fd, buf_info->buf+buf_info->start, buf_info->len);
        assert((n == buf_info->len) && (buf_info->start == 0));

        if(n < 0) {
            if(errno == EINTR || errno == EAGAIN) {
                ccrReturn(tcp_client, CALLER_PENDING);
            }
            else {
                lwsl_warn("tcp client write sock error\n");
                ccrReturn(tcp_client, CALLER_FINISH);
            }
        }

        if((buf_info->len -= n) > 0) {
            buf_info->start += n;
            ccrReturn(tcp_client, CALLER_PENDING);
        } else {
            /* Finished write */
            list_del(&buf_info->node);
            free_buf(mgmt, buf_info);
            tcp_client->buf_write.curr = next_buf_info(&tcp_client->buf_write.list_todo);
            buf_info = tcp_client->buf_write.curr;
        }
    }

    /* Write complete, switch to idle and wait for read */
    pfd->events &= ~POLLOUT;
    pfd->events |= POLLIN;
    tcp_client->func_run = &tcp_client_read;
    memset(&tcp_client->context, 0, sizeof(tcp_client->context));
    tcp_client->idle = 1;
    ccrFinish(tcp_client, CALLER_PENDING);
}

int tcp_server_callback(http_mgmt* mgmt, tcp_server_context* tcp_server)
{
    int optval;
    socklen_t clilen;
    int clientfd = -1;
    struct pollfd* pfd;
    struct sockaddr_in cli_addr;
    tcp_client_context* tcp_client = NULL;

    pfd = tcp_server->pfd;

    ccrBegin(tcp_server);

    for(;;)
    {
        clilen = sizeof(cli_addr);
        clientfd = accept(tcp_server->serv_fd, (struct sockaddr*)&cli_addr, &clilen);
        if(clientfd < 0) {
            if(errno == EINTR || errno == EAGAIN) {
                ccrReturn(tcp_server, CALLER_CONTINUE);
            }
        }

        /* Now create client. TODO change it to a function? */
        tcp_client = calloc(1, sizeof(tcp_client_context));
        if(NULL == tcp_client) {
            lwsl_warn("out of memory to create tcp_client");
            close(clientfd);
            ccrReturn(tcp_server, CALLER_PENDING);
        }

        //setsockopt(clientfd, SOL_TCP, TCP_NODELAY
        //        , (const void *)&optval, sizeof(optval));

        setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY
                , (const void *)&optval, sizeof(optval));
        fcntl(clientfd, F_SETFL, O_NONBLOCK);

        tcp_client->client_fd = clientfd;
        tcp_client->context_type = CONTEXT_TYPE_TCPCLIENT;
        tcp_client->func_run = &tcp_client_read;
        tcp_client->status = CALLING_PENDING;/* Wait for stream */
        tcp_client->idle = 1;/* Now we in idle state, can switch to writable directly */
        INIT_LIST_HEAD(&tcp_client->buf_read.list_todo);
        INIT_LIST_HEAD(&tcp_client->buf_write.list_todo);
        time(&tcp_client->timeout);
        list_add(&tcp_client->node, &tcp_server->list_client);
        tcp_client->pfd = mgmt_add_fd(mgmt, clientfd, (POLLIN|POLLERR|POLLHUP));
        tcp_server->client_len++;
        mgmt->http_lookup[clientfd] = (common_context*)tcp_client;

        lwsl_info("got a client client_len=%d\n", tcp_server->client_len);

        ccrReturn(tcp_server, CALLER_PENDING);
    }
    ccrFinish(tcp_server, CALLER_PENDING);
}

int tcp_client_release(http_mgmt* mgmt, tcp_client_context* tcp_client)
{
    tcp_server_context* tcp_server = &mgmt->tcp_server;
    list_del(&tcp_client->node);
    if(tcp_client->client_fd > 0) {
        mgmt_del_fd(mgmt, tcp_client->client_fd);
        mgmt->http_lookup[tcp_client->client_fd] = NULL;
        close(tcp_client->client_fd);
    }

    if(NULL != tcp_client->buf_read.curr) {
        free_buf(mgmt, tcp_client->buf_read.curr);
    }
    list_splice(&tcp_client->buf_read.list_todo, &mgmt->http_buf_caches);

    if(NULL != tcp_client->buf_write.curr) {
        free_buf(mgmt, tcp_client->buf_write.curr);
    }
    list_splice(&tcp_client->buf_write.list_todo, &mgmt->http_buf_caches);

    tcp_server->client_len--;
    free(tcp_client);

    lwsl_info("close a tcp client, client_len=%d\n", tcp_server->client_len);

    return 0;
}

#if 0
int tcp_server_init(http_mgmt* mgmt, tcp_server_context* tcp_server)
{
    int n, optval, listenfd;
    struct sockaddr_in server_addr;

    tcp_server->context_type = CONTEXT_TYPE_TCPSERVER;
    tcp_server->port = 5566;
    INIT_LIST_HEAD(&tcp_server->list_client);
    tcp_server->func_run = &tcp_server_callback;
    tcp_server->status = CALLING_PENDING;   /* Wait for client to connect */

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(tcp_server->port);
    server_addr.sin_addr.s_addr = name_resolve("127.0.0.1");
    n = bind(listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if(n < 0) {
        lwsl_err("create mgmt server error\n");
        return -1;
    }
    listen(listenfd, 10);
    lwsl_info("server listening \n");

    setsockopt(listenfd, SOL_TCP, TCP_NODELAY
            , (const void *)&optval, sizeof(optval));
    fcntl(listenfd, F_SETFL, O_NONBLOCK);
    tcp_server->serv_fd = listenfd;
    tcp_server->pfd = mgmt_add_fd(mgmt, listenfd, (POLLIN | POLLERR | POLLHUP));
    mgmt->http_lookup[listenfd] = (common_context*)tcp_server;


    return 0;
}
#endif

int tcp_server_init(http_mgmt* mgmt, tcp_server_context* tcp_server)
{
    int n, optval, socket_len, listenfd;
    struct sockaddr_un socket_un;

    tcp_server->context_type = CONTEXT_TYPE_TCPSERVER;
    strcpy(tcp_server->sock_path, "/tmp/ss-mgmt.sock");
    INIT_LIST_HEAD(&tcp_server->list_client);
    tcp_server->func_run = &tcp_server_callback;
    tcp_server->status = CALLING_PENDING;   /* Wait for client to connect */

    unlink(tcp_server->sock_path);
    listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(-1 == listenfd) {
        lwsl_err("create mgmt server socket error\n");
        return -1;
    }
    socket_un.sun_family = AF_UNIX;
    strcpy(socket_un.sun_path, tcp_server->sock_path);
    socket_len = sizeof(socket_un.sun_family) + strlen(tcp_server->sock_path);

    n = bind(listenfd, (struct sockaddr*)&socket_un, socket_len);
    if(n < 0) {
        lwsl_err("create mgmt server error\n");
        return -1;
    }
    listen(listenfd, 10);
    lwsl_info("server listening \n");

    setsockopt(listenfd, SOL_TCP, TCP_NODELAY
            , (const void *)&optval, sizeof(optval));
    fcntl(listenfd, F_SETFL, O_NONBLOCK);
    tcp_server->serv_fd = listenfd;
    tcp_server->pfd = mgmt_add_fd(mgmt, listenfd, (POLLIN | POLLERR | POLLHUP));
    mgmt->http_lookup[listenfd] = (common_context*)tcp_server;


    return 0;
}

int tcp_server_run(http_mgmt* mgmt)
{
    tcp_server_context* tcp_server = &mgmt->tcp_server;
    tcp_client_context* tcp_client;

    list_for_each_entry(tcp_client, &tcp_server->list_client, node)
    {
        if(CALLING_READY == tcp_client->status)
        {
            if(CALLER_CONTINUE == (*tcp_client->func_run)(mgmt, tcp_client)) {
                tcp_client->status = CALLING_READY;
            }
            else {
                tcp_client->status = CALLING_PENDING;
            }
        }
    }

    if(CALLING_READY == tcp_server->status)
    {
        if(CALLER_CONTINUE == (*tcp_server->func_run)(mgmt, tcp_server)) {
            tcp_server->status = CALLING_READY;
        }
        else {
            tcp_server->status = CALLING_PENDING;
        }
    }

    return 0;
}

int process_tcp_forward(http_mgmt* mgmt)
{
    tcp_forward_context* tcp_forward;
    unsigned short seq, port;
    unsigned int type;
    int the_len = 8;
    char host[128];

    http_buf_info* buf_info;
    http_buf* pbuf = &mgmt->buf_prepare;

    /* The header is parsed */
    if(pbuf->len < the_len) {
        lwsl_warn("process packet forward, packet length not ok len=%d\n", pbuf->len);
        return -1;
    }

    buf_info = next_buf_info(&pbuf->list_todo);
    memcpy(&seq, buf_info->buf, sizeof(unsigned short));
    memcpy(&port, buf_info->buf + sizeof(unsigned short), sizeof(unsigned short));
    memcpy(&type, buf_info->buf+4, sizeof(unsigned int));
    seq = htons(seq);
    port = htons(port);
    type = htonl(type);

    if(0 == type) {
        /* Now create tcp_forward */
        memcpy(host, buf_info->buf+8, buf_info->len-8);
        host[buf_info->len-8] = '\0';
        host[buf_info->len-7] = '\0';
        tcp_forward_create(mgmt, seq, host, port);
    }
    else {
        /* Delete the connection */
        list_for_each_entry(tcp_forward, &mgmt->list_forward, node) {
            if(pbuf->seq == tcp_forward->seq) {
                break;
            }
        }
        if((NULL == tcp_forward)
                || (&mgmt->list_forward == &tcp_forward->node)) {
            /* Not found */
            return -1;
        }

        tcp_forward_release(mgmt, tcp_forward);
    }

    release_prepare(mgmt);
    return 0;
}

int process_tcp_forwardresp(http_mgmt *mgmt)
{
    tcp_forward_context* tcp_forward = NULL;
    http_buf* pbuf = &mgmt->buf_prepare;

    list_for_each_entry(tcp_forward, &mgmt->list_forward, node) {
        if(pbuf->seq == tcp_forward->seq) {
            break;
        }
    }
    if((NULL == tcp_forward)
            || (&mgmt->list_forward == &tcp_forward->node)) {
        /* Not found */
        return -1;
    }

    list_splice_tail_init(&pbuf->list_todo, &tcp_forward->buf_write.list_todo);
    assert(NULL == pbuf->curr);
    release_prepare(mgmt);  //Reset the buf_prepare

    if(tcp_forward->idle) {
        //Change to write state
        tcp_forward->func_run = &tcp_forward_write;
        tcp_forward->pfd->events |= POLLOUT;
        tcp_forward->pfd->events &= ~POLLIN;
        memset(&tcp_forward->context, 0, sizeof(tcp_forward->context));
    }

    return 0;
}

int tcp_forward_write(http_mgmt* mgmt, tcp_forward_context* tcp_forward)
{
    int n = 0;
    http_buf_info* buf_info = tcp_forward->buf_write.curr;
    struct pollfd* pfd = tcp_forward->pfd;

    /* Set to busy */
    tcp_forward->idle = 0;

    lwsl_info("tcp_forward_write\n");

    ccrBegin(tcp_forward);

    /* Only run once */
    tcp_forward->buf_write.curr = next_buf_info(&tcp_forward->buf_write.list_todo);
    buf_info = tcp_forward->buf_write.curr;


    while(buf_info != NULL)
    {
        n = write(tcp_forward->fwd_fd, buf_info->buf + buf_info->start, buf_info->len);
        lwsl_info("tcp_forward_writed n = %d\n", n);

        if(n < 0) {
            if(errno == EINTR || errno == EAGAIN) {
                lwsl_warn("tcp forward have to write buf again\n");
                ccrReturn(tcp_forward, CALLER_PENDING);
            }
            else {
                lwsl_warn("tcp forward write sock error\n");
                ccrReturn(tcp_forward, CALLER_FINISH);
            }
        }
        else if((!n) && ((errno != EINTR) && (errno != EAGAIN))) {
            lwsl_warn("The tcp forward has closed\n");
            ccrReturn(tcp_forward, CALLER_FINISH);
        }

        if((buf_info->len -= n) > 0) {
            buf_info->start += n;
            ccrReturn(tcp_forward, CALLER_PENDING);
        } else {
            /* Finished write */
            list_del(&buf_info->node);
            free_buf(mgmt, buf_info);
            tcp_forward->buf_write.curr = next_buf_info(&tcp_forward->buf_write.list_todo);
            buf_info = tcp_forward->buf_write.curr;
        }
    }

    /* Write complete, switch to idle and wait for read */
    lwsl_info("write complete\n");

    pfd->events &= ~POLLOUT;
    pfd->events |= POLLIN;
    tcp_forward->func_run = &tcp_forward_read;
    memset(&tcp_forward->context, 0, sizeof(tcp_forward->context));
    tcp_forward->idle = 1;
    ccrFinish(tcp_forward, CALLER_PENDING);
}

// read and forward to websocket
int tcp_forward_read(http_mgmt* mgmt, tcp_forward_context* tcp_forward)
{
    http_c_header *header;
    int n, left, header_len = HTTP_C_HEADER_LEN + 4;
    CALLER_STATUS status = CALLER_PENDING;
    http_buf_info* buf_info = NULL;

    /* Set to busy first */
    tcp_forward->idle = 0;
    lwsl_info("tcp_forward_read\n");

    do {
        buf_info = alloc_buf(mgmt);
        if(NULL == buf_info) {
            lwsl_warn("tcp_forward_read not buf_info\n");
            break;
        }
        header = (http_c_header*)buf_info->buf;
        left = HTTP_BUF_SIZE - header_len;

        n = read(tcp_forward->fwd_fd, buf_info->buf + header_len, left);
        if(n > 0) {
            lwsl_info("Tcp client got n=%d, forwarding to websocket server\n", n);

            //Set the length
            *((uint32_t*)(buf_info->buf + HTTP_C_HEADER_LEN)) = htonl(n);

            buf_info->start = 0;
            buf_info->len = n + header_len;
            //Set the min package size
            if(buf_info->len < WEBSOCK_MIN_PACK_SIZE) {
                memset(buf_info->buf + n + header_len
                        , '\0', WEBSOCK_MIN_PACK_SIZE - buf_info->len);
                buf_info->len = WEBSOCK_MIN_PACK_SIZE;
            }
            buf_info->total_len = buf_info->len;

            header->magic = htonl(HTTP_C_MAGIC);
            header->version = HTTP_C_VERSION;
            header->type = HTTP_C_FORWARD_RESP;
            header->seq = htons(tcp_forward->seq);
            header->length = htonl(buf_info->len);
            header->reserved = 0;

            list_add_tail(&buf_info->node, &mgmt->buf_toserver.list_todo);
            buf_info = NULL;    //Set to null, so it will not be free
            http_mgmt_toserver(mgmt);

            //check if we need to change to write state
            if(!list_empty(&tcp_forward->buf_write.list_todo)) {
                tcp_forward->func_run = &tcp_forward_write;
                tcp_forward->pfd->events |= POLLOUT;
                tcp_forward->pfd->events &= ~POLLIN;
                memset(&tcp_forward->context, 0, sizeof(tcp_forward->context));
            }
        }
        else if((!n) || ((errno != EINTR) && (errno != EAGAIN))) {
            // Read finished
            lwsl_warn("The tcp forward has closed\n");
            status = CALLER_FINISH;
        }
    } while(0);

    if(NULL != buf_info) {
        free_buf(mgmt, buf_info);
    }

    tcp_forward->idle = 1;

    return status;
}

int tcp_forward_closing(http_mgmt* mgmt, tcp_forward_context* tcp_forward) {
    http_c_header* header;
    int header_len = HTTP_C_HEADER_LEN + 4;
    http_buf_info* buf_info = NULL;
    char* failed = "CLOSED!";

    buf_info = alloc_buf(mgmt);
    if(NULL == buf_info) {
        lwsl_warn("%s: cannot alloc buf ", __FUNCTION__);
        return -1;
    }
    buf_info->start = 0;
    buf_info->len = WEBSOCK_MIN_PACK_SIZE;
    buf_info->total_len = buf_info->len;

    header = (http_c_header*)buf_info->buf;
    header->magic = htonl(HTTP_C_MAGIC);
    header->version = HTTP_C_VERSION;
    header->type = HTTP_C_FORWARD_RESP;
    header->seq = htons(tcp_forward->seq);
    header->length = htonl(buf_info->len);
    header->reserved = 0;

    memset(buf_info->buf+header_len
            , '\0', WEBSOCK_MIN_PACK_SIZE - header_len);
    memset(buf_info->buf+header_len, '\0'
            , WEBSOCK_MIN_PACK_SIZE - header_len);
    strcpy(buf_info->buf + header_len, failed);
    *((uint32_t*)(buf_info->buf + HTTP_C_HEADER_LEN)) = htonl(strlen(failed)+1);

    list_add_tail(&buf_info->node, &mgmt->buf_toserver.list_todo);
    http_mgmt_toserver(mgmt);

    lwsl_info(" inform to server that forward failed\n ");
    return 0;
}

int tcp_forward_connect(http_mgmt* mgmt, tcp_forward_context* tcp_forward)
{
    int rc;
    struct sockaddr_in server_addr;
    struct pollfd* pfd;

    pfd = (struct pollfd*)tcp_forward->pfd;

    ccrBegin(tcp_forward);

    while(POLLOUT != pfd->revents)
    {
        memset(&server_addr, 0, sizeof(struct sockaddr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(tcp_forward->port);
        //server_addr.sin_addr.s_addr = name_resolve(mgmt->local_host);
        server_addr.sin_addr.s_addr = tcp_forward->host;
        lwsl_notice("tcp_forward before connect ctx->seq=%d\n", tcp_forward->seq);
        rc = connect(tcp_forward->fwd_fd, (struct sockaddr*) &server_addr, sizeof(struct sockaddr));
        if(rc >= 0)
        {
            break;
        }

        if((EALREADY == errno) || (EINPROGRESS == errno))
        {
            pfd->events &= ~POLLIN;
            pfd->events |= POLLOUT;
            lwsl_notice("tcp_forward still connecting ctx->seq=%d\n", tcp_forward->seq);
            ccrReturn(tcp_forward, CALLER_CONTINUE);
        }
        else
        {
            lwsl_warn("connect fail\n");
            tcp_forward->errcode = 3; //connected fail
            ccrReturn(tcp_forward, CALLER_FINISH);
        }
    }
    lwsl_info("forward connected ok\n");

    // Clear pollout flag
    pfd->events &= ~POLLOUT;
    pfd->events |= POLLIN;
    tcp_forward->func_run = &tcp_forward_read;
    memset(&tcp_forward->context, 0, sizeof(tcp_forward->context));
    tcp_forward->idle = 1;
    ccrFinish(tcp_forward, CALLER_PENDING);
}

int tcp_forward_create(http_mgmt* mgmt, uint16_t seq, char* host, uint16_t port)
{
    int sockfd, rc = 0, optval = 0;
    tcp_forward_context* tcp_forward;
    tcp_forward = calloc(1, sizeof(tcp_forward_context));
    if(NULL == tcp_forward) {
        lwsl_warn(" tcp_forward created failed\n");
        return -1;
    }

    do {
        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            lwsl_err("create tcp_forward socket error\n");
            rc = -1;    // Error
            break;
        }

        //INIT_LIST_HEAD(&tcp_forward->node);
        tcp_forward->context_type = CONTEXT_TYPE_FORWARD;
        tcp_forward->seq = seq;
        tcp_forward->host = name_resolve(host);
        tcp_forward->port = port;

        setsockopt(sockfd, SOL_TCP, TCP_NODELAY, (const void *)&optval, sizeof(optval));
        fcntl(sockfd, F_SETFL, O_NONBLOCK);
        tcp_forward->fwd_fd = sockfd;

        tcp_forward->idle = 1;
        //INIT_LIST_HEAD(&tcp_forward->buf_read.list_todo);
        INIT_LIST_HEAD(&tcp_forward->buf_write.list_todo);
        tcp_forward->pfd = mgmt_add_fd(mgmt, sockfd, (POLLIN | POLLERR | POLLHUP) );
        mgmt->http_lookup[tcp_forward->fwd_fd] = (common_context*)tcp_forward;

        tcp_forward->func_run = &tcp_forward_connect;
        tcp_forward->status = CALLING_READY;

        list_add(&tcp_forward->node, &mgmt->list_forward);
        mgmt->len_forward++;

        lwsl_info("created forward to %s:%d\n", host, port);
    } while(0);

    if(0 != rc) {
        //TODO use tcp_forward_release ?
        free(tcp_forward);
    }

    return rc;
}

int tcp_forward_release(http_mgmt* mgmt, tcp_forward_context* tcp_forward)
{
    assert(NULL != tcp_forward);

    // First inform the server that the client is deleted
    tcp_forward_closing(mgmt, tcp_forward);

    list_del(&tcp_forward->node);

    lwsl_info("delete tcp_forward seq=%d \n", tcp_forward->seq);

    if(tcp_forward->fwd_fd > 0) {
        mgmt_del_fd(mgmt, tcp_forward->fwd_fd);
        mgmt->http_lookup[tcp_forward->fwd_fd] = NULL;
        close(tcp_forward->fwd_fd);
    }

    /* if(NULL != tcp_forward->buf_read.curr) {
        free_buf(mgmt, tcp_forward->buf_read.curr);
    }
    list_splice(&tcp_forward->buf_read.list_todo, &mgmt->http_buf_caches); */

    if(NULL != tcp_forward->buf_write.curr) {
        free_buf(mgmt, tcp_forward->buf_write.curr);
    }
    list_splice(&tcp_forward->buf_write.list_todo, &mgmt->http_buf_caches);

    free(tcp_forward);
    mgmt->len_forward--;
    return 0;
}

