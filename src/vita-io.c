// SPDX-Licence-Identifier: GPL-3.0-or-later
/*
 * vita-io.c - Support for VITA-49 data socket of FlexRadio 6000 units.
 *
 * Author: Annaliese McDermond <nh6z@nh6z.net>
 *
 * Copyright 2019 Annaliese McDermond
 *
 * This file is part of smartsdr-codec2.
 *
 * smartsdr-codec2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Foobar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with smartsdr-codec2.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#define _GNU_SOURCE

#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <stdbool.h>
#include <fcntl.h>
#include <aio.h>

#include "utils.h"
#include "vita.h"
#include "freedv-processor.h"
#include "api-io.h"

static int vita_sock;
static bool vita_processing_thread_abort = true;
static pthread_t vita_processing_thread;
static uint8_t meter_sequence = 0;

static freedv_proc_t freedv_params;

static uint32_t rx_stream_id, tx_stream_id;

static void vita_process_waveform_packet(struct vita_packet *packet, ssize_t length)
{
    unsigned long payload_length = ((htons(packet->length) * sizeof(uint32_t)) - VITA_PACKET_HEADER_SIZE);
    if(payload_length != length - VITA_PACKET_HEADER_SIZE) {
        output("VITA header size doesn't match bytes read from network (%d != %d - %d) -- %d\n", payload_length, length, VITA_PACKET_HEADER_SIZE, sizeof(struct vita_packet));
        return;
    }

    if (!(htonl(packet->stream_id) & 0x0001u)) {
        //  Receive Packet Processing
        rx_stream_id = packet->stream_id;
        freedv_queue_samples(freedv_params, 0, payload_length, packet->if_samples);
    } else {
        //  Transmit packet processing
        tx_stream_id = packet->stream_id;
        freedv_queue_samples(freedv_params, 1, payload_length, packet->if_samples);
    }
}

static void vita_parse_packet(struct vita_packet *packet, size_t packet_len)
{
    // make sure packet is long enough to inspect for VITA header info
    if (packet_len < VITA_PACKET_HEADER_SIZE)
        return;

    if((packet->class_id & VITA_OUI_MASK) != FLEX_OUI)
        return;

    switch(packet->stream_id & STREAM_BITS_MASK) {
        case STREAM_BITS_WAVEFORM | STREAM_BITS_IN:
            vita_process_waveform_packet(packet, packet_len);
            break;
        default:
            output("Undefined stream in %08X", htonl(packet->stream_id));
            break;
    }
}

#define MAX_PACKETS_TO_RECEIVE 1

static void* vita_processing_loop()
{
    struct vita_packet packet[MAX_PACKETS_TO_RECEIVE];
    struct iovec iovec[MAX_PACKETS_TO_RECEIVE];
    struct mmsghdr msgvec[MAX_PACKETS_TO_RECEIVE];

    int ret;
    size_t bytes_received = 0;

    output("Beginning VITA Listener Loop...\n");
    vita_processing_thread_abort = false;

    pthread_setname_np(pthread_self(), "FreeDV VitaIO");
    struct timespec timeout;
    while(!vita_processing_thread_abort) {
        if(clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
            output("Couldn't get time.\n");
            continue;
        }

        long nanoseconds = 5333333; // (1000000000 / RADIO_SAMPLE_RATE) * 128
        long seconds = (timeout.tv_nsec + nanoseconds) / 1000000000;
        timeout.tv_nsec = (timeout.tv_nsec + nanoseconds) % 1000000000;
        timeout.tv_sec += seconds;

        for (int i = 0; i < MAX_PACKETS_TO_RECEIVE; i++)
        {
            iovec[i].iov_base = &packet[i];
            iovec[i].iov_len = sizeof(packet[i]);
    
            msgvec[i].msg_hdr.msg_name = NULL;
            msgvec[i].msg_hdr.msg_namelen = 0;
            msgvec[i].msg_hdr.msg_iovlen = 1;
            msgvec[i].msg_hdr.msg_iov = &iovec[i];
            msgvec[i].msg_hdr.msg_control = NULL;
            msgvec[i].msg_hdr.msg_controllen = 0;
            msgvec[i].msg_hdr.msg_flags = 0;
            msgvec[i].msg_len = 0;
        }
    
        ret = recvmmsg(vita_sock, msgvec, MAX_PACKETS_TO_RECEIVE, MSG_WAITFORONE, &timeout);

        if (ret == 0) {
            // timeout
            output("VITA recv timed out\n");
            continue;
        } else if (ret == -1) {
            if (errno != EINTR)
            {
                // Interrupted system call errors are fine.
                // Anything else should print an error.
                output("VITA poll failed: %s\n", strerror(errno));
            }
            continue;
        }

        for (int i = 0; i < ret; i++)
        {
            bytes_received = msgvec[i].msg_len;
            vita_parse_packet(&packet[i], bytes_received);
        }

        freedv_signal_processing_thread(freedv_params);
    }

    output("Ending VITA Listener Loop...\n");
    return NULL;
}

extern pthread_attr_t global_pthread_properties;

unsigned short vita_init(freedv_proc_t params)
{
    freedv_params = params;
    struct sockaddr_in bind_addr =  {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = 0,
    };
    socklen_t bind_addr_len = sizeof(bind_addr);

    struct sockaddr_in radio_addr;
    if (get_radio_addr(&radio_addr) == -1) {
        output("Failed to get radio address: %s\n", strerror(errno));
        return 0;
    }
    radio_addr.sin_port = htons(4993);

    output("Initializing VITA-49 engine...\n");

    vita_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (vita_sock == -1) {
        output(" Failed to initialize VITA socket: %s\n", strerror(errno));
        return 0;
    }

    output("Binding VITA socket...\n");
    if(bind(vita_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr))) {
        output("error binding socket: %s\n",strerror(errno));
        close(vita_sock);
        return 0;
    }

    output("Connecting VITA socket...\n");
    if(connect(vita_sock, (struct sockaddr *) &radio_addr, sizeof(struct sockaddr_in)) == -1) {
        output("Couldn't connect socket: %s\n", strerror(errno));
        close(vita_sock);
        return 0;
    }

    output("Getting VITA socket port number...\n");
    if (getsockname(vita_sock, (struct sockaddr *) &bind_addr, &bind_addr_len) == -1) {
        output("Couldn't get port number of VITA socket\n");
        close(vita_sock);
        return 0;
    }

    vita_processing_thread = (pthread_t) NULL;

    output("Creating VITA thread...\n");
    if (pthread_create(&vita_processing_thread, &global_pthread_properties, &vita_processing_loop, NULL) == -1)
    {
        output("Could not create VITA thread: %s\n", strerror(errno));
        close(vita_sock);
        return 0;
    }
    
    return ntohs(bind_addr.sin_port);
}

void vita_stop()
{
    if (vita_processing_thread_abort == false) {
        vita_processing_thread_abort = true;
        pthread_join(vita_processing_thread, NULL);
        close(vita_sock);
    }

    if (freedv_params) {
        freedv_destroy(freedv_params);
        freedv_params = NULL;
    }
}

#define MAX_SEND_PACKETS_IN_QUEUE 32

static struct vita_packet queued_packet[MAX_SEND_PACKETS_IN_QUEUE];
static struct aiocb queued_packet_cb[MAX_SEND_PACKETS_IN_QUEUE];
static int current_queue_index = 0;

static time_t current_time = 0;
static uint32_t frac_seq = 0;

static struct aiocb* get_next_vita_queue_entry()
{
    int err = aio_error(&queued_packet_cb[current_queue_index]);
    while(err == EINPROGRESS)
    {
        current_queue_index = (current_queue_index + 1) % MAX_SEND_PACKETS_IN_QUEUE;
        err = aio_error(&queued_packet_cb[current_queue_index]);
    }

    memset(&queued_packet_cb[current_queue_index], 0, sizeof(struct aiocb));
    queued_packet_cb[current_queue_index].aio_fildes = vita_sock;
    queued_packet_cb[current_queue_index].aio_buf = &queued_packet[current_queue_index];
    queued_packet_cb[current_queue_index].aio_sigevent.sigev_notify = SIGEV_NONE;

    struct aiocb* ret = &queued_packet_cb[current_queue_index];
    current_queue_index = (current_queue_index + 1) % MAX_SEND_PACKETS_IN_QUEUE;
    return ret;
}

static void vita_send_packet(struct aiocb* cb,  size_t payload_len)
{
    struct vita_packet* packet = (struct vita_packet*)cb->aio_buf;
    ssize_t bytes_sent;
    size_t packet_len = VITA_PACKET_HEADER_SIZE + payload_len;

    //  XXX Lots of magic numbers here!
    packet->timestamp_type = 0x50u | (packet->timestamp_type & 0x0Fu);
    assert(packet_len % 4 == 0);
    packet->length = htons(packet_len / 4); // Length is in 32-bit words

    packet->timestamp_int = time(NULL);
    if (packet->timestamp_int != current_time)
    {
        frac_seq = 0;
    }
    packet->timestamp_frac = frac_seq++;
    current_time = packet->timestamp_int;

    cb->aio_nbytes = packet_len;
    aio_write(cb);
}

void vita_send_meter_packet(void *meters, size_t len)
{
    struct aiocb* cb = get_next_vita_queue_entry();
    struct vita_packet* packet = (struct vita_packet*)cb->aio_buf;

    packet->packet_type = VITA_PACKET_TYPE_EXT_DATA_WITH_STREAM_ID;
    packet->stream_id = METER_STREAM_ID;
    packet->class_id = METER_CLASS_ID;
    packet->timestamp_type = meter_sequence++;

    assert(len < sizeof(packet->raw_payload));
    memcpy(packet->raw_payload, meters, len);

    vita_send_packet(cb, len);
}

static uint32_t audio_sequence = 0;
void vita_send_audio_packet(uint32_t *samples, size_t len, unsigned int tx)
{
    struct aiocb* cb = get_next_vita_queue_entry();
    struct vita_packet* packet = (struct vita_packet*)cb->aio_buf;

    assert(len * 2 <= sizeof(packet->if_samples));

    packet->packet_type = VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID;
    packet->stream_id = tx ? tx_stream_id : rx_stream_id;
    packet->class_id = AUDIO_CLASS_ID;
    packet->timestamp_type = audio_sequence++;

    for (unsigned int i = 0, j = 0; i < len / sizeof(uint32_t); ++i, j += 2)
        packet->if_samples[j] = packet->if_samples[j + 1] = htonl(samples[i]);

    vita_send_packet(cb, len * 2);
}
