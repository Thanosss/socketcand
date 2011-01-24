
/*
 *
 * Authors:
 * Andre Naujoks (the socket server stuff)
 * Oliver Hartkopp (the rest)
 * Jan-Niklas Meier (extensions for use with kayak)
 *
 * Copyright (c) 2002-2009 Volkswagen Group Electronic Research
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <socketcan-users@lists.berlios.de>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <getopt.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

#include <linux/can.h>
#include <linux/can/bcm.h>
#include <linux/can/error.h>
#include <linux/can/netlink.h>

#include <libsocketcan.h>

#include "socketcand.h"
#include "statistics.h"
#include "beacon.h"

void print_usage(void);
void sigint();
void childdied();
int receive_command(int socket, char *buf);

int sl, client_socket;
pthread_t beacon_thread, statistics_thread;
char **interface_names;
int interface_count=0;
int port=PORT;
struct in_addr laddr;
int verbose_flag=0;
int daemon_flag=0;
int uid;
int state = STATE_NO_BUS;
int previous_state = -1;
char bus_name[MAX_BUSNAME];

int main(int argc, char **argv)
{
    int sc = -1;
    int i, ret, found;
    int idx = 0;
    struct sockaddr_in  saddr, clientaddr;
    struct sockaddr_can caddr;
    socklen_t caddrlen = sizeof(caddr);
    struct ifreq ifr;
    fd_set readfds;
    socklen_t sin_size = sizeof(clientaddr);
    struct sigaction signalaction, sigint_action;
    sigset_t sigset;

    char buf[MAXLEN];
    char rxmsg[50];

    struct {
        struct bcm_msg_head msg_head;
        struct can_frame frame;
    } msg;

    int c;

    uid = getuid();
    if(uid != 0) {
        printf("You are not running socketcand as root. This is highly reccomended because otherwise you won't be able to change bitrate settings, etc.\n");
    }

    /* default is to listen on 127.0.0.1 only */
    laddr.s_addr = inet_addr( "127.0.0.1" );

    /* Parse commandline arguments */
    while (1) {
        /* getopt_long stores the option index here. */
        int option_index = 0;
        static struct option long_options[] = {
            {"verbose", no_argument, 0, 'v'},
            {"interfaces",  required_argument, 0, 'i'},
            {"port", required_argument, 0, 'p'},
            {"listen", required_argument, 0, 'l'},
            {"daemon", no_argument, 0, 'd'},
            {0, 0, 0, 0}
        };
    
        c = getopt_long (argc, argv, "vhi:p:l:d", long_options, &option_index);
    
        if (c == -1)
            break;
    
        switch (c) {
            case 0:
                /* If this option set a flag, do nothing else now. */
                if (long_options[option_index].flag != 0)
                    break;
            break;
    
    
            case 'v':
                puts ("Verbose output activated\n");
                verbose_flag = 1;
                break;
    
            case 'p':
                port = atoi(optarg);
                printf("Using Port %d\n", port);
                break;
    
            case 'i':
                for(i=0;;i++) {
                    if(optarg[i] == '\0')
                        break;
                    if(optarg[i] == ',')
                        interface_count++;
                }
                interface_count++;
    
                interface_names = malloc(sizeof(char *) * interface_count);
    
                interface_names[0] = strtok(optarg, ",");
    
                for(i=1;i<interface_count;i++) {
                    interface_names[i] = strtok(NULL, ",");
                }
                break;

            case 'l':
                laddr.s_addr = inet_addr( optarg );
                break;

            case 'h':
                print_usage();
                return 0;

            case 'd':
                daemon_flag=1;
                break;
                
            case '?':
                print_usage();
                return -1;
    
            default:
                print_usage();
                return -1;
        }
    }

    /* if daemon mode was activated the syslog must be opened */
    if(daemon_flag) {
        openlog("socketcand", 0, LOG_DAEMON);
    }


    sigemptyset(&sigset);
    signalaction.sa_handler = &childdied;
    signalaction.sa_mask = sigset;
    signalaction.sa_flags = 0;
    sigaction(SIGCHLD, &signalaction, NULL);  /* signal for dying child */
    
    sigint_action.sa_handler = &sigint;
    sigint_action.sa_mask = sigset;
    sigint_action.sa_flags = 0;
    sigaction(SIGINT, &sigint_action, NULL);

    if((sl = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("inetsocket");
        exit(1);
    }

#ifdef DEBUG
    if(verbose_flag)
        printf("setting SO_REUSEADDR\n");
    i = 1;
    if(setsockopt(sl, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i)) <0) {
        perror("setting SO_REUSEADDR failed");
    }
#endif

    saddr.sin_family = AF_INET;
    saddr.sin_addr = laddr;
    saddr.sin_port = htons(port);

    PRINT_VERBOSE("creating broadcast thread...\n")
    pthread_create(&beacon_thread, NULL, &beacon_loop, NULL);

    PRINT_VERBOSE("binding socket to %s:%d\n", inet_ntoa(saddr.sin_addr), ntohs(saddr.sin_port))
    if(bind(sl,(struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        perror("bind");
        exit(-1);
    }

    if (listen(sl,3) != 0) {
        perror("listen");
        exit(1);
    }

    while (1) { 
        client_socket = accept(sl,(struct sockaddr *)&clientaddr, &sin_size);
        if (client_socket > 0 ){

            if (fork())
                close(client_socket);
            else
                break;
        }
        else {
            if (errno != EINTR) {
                /*
                 * If the cause for the error was NOT the
                 * signal from a dying child => give an error
                 */
                perror("accept");
                exit(1);
            }
        }
    }

    PRINT_VERBOSE("client connected\n")
    
#ifdef DEBUG
    PRINT_VERBOSE("setting SO_REUSEADDR\n")
    i = 1;
    if(setsockopt(client_socket, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i)) <0) {
        perror("setting SO_REUSEADDR failed");
    }
#endif

    /* main loop with state machine */
    while(1) {
        switch(state) {
            case STATE_NO_BUS:
                if(previous_state != STATE_NO_BUS) {
                    strcpy(buf, "<hi>");
                    send(client_socket, buf, strlen(buf), 0);
                    previous_state = STATE_NO_BUS;
                }
                /* client has to start with a command */
                receive_command(client_socket, buf);

                if(strcmp("<open ", buf)) {
                    sscanf("<open %s>", buf, bus_name);

                    /* check if access to this bus is allowed */
                    found = 0;
                    for(i=0;i<interface_count;i++) {
                        if(!strcmp(interface_names[i], bus_name))
                        found = 1;
                    }

                    if(found) {
                        state = STATE_BCM;
                        break;
                    } else {
                        PRINT_INFO("client tried to access unauthorized bus.\n");
                        strcpy(buf, "<error could not open bus>");
                        send(client_socket, buf, strlen(buf), 0);
                        state = STATE_SHUTDOWN;
                    }
                }
                break;

            case STATE_BCM:
                if(previous_state != STATE_BCM) {
                    /* open BCM socket */
                    if ((sc = socket(PF_CAN, SOCK_DGRAM, CAN_BCM)) < 0) {
                        PRINT_ERROR("Error while opening BCM socket %s\n", strerror(errno));
                        state = STATE_SHUTDOWN;
                        break;
                    }

                    memset(&caddr, 0, sizeof(caddr));
                    caddr.can_family = PF_CAN;
                    /* can_ifindex is set to 0 (any device) => need for sendto() */
                    
                    PRINT_VERBOSE("connecting BCM socket...\n")
                    if (connect(sc, (struct sockaddr *)&caddr, sizeof(caddr)) < 0) {
                        PRINT_ERROR("Error while connecting BCM socket %s\n", strerror(errno));
                        state = STATE_SHUTDOWN;
                        break;
                    }
                    previous_state = STATE_BCM;
                }

                FD_ZERO(&readfds);
                FD_SET(sc, &readfds);
                FD_SET(client_socket, &readfds);

                ret = select((sc > client_socket)?sc+1:client_socket+1, &readfds, NULL, NULL, NULL);

                if (FD_ISSET(sc, &readfds)) {

                    ret = recvfrom(sc, &msg, sizeof(msg), 0,
                            (struct sockaddr*)&caddr, &caddrlen);

                    ifr.ifr_ifindex = caddr.can_ifindex;
                    ioctl(sc, SIOCGIFNAME, &ifr);

                    /* Check if this is an error frame */
                    if(msg.msg_head.can_id & 0x20000000) {
                        if(msg.frame.can_dlc != CAN_ERR_DLC) {
                            PRINT_ERROR("Error frame has a wrong DLC!\n")
                        } else {
                            sprintf(rxmsg, "< %s e %03X ", ifr.ifr_name,
                                msg.msg_head.can_id);

                            for ( i = 0; i < msg.frame.can_dlc; i++)
                                sprintf(rxmsg + strlen(rxmsg), "%02X ",
                                    msg.frame.data[i]);

                            /* delimiter '\0' for Adobe(TM) Flash(TM) XML sockets */
                            strcat(rxmsg, ">\0");
                            send(client_socket, rxmsg, strlen(rxmsg) + 1, 0);
                        }
                    } else {
                        sprintf(rxmsg, "< %s f %03X %d ", ifr.ifr_name,
                            msg.msg_head.can_id, msg.frame.can_dlc);

                        for ( i = 0; i < msg.frame.can_dlc; i++)
                            sprintf(rxmsg + strlen(rxmsg), "%02X ",
                                msg.frame.data[i]);

                        /* delimiter '\0' for Adobe(TM) Flash(TM) XML sockets */
                        strcat(rxmsg, ">\0");

                        send(client_socket, rxmsg, strlen(rxmsg) + 1, 0);
                    }
                }
                
                if (FD_ISSET(client_socket, &readfds)) {

                    char cmd;
                    char bus_name[6];
                    int found=0;
                    int items;
                    int i,j,k;
                    struct can_bittiming timing;
                    struct can_ctrlmode ctrlmode;

                    if (read(client_socket, buf+idx, 1) < 1)
                        exit(1);

                    if (!idx) {
                        if (buf[0] == '<')
                            idx = 1;

                        continue;
                    }

                    if (idx > MAXLEN-2) {
                        idx = 0;
                        continue;
                    }

                    if (buf[idx] != '>') {
                        idx++;
                        continue;
                    }

                    buf[idx+1] = 0;
                    idx = 0;

                    PRINT_VERBOSE("Received '%s'\n", buf)

                    /* Extract busname and command */
                    sscanf(buf, "< %6s %c ", bus_name, &cmd);

                    /* Check if we work on this bus */
                    found = 0;
                    for(i=0;i<interface_count;i++) {
                        if(!strcmp(interface_names[i], bus_name))
                            found = 1;
                    }
                    if(found == 0) {
                        PRINT_ERROR("Wrong bus name was specified!\n")
                    } else {
                        /* prepare bcm message settings */
                        memset(&msg, 0, sizeof(msg));
                        msg.msg_head.nframes = 1;

                        switch (cmd) {
                        case 'S': /* Send a single frame */
                            items = sscanf(buf, "< %6s %c %x %hhu "
                                "%hhx %hhx %hhx %hhx %hhx %hhx "
                                "%hhx %hhx >",
                                ifr.ifr_name,
                                &cmd, 
                                &msg.msg_head.can_id,
                                &msg.frame.can_dlc,
                                &msg.frame.data[0],
                                &msg.frame.data[1],
                                &msg.frame.data[2],
                                &msg.frame.data[3],
                                &msg.frame.data[4],
                                &msg.frame.data[5],
                                &msg.frame.data[6],
                                &msg.frame.data[7]);

                            if ( (items < 4) ||
                                (msg.frame.can_dlc > 8) ||
                                (items != 4 + msg.frame.can_dlc)) {
                                PRINT_ERROR("Syntax error in send command\n")
                                break;
                            }

                            msg.msg_head.opcode = TX_SEND;
                            msg.frame.can_id = msg.msg_head.can_id;

                            if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                caddr.can_ifindex = ifr.ifr_ifindex;
                                sendto(sc, &msg, sizeof(msg), 0,
                                    (struct sockaddr*)&caddr, sizeof(caddr));
                            }

                            break;
                        case 'A': /* Add a send job */
                            items = sscanf(buf, "< %6s %c %lu %lu %x %hhu "
                                "%hhx %hhx %hhx %hhx %hhx %hhx "
                                "%hhx %hhx >",
                                ifr.ifr_name,
                                &cmd, 
                                &msg.msg_head.ival2.tv_sec,
                                &msg.msg_head.ival2.tv_usec,
                                &msg.msg_head.can_id,
                                &msg.frame.can_dlc,
                                &msg.frame.data[0],
                                &msg.frame.data[1],
                                &msg.frame.data[2],
                                &msg.frame.data[3],
                                &msg.frame.data[4],
                                &msg.frame.data[5],
                                &msg.frame.data[6],
                                &msg.frame.data[7]);

                            if (items < 6)
                                break;
                            if (msg.frame.can_dlc > 8)
                                break;
                            if (items != 6 + msg.frame.can_dlc)
                                break;

                            msg.msg_head.opcode = TX_SETUP;
                            msg.msg_head.flags |= SETTIMER | STARTTIMER;
                            msg.frame.can_id = msg.msg_head.can_id;

                            if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                caddr.can_ifindex = ifr.ifr_ifindex;
                                sendto(sc, &msg, sizeof(msg), 0,
                                    (struct sockaddr*)&caddr, sizeof(caddr));
                            }

                            break;
                        case 'U': /* Update send job */
                            items = sscanf(buf, "< %6s %c %x %hhu "
                                "%hhx %hhx %hhx %hhx %hhx %hhx "
                                "%hhx %hhx >",
                                ifr.ifr_name,
                                &cmd, 
                                &msg.msg_head.can_id,
                                &msg.frame.can_dlc,
                                &msg.frame.data[0],
                                &msg.frame.data[1],
                                &msg.frame.data[2],
                                &msg.frame.data[3],
                                &msg.frame.data[4],
                                &msg.frame.data[5],
                                &msg.frame.data[6],
                                &msg.frame.data[7]);

                            if ( (items < 4) ||
                                (msg.frame.can_dlc > 8) ||
                                (items != 4 + msg.frame.can_dlc)) {
                                PRINT_ERROR("Syntax error in update send job command\n")
                                break;
                            }
                            
                            msg.msg_head.opcode = TX_SETUP;
                            msg.msg_head.flags  = 0;
                            msg.frame.can_id = msg.msg_head.can_id;

                            if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                caddr.can_ifindex = ifr.ifr_ifindex;
                                sendto(sc, &msg, sizeof(msg), 0,
                                    (struct sockaddr*)&caddr, sizeof(caddr));
                            }

                            break;
                        case 'D': /* Delete a send job */
                            items = sscanf(buf, "< %6s %c %x >",
                                ifr.ifr_name,
                                &cmd, 
                                &msg.msg_head.can_id);

                            if (items != 3)  {
                                PRINT_ERROR("Syntax error in delete job command\n")
                                break;
                            }

                            msg.msg_head.opcode = TX_DELETE;
                            msg.frame.can_id = msg.msg_head.can_id;

                            if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                caddr.can_ifindex = ifr.ifr_ifindex;
                                sendto(sc, &msg, sizeof(msg), 0,
                                    (struct sockaddr*)&caddr, sizeof(caddr));
                            }

                            break;

                        case 'R': /* Receive CAN ID with content matching */
                            items = sscanf(buf, "< %6s %c %lu %lu %x %hhu "
                                "%hhx %hhx %hhx %hhx %hhx %hhx "
                                "%hhx %hhx >",
                                ifr.ifr_name,
                                &cmd, 
                                &msg.msg_head.ival2.tv_sec,
                                &msg.msg_head.ival2.tv_usec,
                                &msg.msg_head.can_id,
                                &msg.frame.can_dlc,
                                &msg.frame.data[0],
                                &msg.frame.data[1],
                                &msg.frame.data[2],
                                &msg.frame.data[3],
                                &msg.frame.data[4],
                                &msg.frame.data[5],
                                &msg.frame.data[6],
                                &msg.frame.data[7]);

                            if (items < 6)
                                break;
                            if (msg.frame.can_dlc > 8)
                                break;
                            if (items != 6 + msg.frame.can_dlc)
                                break;

                            msg.msg_head.opcode = RX_SETUP;
                            msg.msg_head.flags  = SETTIMER;
                            msg.frame.can_id = msg.msg_head.can_id;

                            if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                caddr.can_ifindex = ifr.ifr_ifindex;
                                sendto(sc, &msg, sizeof(msg), 0,
                                    (struct sockaddr*)&caddr, sizeof(caddr));
                            }

                            break;
                        case 'F': /* Add a filter */
                            items = sscanf(buf, "< %6s %c %lu %lu %x >",
                                ifr.ifr_name,
                                &cmd, 
                                &msg.msg_head.ival2.tv_sec,
                                &msg.msg_head.ival2.tv_usec,
                                &msg.msg_head.can_id);

                            if (items != 5) {
                                PRINT_ERROR("syntax error in add filter command\n")
                                break;
                            }

                            msg.msg_head.opcode = RX_SETUP;
                            msg.msg_head.flags  = RX_FILTER_ID | SETTIMER;
                            msg.frame.can_id = msg.msg_head.can_id;

                            /*
                            * if the ID is one of the special values for multiple IDs
                            * a subscription to all of these IDs is initiated.
                            */
                            if(msg.msg_head.can_id == CANID_BASE_ALL) {
                                for(i=0;i<0x7ff;i++) {
                                    msg.msg_head.can_id = i;
                                    msg.frame.can_id = i;
                                    if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                        caddr.can_ifindex = ifr.ifr_ifindex;
                                        sendto(sc, &msg, sizeof(msg), 0,
                                            (struct sockaddr*)&caddr, sizeof(caddr));
                                    }
                                }
                            /*
                            * TODO this solution is suboptimal because it takes a REALLY
                            * long time to subscribe to all IDs.
                            */
                            } else if(msg.msg_head.can_id == CANID_EXTENDED_ALL) {
                                for(i=0;i<0x1fffffff;i++) {
                                    msg.msg_head.can_id = i;
                                    msg.frame.can_id = i;
                                    if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                        caddr.can_ifindex = ifr.ifr_ifindex;
                                        sendto(sc, &msg, sizeof(msg), 0,
                                            (struct sockaddr*)&caddr, sizeof(caddr));
                                    }
                                }
                            /* normal subscription to a single id */
                            } else {
                                if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                    caddr.can_ifindex = ifr.ifr_ifindex;
                                    sendto(sc, &msg, sizeof(msg), 0,
                                        (struct sockaddr*)&caddr, sizeof(caddr));
                                }
                            }

                            break;
                        case 'X': /* Delete filter */
                            items = sscanf(buf, "< %6s %c %x >",
                                ifr.ifr_name,
                                &cmd, 
                                &msg.msg_head.can_id);

                            if (items != 3) {
                                PRINT_ERROR("syntax error in delete filter command\n")
                                break;
                            }

                            msg.msg_head.opcode = RX_DELETE;
                            msg.frame.can_id = msg.msg_head.can_id;
                            
                            /*
                            * if the ID is one of the special values for multiple IDs
                            * a subscription to all of these IDs is initiated.
                            */
                            if(msg.msg_head.can_id == CANID_BASE_ALL) {
                                for(i=0;i<0x7ff;i++) {
                                    msg.msg_head.can_id = i;
                                    msg.frame.can_id = i;
                                    if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                        caddr.can_ifindex = ifr.ifr_ifindex;
                                        sendto(sc, &msg, sizeof(msg), 0,
                                            (struct sockaddr*)&caddr, sizeof(caddr));
                                    }
                                }
                            /*
                            * TODO this solution is suboptimal because it takes a REALLY
                            * long time to subscribe to all IDs.
                            */
                            } else if(msg.msg_head.can_id == CANID_EXTENDED_ALL) {
                                for(i=0;i<0x1fffffff;i++) {
                                    msg.msg_head.can_id = i;
                                    msg.frame.can_id = i;
                                    if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                        caddr.can_ifindex = ifr.ifr_ifindex;
                                        sendto(sc, &msg, sizeof(msg), 0,
                                            (struct sockaddr*)&caddr, sizeof(caddr));
                                    }
                                }
                            } else {
                                if (!ioctl(sc, SIOCGIFINDEX, &ifr)) {
                                    caddr.can_ifindex = ifr.ifr_ifindex;
                                    sendto(sc, &msg, sizeof(msg), 0,
                                        (struct sockaddr*)&caddr, sizeof(caddr));
                                }
                            }

                            break;
                        case 'B': /* Set bitrate */
                            memset(&timing, 0, sizeof(timing));

                            items = sscanf(buf, "< %6s %c %x %x %x %x %x %x %x %x >",
                                bus_name,
                                &cmd,
                                &timing.bitrate,
                                &timing.sample_point,
                                &timing.tq,
                                &timing.prop_seg,
                                &timing.phase_seg1,
                                &timing.phase_seg2,
                                &timing.sjw,
                                &timing.brp);

                            if (items != 10) {
                                PRINT_ERROR("Syntax error in set bitrate command\n")
                                break;
                            }

                            can_set_bittiming(bus_name, &timing);

                            break;
                        case 'C': /* Set control mode */
                            memset(&ctrlmode, 0, sizeof(ctrlmode));
                            ctrlmode.mask = CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_LISTENONLY | CAN_CTRLMODE_3_SAMPLES;

                            items = sscanf(buf, "< %6s %c %u %u %u >",
                                bus_name,
                                &cmd,
                                &i,
                                &j,
                                &k);

                            if (items != 5) {
                                PRINT_ERROR("Syntax error in set controlmode command\n")
                                break;
                            }

                            if(i)
                                ctrlmode.flags |= CAN_CTRLMODE_LISTENONLY;
                            if(j)
                                ctrlmode.flags |= CAN_CTRLMODE_LOOPBACK;
                            if(k)
                                ctrlmode.flags |= CAN_CTRLMODE_3_SAMPLES;

                            can_set_ctrlmode(bus_name, &ctrlmode);

                            break;
                        case 'E': /* Enable or disable statistics */
                            items = sscanf(buf, "< %6s %c %u >",
                                bus_name,
                                &cmd,
                                &i);

                            if (items != 3) {
                                PRINT_ERROR("Syntax error in statistics command\n")
                                break;
                            }

                            set_statistics(bus_name, i);

                            break;
                        default:
                            PRINT_ERROR("unknown command '%c'.\n", cmd)
                            exit(1);
                        }
                    }
                }
                        break;
                    
                    case STATE_RAW:
                        if(previous_state != STATE_RAW) {
                            PRINT_VERBOSE("starting statistics thread...\n")
                            pthread_mutex_init( &stat_mutex, NULL );
                            pthread_cond_init( &stat_condition, NULL );
                            pthread_mutex_lock( &stat_mutex );
                            pthread_create( &statistics_thread, NULL, &statistics_loop, NULL );
                            pthread_cond_wait( &stat_condition, &stat_mutex );
                            pthread_mutex_unlock( &stat_mutex );
                            previous_state = STATE_RAW;
                        }

                break;

            case STATE_SHUTDOWN:
                close(client_socket);
                return 0;
        }
    }

    return 0;
}

int receive_command(int socket, char *buf) {
    int idx = 0;
    while(1) {
        if (read(socket, buf+idx, 1) < 1)
            return -1;

        if (!idx) {
            if (buf[0] == '<')
                idx = 1;

            continue;
        }

        if (idx > MAXLEN-2) {
            idx = 0;
            continue;
        }

        if (buf[idx] != '>') {
            idx++;
            continue;
        }

        buf[idx+1] = 0;
        break;
    }

    return idx;
}

void print_usage(void) {
    printf("Socket CAN daemon\n");
    printf("Usage: socketcand [-v | --verbose] [-i interfaces | --interfaces interfaces]\n\t\t[-p port | --port port] [-l ip_addr | --listen ip_addr]\n\n");
    printf("Options:\n");
    printf("\t-v activates verbose output to STDOUT\n");
    printf("\t-i interfaces is used to specify the Socket CAN interfaces the daemon\n\t\tshall provide access to\n");
    printf("\t-p port changes the default port (28600) the daemon is listening at\n");
    printf("\t-l ip_addr changes the default ip address (127.0.0.1) the daemon will\n\t\tbind to\n");
    printf("\t-h prints this message\n");
}

void childdied() {
    wait(NULL);
}

void sigint() {
    if(verbose_flag)
        PRINT_ERROR("received SIGINT\n")

    if(sl != -1) {
        if(verbose_flag)
            PRINT_INFO("closing listening socket\n")
        if(!close(sl))
            sl = -1;
    }

    if(client_socket != -1) {
        if(verbose_flag)
            PRINT_INFO("closing client socket\n")
        if(!close(client_socket))
            client_socket = -1;
    }

    pthread_cancel(beacon_thread);
    pthread_cancel(statistics_thread);

    closelog();

    exit(0);
}
