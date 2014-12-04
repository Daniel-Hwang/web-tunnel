#ifndef _HTTP_CLIENT_H__
#define _HTTP_CLIENT_H__

#include "list.h"
#include "coroutine.h"

#define CALLER_CACHES   256
#define HTTPBUF_CACHES  600
#define HTTP_BUF_SIZE   2048
#define HTTP_CHUNK_MAX  32
#define HTTP_IN_MAX     20480

#define HTTP_C_MAGIC        0x10293874
#define HTTP_C_VERSION      0x1

#define HTTP_C_REQ          0x1
#define HTTP_C_RESP         0x2
#define HTTP_C_HAND         0x3
#define HTTP_C_SYNC         0x4
#define HTTP_C_TUNNELREQ    0x5
#define HTTP_C_TUNNELRESP   0x6

#define HTTP_C_FORWARD        0x10
#define HTTP_C_FORWARD_REQ    0x11
#define HTTP_C_FORWARD_RESP   0x12
//#define HTTP_C_FORWARD_CLOSING   0x13

#define HTTP_C_HEADER_LEN   sizeof(http_c_header)
#define WEBSOCK_MIN_PACK_SIZE 40

#define HTTP_C_EVT_AUTH     0x88999966
#define HTTP_C_EVT_RESET    0x88999988
#define HTTP_C_EVT_FAIL     0x88999998

#define CONTEXT_TYPE_HTTP       0x1
#define CONTEXT_TYPE_TCPSERVER  0x2
#define CONTEXT_TYPE_TCPCLIENT  0x3
#define CONTEXT_TYPE_FORWARD    0x4

#define HTTP_TIMEOUT_SECS   8

typedef struct _http_c_header {
    unsigned int    magic;
    unsigned char  version;
    unsigned char  type;
    unsigned short  seq;
    unsigned int  length;     /* Length include the header */
    unsigned int    reserved;   /* the first byte is use to identify TODO */
}__attribute__ ((packed)) http_c_header;

struct _http_context;
struct _tcp_server_context;
struct _tcp_client_context;
struct _tcp_forward_context;
struct _http_mgmt;
typedef int (*ASYNC_FUNC)(struct _http_mgmt*, struct _http_context*);
typedef int (*TCP_SERV_FUNC)(struct _http_mgmt*, struct _tcp_server_context*);
typedef int (*TCP_CLIENT_FUNC)(struct _http_mgmt*, struct _tcp_client_context*);
typedef int (*TCP_FORWARD_FUNC)(struct _http_mgmt*, struct _tcp_forward_context*);

typedef enum _CALLING_STATUS {
    CALLING_READY = 0,
    CALLING_PENDING,
    CALLING_FINISH
} CALLING_STATUS;

typedef enum _CALLER_STATUS
{
    CALLER_FINISH = 0,
    CALLER_PENDING,
    CALLER_CONTINUE

} CALLER_STATUS;

typedef enum _CALLING_PRIO {
    CALLING_PRIO_HIGH = 0,
    CALLING_PRIO_LOW,
    CALLING_PRIO_COUNT
} CALLING_PRIO;

typedef enum _CALLER_STATE {
    CALLER_STATE_CONNECT = 0,
    CALLER_STATE_WRITE,
    CALLER_STATE_READ
} CALLER_STATE;

typedef struct _http_buf_info {
    struct list_head    node;
    // Because of websocket
    char                real_buf[LWS_SEND_BUFFER_PRE_PADDING + HTTP_BUF_SIZE + LWS_SEND_BUFFER_POST_PADDING + 8];
    char*               buf;
    int                 start;
    int                 len;
    int                 total_len;
} http_buf_info;

typedef struct _http_buf {
    struct list_head    list_todo;
    http_buf_info*      curr;       //The current node is always in list_todo
    int                 len;
    int                 total_len;
    int                 type;
    unsigned short      seq;
} http_buf;

/* TODO do better for it */
typedef struct _common_context {
    struct ccrContextTag    context;
    int                     context_type;
} common_context;

typedef struct _http_context {
    struct ccrContextTag    context;
    int                     context_type;

    struct list_head        node;
    struct list_head        node_time;
    CALLING_STATUS          status;
    CALLER_STATE            state;
    ASYNC_FUNC              func_run;
    int                     prio;
    void*                   args;
    time_t                  timeout;
    int                     errcode;

    int             is_chunk;

    struct pollfd*          pfd;
    int                     sockfd;
    char                    hostname[128];
    short                   port;
    int                     revents;

    unsigned short          seq;
    http_buf                buf_read;
    http_buf                buf_write;
} http_context;

typedef struct _http_param {
    time_t                  timeout;
    http_buf*               pbuf;
} http_param;

typedef struct _tcp_client_context {
    struct ccrContextTag    context;
    int                     context_type;

    struct list_head        node;
    CALLING_STATUS          status;
    TCP_CLIENT_FUNC         func_run;
    time_t                  timeout;
    int                     errcode;

    struct pollfd*          pfd;
    int                     client_fd;
    http_buf                buf_read;
    http_buf                buf_write;
    int                     req_seq;
    int                     idle;

    int                     header_parsed;
} tcp_client_context;

typedef struct _tcp_server_context {
    struct ccrContextTag    context;
    int                     context_type;

    struct list_head        list_client;
    int                     client_len;
    CALLING_STATUS          status;
    TCP_SERV_FUNC           func_run;

    struct pollfd*          pfd;
    int                     serv_fd;
    int                     port;
    int                     seq;
    char                    sock_path[128];
} tcp_server_context;

typedef struct _tcp_forward_context {
    struct ccrContextTag    context;
    int                     context_type;

    struct list_head        node;
    CALLING_STATUS          status;
    TCP_FORWARD_FUNC        func_run;

    //http_buf                buf_read;
    http_buf                buf_write;
    int                     idle;
    //int           is_writing;
    int             errcode;

    struct pollfd*          pfd;
    int                     fwd_fd;
    uint32_t        host;
    int             port;
    int             seq;
} tcp_forward_context;

typedef struct _http_mgmt {
    struct list_head        list_ready[CALLING_PRIO_COUNT];
    int                     ready_len;
    struct list_head        list_timeout;

    int                     total_add;
    int                     total_process;

    int                     max_fds;
    struct pollfd*          pollfds;
    int*                    fd_lookup;
    int                     count_pollfds;
    common_context**        http_lookup;

    http_buf                buf_prepare;
    int                     sync_prepare;
    time_t                  sync_time;

    time_t                  last_alive_time;

    http_buf                buf_toserver;
    int                     toserver;

    int                     shakehand;
    int                     client_id;

    char            username[128];
    char            remote_host[128];
    int             remote_port;
    char            local_host[128];
    int             local_port;

    tcp_server_context      tcp_server;
    struct list_head        list_forward;
    int             len_forward;

    struct list_head        ctx_caches;
    struct list_head        http_buf_caches;
} http_mgmt;

int http_mgmt_init(http_mgmt* mgmt);
int http_mgmt_param_init(http_mgmt* mgmt);
int http_mgmt_handshake(http_mgmt* mgmt);
int http_mgmt_isshake(http_mgmt* mgmt);
int mgmt_del_fd(http_mgmt* mgmt, int fd);
struct pollfd* mgmt_add_fd(http_mgmt* mgmt, int fd, int events);
int http_mgmt_service(http_mgmt* mgmt, struct pollfd* pfd);
int http_mgmt_run_timeout(http_mgmt* mgmt);
int http_mgmt_run(http_mgmt* mgmt);
int http_mgmt_prepare(http_mgmt* mgmt, char* buf, int len);
int http_context_create(http_mgmt* mgmt, http_param* param);
int http_mgmt_writable(void* this, void* wsi, http_mgmt* mgmt);
int http_mgmt_release_all(http_mgmt* mgmt);

extern int websocket_go_writable();
extern int websocket_write(void* wsi, char *buf, size_t len);
extern int websocket_write_again(void* context, void* wsi);
extern int websocket_closed();
#endif

