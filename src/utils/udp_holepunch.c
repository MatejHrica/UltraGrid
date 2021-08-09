/**
 * @file   utils/udp_holepuch.c
 * @author Martin Piatka     <piatka@cesnet.cz>
 */
/*
 * Copyright (c) 2021 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <juice/juice.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "utils/udp_holepunch.h"

#include "debug.h"

#ifdef _WIN32
#include <windows.h>
static void sleep(unsigned int secs) { Sleep(secs * 1000); }
#else
#include <unistd.h> // for sleep
#endif

#define MAX_MSG_LEN 2048
#define MSG_HEADER_LEN 5
#define MOD_NAME "[HOLEPUNCH] "

/* Coordination protocol description
 *
 * The hole punching library requires that clients exchange candidate ip:port
 * pairs. Since the clients cannot yet communicate directly, this information
 * must be sent through a coordination server, which serves as a meeting point
 * for clients that cannot yet communicate directly.
 *
 * Clients connect to the coordination server, identify themselves with a name,
 * and join a "room". Once two clients enter the same room, the coordination
 * server forwards the other clients name, sdp description string, and all
 * candidate address pairs.
 *
 * All communication is done via messages that have the following structure:
 * <HEADER><MSG_BODY>
 * HEADER: 5B string containing length of MSG_BODY, null-termination optional
 * MSG_BODY: content of message, length determined by header, max 2048B
 *
 * After establishing connection to the coordination server, following
 * messages are sent and received in that order:
 * 1. Client sends its name
 * 2. Client sends room name to join
 * 3. Client sends its sdp description
 * 4. Client receives the name of the other client in the room
 * 5. Client receives the sdp description of the other client
 *
 * After that the client sends and receives sdp candidate pairs as they are
 * discovered.
 *
 */

struct Punch_ctx {
        juice_agent_t *juice_agent;

        int coord_sock;
        int local_candidate_port;
};

static void send_msg(int sock, const char *msg){
        size_t msg_size = strlen(msg);
        assert(msg_size < MAX_MSG_LEN);

        char header[MSG_HEADER_LEN];
        memset(header, ' ', sizeof(header));
        snprintf(header, sizeof(header), "%lu", msg_size);

        send(sock, header, sizeof(header), 0);

        send(sock, msg, msg_size, 0);
}

static void on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr) {
        log_msg(LOG_LEVEL_NOTICE, MOD_NAME "Received candidate: %s\n", sdp);
        struct Punch_ctx *ctx = (struct Punch_ctx *) user_ptr;
        send_msg(ctx->coord_sock, sdp);

        /* sdp is a RFC5245 string which should look like this:
         * "a=candidate:2 1 UDP <prio> <ip> <port> typ <type> ..."
         * Since libjuice reports only the external (after NAT translation)
         * receive port, we need to get the receive port number from the local
         * candidate (of type "host").
         */

        char *c = sdp;
        //port is located after 5 space characters
        for(int i = 0; i < 5; i++){
                c = strchr(c, ' ') + 1;
                assert(c);
        }
        char *end;
        int port = strtol(c, &end, 10);
        assert(c != end);
        assert(*end == ' ');
        c = end + 1;

        const char *host_type_str = "typ host";
        if(strncmp(c, host_type_str, strlen(host_type_str)) == 0){
                log_msg(LOG_LEVEL_INFO, MOD_NAME "Local candidate port: %d\n", port);
                ctx->local_candidate_port = port;
        }
}

static juice_agent_t *create_agent(const struct Holepunch_config *c, void *usr_ptr){
        juice_config_t conf;
        memset(&conf, 0, sizeof(conf));

        conf.stun_server_host = c->stun_srv_addr;
        conf.stun_server_port = c->stun_srv_port;

        conf.turn_servers = NULL;
        conf.turn_servers_count = 0;

        conf.cb_candidate = on_candidate;
        conf.user_ptr = usr_ptr;

        return juice_create(&conf);
}


static size_t recv_msg(int sock, char *buf, size_t buf_len){
        char header[MSG_HEADER_LEN + 1];

        int bytes = recv(sock, header, MSG_HEADER_LEN, MSG_WAITALL);
        if(bytes != MSG_HEADER_LEN){
                return 0;
        }
        header[MSG_HEADER_LEN] = '\0';

        unsigned expected_len;
        char *end;
        expected_len = strtol(header, &end, 10);
        if(header == end){
                return 0;
        }

        if(expected_len > buf_len - 1)
                expected_len = buf_len - 1;

        bytes = recv(sock, buf, expected_len, MSG_WAITALL);
        buf[bytes] = '\0';

        return bytes;
}

static bool connect_to_coordinator(const char *coord_srv_addr,
                int coord_srv_port,
                int *sock)
{
        int s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

        struct sockaddr_in sockaddr;
        memset(&sockaddr, 0, sizeof(sockaddr));
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(coord_srv_port);
        struct hostent *host = gethostbyname(coord_srv_addr);
        if(!host){
                error_msg(MOD_NAME "Failed to resolve coordination server host\n");
                return false;
        }

        memcpy(&sockaddr.sin_addr.s_addr, host->h_addr_list[0], host->h_length);
        if (connect(s, (struct sockaddr *) &sockaddr, sizeof(sockaddr)) < 0){
                error_msg(MOD_NAME "Failed to connect to coordination server\n");
                return false;
        }

        *sock = s;
        return true;
}

static bool exchange_coord_desc(juice_agent_t *agent, int coord_sock){
        char sdp[JUICE_MAX_SDP_STRING_LEN];
        juice_get_local_description(agent, sdp, JUICE_MAX_SDP_STRING_LEN);
        log_msg(LOG_LEVEL_VERBOSE, MOD_NAME "Local description:\n%s\n", sdp);

        send_msg(coord_sock, sdp);

        char msg_buf[MAX_MSG_LEN];
        recv_msg(coord_sock, msg_buf, sizeof(msg_buf));
        log_msg(LOG_LEVEL_INFO, MOD_NAME "Remote client name: %s\n", msg_buf);
        recv_msg(coord_sock, msg_buf, sizeof(msg_buf));
        log_msg(LOG_LEVEL_VERBOSE, MOD_NAME "Remote desc: %s\n", msg_buf);

        juice_set_remote_description(agent, msg_buf);
}

static bool discover_and_xchg_candidates(juice_agent_t *agent, int coord_sock,
                char *local, char *remote)
{
        juice_gather_candidates(agent);

        struct pollfd fds;
        fds.fd = coord_sock;
        fds.events = POLLIN;

        while(1){
                poll(&fds, 1, 300);
                if(fds.revents & POLLIN){
                        char msg_buf[MAX_MSG_LEN];
                        recv_msg(coord_sock, msg_buf, sizeof(msg_buf));
                        log_msg(LOG_LEVEL_VERBOSE, MOD_NAME "Received remote candidate\n");
                        juice_add_remote_candidate(agent, msg_buf);
                }
                juice_state_t state = juice_get_state(agent);
                if(state == JUICE_STATE_COMPLETED)
                        break;
        }

        if ((juice_get_selected_addresses(agent,
                                        local,
                                        JUICE_MAX_CANDIDATE_SDP_STRING_LEN,
                                        remote,
                                        JUICE_MAX_CANDIDATE_SDP_STRING_LEN) == 0)) {
                log_msg(LOG_LEVEL_INFO, MOD_NAME "Local candidate  : %s\n", local);
                log_msg(LOG_LEVEL_INFO, MOD_NAME "Remote candidate : %s\n", remote);
        }
}

static bool initialize_punch(struct Punch_ctx *ctx, const struct Holepunch_config *c){
        if(!connect_to_coordinator(c->coord_srv_addr, c->coord_srv_port, &ctx->coord_sock)){
                return false;
        }

        send_msg(ctx->coord_sock, c->client_name);
        send_msg(ctx->coord_sock, c->room_name);

        ctx->juice_agent = create_agent(c, ctx);

        exchange_coord_desc(ctx->juice_agent, ctx->coord_sock);

        return true;
}

static bool split_host_port(char *pair, int *port){
        char *colon = strrchr(pair, ':');
        if(!colon)
                return false;

        *colon = '\0';

        char *end;
        int p = strtol(colon + 1, &end, 10);
        if(end == colon + 1)
                return false;

        *port = p;
        return true;
}

static void juice_log_handler(juice_log_level_t level, const char *message){
        log_msg(LOG_LEVEL_DEBUG, MOD_NAME "libjuice: %s\n", message);
}

bool punch_udp(const struct Holepunch_config *c){
        struct Punch_ctx video_ctx = {0};

        juice_set_log_level(JUICE_LOG_LEVEL_DEBUG);
        juice_set_log_handler(juice_log_handler);

        if(!initialize_punch(&video_ctx, c)){
                return false;
        }

        char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
        char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
        discover_and_xchg_candidates(video_ctx.juice_agent,
                        video_ctx.coord_sock,
                        local, remote);

        juice_destroy(video_ctx.juice_agent);
        close(video_ctx.coord_sock);

        *c->video_rx_port = video_ctx.local_candidate_port;
        assert(split_host_port(remote, c->video_tx_port));

        strncpy(c->host_addr, remote, c->host_addr_len);

        return true;
}
