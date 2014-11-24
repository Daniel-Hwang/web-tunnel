/*
 * libwebsockets-test-client - libwebsockets test implementation *
 * Copyright (C) 2011 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>

#ifdef CMAKE_BUILD
#include "lws_config.h"
#endif

#include "../lib/libwebsockets.h"

#include "http_client.h"

static http_mgmt    g_mgmt;
static http_mgmt*   pmgmt;
int was_closed = 0;
int force_exit = 0;

enum demo_protocols {
    /* always first */
    PROTOCOL_HTTP = 0,

    PROTOCOL_DUMB_INCREMENT,

    /* always last */
    DEMO_PROTOCOL_COUNT
};

static int callback_http(struct libwebsocket_context *context,
        struct libwebsocket *wsi,
        enum libwebsocket_callback_reasons reason, void *user,
                               void *in, size_t len)
{
    struct libwebsocket_pollargs *pa = (struct libwebsocket_pollargs *)in;
    switch (reason) {
    case LWS_CALLBACK_LOCK_POLL:
        break;
    case LWS_CALLBACK_UNLOCK_POLL:
        break;
    case LWS_CALLBACK_ADD_POLL_FD:
            if(NULL == mgmt_add_fd(pmgmt, pa->fd, pa->events)) {
                return 1;
            }
            break;

    case LWS_CALLBACK_DEL_POLL_FD:
            mgmt_del_fd(pmgmt, pa->fd);
        break;

    case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
            pmgmt->pollfds[pmgmt->fd_lookup[pa->fd]].events = pa->events;
        break;
        default:
                break;
    }

    return 0;
}



static int
callback_dumb_increment(struct libwebsocket_context *this,
            struct libwebsocket *wsi,
            enum libwebsocket_callback_reasons reason,
                           void *user, void *in, size_t len)
{
    int n = 0;
    switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        libwebsocket_callback_on_writable(this, wsi); // To shakehand
        fprintf(stderr, "callback_dumb_increment: LWS_CALLBACK_CLIENT_ESTABLISHED\n");
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "LWS_CALLBACK_CLIENT_CONNECTION_ERROR\n");
        was_closed = 1;
        break;

    case LWS_CALLBACK_CLOSED:
        fprintf(stderr, "LWS_CALLBACK_CLOSED\n");
        was_closed = 1;
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        http_mgmt_prepare(pmgmt, (char*)in, len);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if(!http_mgmt_isshake(pmgmt)) {
            http_mgmt_handshake(pmgmt);
        }
        n = http_mgmt_writable((void*)this, (void*)wsi, pmgmt);
        break;
    case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
        break;
    default:
        break;
    }

    return n;
}

static struct libwebsocket_protocols protocols[] = {
    {
        "http-only",        /* name */
        callback_http,      /* callback */
        0,  /* per_session_data_size */
        0,          /* max frame size / rx buffer */
    },
    {
        "dumb-increment-protocol",
        callback_dumb_increment,
        0,
        20,
    },
    { NULL, NULL, 0, 0 } /* end */
};

int websocket_go_writable()
{
    libwebsocket_callback_on_writable_all_protocol(
            &protocols[PROTOCOL_DUMB_INCREMENT]);
    return 0;
}

int websocket_write(void* wsi, char *buf, size_t len)
{
    return libwebsocket_write((struct libwebsocket *)wsi,
       (unsigned char*)buf, len, LWS_WRITE_BINARY);
}

int websocket_write_again(void* context, void* wsi)
{
    return libwebsocket_callback_on_writable(
            (struct libwebsocket_context *)context,
            (struct libwebsocket *)wsi);
}

int websocket_closed() {
    was_closed = 1;
    return 0;
}

void sighandler(int sig)
{
    if(sig == SIGINT) {
        force_exit = 1;
    }
    if(sig == SIGUSR1){
        //kill -l; kill -10 pid of web-tunnel
        was_closed = 1;
    }
}

static struct option options[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "debug",      required_argument,      NULL, 'd' },
	{ "lport",	required_argument,	NULL, 'l' },
	{ "rport",	required_argument,	NULL, 'r' },
	{ "ssl",	no_argument,		NULL, 's' },
	{ "pid",	no_argument,		NULL, 'p' },
	{ NULL, 0, 0, 0 }
};

void daemonize(const char* path)
{
#ifndef __MINGW32__
    /* Our process ID and Session ID */
    pid_t pid, sid;

    /* Fork off the parent process */
    pid = fork();
    if (pid < 0)
    {
        exit(EXIT_FAILURE);
    }

    /* If we got a good PID, then
       we can exit the parent process. */
    if (pid > 0)
    {
        FILE *file = fopen(path, "w");
        if (file == NULL) fprintf(stderr, "Invalid pid file\n");

        fprintf(file, "%d", pid);
        fclose(file);
        exit(EXIT_SUCCESS);
    }

    /* Change the file mode mask */
    umask(0);

    /* Open any logs here */

    /* Create a new SID for the child process */
    sid = setsid();
    if (sid < 0)
    {
        /* Log the failure */
        exit(EXIT_FAILURE);
    }

    /* Change the current working directory */
    if ((chdir("/")) < 0)
    {
        /* Log the failure */
        exit(EXIT_FAILURE);
    }

    /* Close out the standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
#endif
}

int main(int argc, char **argv)
{
    int n = 0;
    int ret = 0;
    int rport = 8060;
    int lport = 80;
    int use_ssl = 0;
    char* pid_path = NULL;
//    unsigned int oldus = 0;
    struct libwebsocket_context *context;
    struct libwebsocket *wsi_dumb;
    int ietf_version = -1; /* latest */
    struct lws_context_creation_info info;

    fprintf(stderr, "Remote managerment client\n"
                    "(C) Copyright 2014-2014 Janson <janson.wei@gmail.com> \n");

    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_INFO, NULL);

    while (n >= 0) {
            n = getopt_long(argc, argv, "hspr:l:d:", options, NULL);
            if (n < 0)
                    continue;
            switch (n) {
            case 'd':
                lws_set_log_level(atoi(optarg), NULL);
                break;
            case 's':
                use_ssl = 2; /* 2 = allow selfsigned */
                break;
            case 'r':
                rport = atoi(optarg);
                break;
            case 'l':
                lport = atoi(optarg);
                break;
            case 'p':
                pid_path = "/tmp/web-tunnel.pid";
                break;
            case 'h':
                goto usage;
            }
    }
    if (optind+2 >= argc)
            goto usage;

    pmgmt = &g_mgmt;
    if(0 != http_mgmt_init(pmgmt)) {
        return 1;
    }

    pmgmt->local_port = lport;
    pmgmt->remote_port = rport;
    strcpy(pmgmt->username, argv[optind]);
    strcpy(pmgmt->local_host, argv[optind+1]);
    strcpy(pmgmt->remote_host, argv[optind+2]);
    
    lwsl_info("lport=%d rport=%d username=%s local=%s remote=%s\n"
            , lport, rport, pmgmt->username, pmgmt->local_host, pmgmt->remote_host);

    if(NULL != pid_path) {
        daemonize(pid_path);
    }

    signal(SIGINT|SIGUSR1, sighandler);

reconn:
    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.extensions = libwebsocket_get_internal_extensions();
    info.gid = -1;
    info.uid = -1;

    http_mgmt_param_init(pmgmt);

    context = libwebsocket_create_context(&info);
    if (context == NULL) {
        fprintf(stderr, "Creating libwebsocket context failed\n");
        return 1;
    }

    /* create a client websocket using dumb increment protocol */
    //libwebsocket_close_and_free_session(context,
    //					wsi_mirror, LWS_CLOSE_STATUS_GOINGAWAY);
    wsi_dumb = libwebsocket_client_connect(context, pmgmt->remote_host, pmgmt->remote_port, use_ssl,
            "/", pmgmt->remote_host, pmgmt->remote_host,
             protocols[PROTOCOL_DUMB_INCREMENT].name, ietf_version);

    if (wsi_dumb == NULL) {
        fprintf(stderr, "libwebsocket connect failed\n");
        ret = 1;
        goto done;
    }

    fprintf(stderr, "Waiting for connect...\n");

    /*
     * sit there servicing the websocket context to handle incoming
     * packets, and drawing random circles on the mirror protocol websocket
     * nothing happens until the client websocket connection is
     * asynchronously established
     */

    n = 0;
    while (n >= 0 && !was_closed && !force_exit) {
        http_mgmt_run(pmgmt);

        n = poll(pmgmt->pollfds, pmgmt->count_pollfds, 10);
        if (n < 0) {
            http_mgmt_run_timeout(pmgmt);
            continue;
        }

        if (n)
                for (n = 0; n < pmgmt->count_pollfds; n++)
                    if (pmgmt->pollfds[n].revents) {

                        //TODO do better for this
                        time(&pmgmt->last_alive_time);
                        //lwsl_info("connection still alive\n");

                        if (libwebsocket_service_fd(context,
                                                  &pmgmt->pollfds[n]) < 0) {
                            goto done;
                        }
                        else {
                            http_mgmt_service(pmgmt, &pmgmt->pollfds[n]);
                        }
                    }
    }

done:
#if 0
    if((NULL != wsi_dumb) && (NULL != context)) {
        libwebsocket_close_and_free_session(context,
                                            wsi_dumb, LWS_CLOSE_STATUS_GOINGAWAY);
        wsi_dumb = NULL;
    }
#endif
    if(NULL != context) {
        libwebsocket_context_destroy(context);
    }
    http_mgmt_release_all(pmgmt);

    if(!force_exit) {
        was_closed = 0;
        // Go back to reconn
        lwsl_info("sleep 8s and will reconnect...\n");
        sleep(8);
        goto reconn;
    }

    return ret;

usage:
    fprintf(stderr, "Usage: web-tunnel "
                            "<username> <local> <remote> --rport=<p> --lport=<p> "
                            "[--ssl] [-d <log bitfield>] [--pid]\n");

    return 1;

}
