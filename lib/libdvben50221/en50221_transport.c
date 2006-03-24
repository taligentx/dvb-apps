/*
    en50221 encoder An implementation for libdvb
    an implementation for the en50221 transport layer

    Copyright (C) 2004, 2005 Manu Abraham (manu@kromtek.com)
    Copyright (C) 2005 Julian Scheel (julian at jusst dot de)
    Copyright (C) 2006 Andrew de Quincey (adq_dvb@lidskialf.net)

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as
    published by the Free Software Foundation; either version 2.1 of
    the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
*/

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/delay.h>
#include <sys/poll.h>
#include <time.h>
#include <dvbmisc.h>
#include <dvbapi/dvbca.h>
#include "en50221_errno.h"
#include "en50221_transport.h"
#include "asn_1.h"

// these are the Transport Tags, like
// described in EN50221, Annex A.4.1.13 (pg70)
#define T_SB                0x80 // sb                           primitive   h<--m
#define T_RCV               0x81 // receive                      primitive   h-->m
#define T_CREATE_T_C        0x82 // create transport connection  primitive   h-->m
#define T_C_T_C_REPLY       0x83 // ctc reply                    primitive   h<--m
#define T_DELETE_T_C        0x84 // delete tc                    primitive   h<->m
#define T_D_T_C_REPLY       0x85 // dtc reply                    primitive   h<->m
#define T_REQUEST_T_C       0x86 // request transport connection primitive   h<--m
#define T_NEW_T_C           0x87 // new tc / reply to t_request  primitive   h-->m
#define T_T_C_ERROR         0x77 // error creating tc            primitive   h-->m
#define T_DATA_LAST         0xA0 // convey data from higher      constructed h<->m
                                 // layers
#define T_DATA_MORE         0xA1 // convey data from higher      constructed h<->m
                                 // layers


struct en50221_connection {
    uint32_t state;                  // the current state: idle/in_delete/in_create/active
    struct timeval tx_time;          // time last request was sent from host->module, or 0 if ok
    struct timeval last_poll_time;   // time of last poll transmission
    int do_poll;                     // indicates whether this slot should be polled
    uint8_t *chain_buffer;           // used to save parts of chained packets
    uint32_t buffer_length;
};

struct en50221_slot {
    int ca_hndl;
    struct en50221_connection *connections;

    pthread_mutex_t slot_lock;

    uint32_t response_timeout;
    uint32_t poll_delay;
};

struct en50221_transport_layer_private
{
    uint8_t max_slots;
    uint8_t max_connections_per_slot;
    struct en50221_slot *slots;
    struct pollfd *slot_pollfds;
    int slots_changed;

    pthread_mutex_t global_lock;
    pthread_mutex_t setcallback_lock;

    int error;
    int error_slot;

    en50221_tl_callback callback;
    void *callback_arg;
};

struct en50221_tpdu {
    uint8_t tpdu_tag;
    uint8_t length_field[3]; // use asn_1_encode/decode to process this field
    int length_field_len; // this stores the length of the length field
    uint8_t connection_id;
    uint8_t *data;
    uint16_t data_length;
};

static int en50221_tl_proc_data_tc(struct en50221_transport_layer_private *tl, uint8_t slot_id,
                                   uint8_t *data, uint32_t data_length);
static int en50221_tl_poll_tc(struct en50221_transport_layer_private *tl, uint8_t slot_id, uint8_t connection_id);
static int en50221_tl_alloc_new_tc(struct en50221_transport_layer_private *tl, uint8_t slot_id);



en50221_transport_layer en50221_tl_create(uint8_t max_slots, uint8_t max_connections_per_slot)
{
    struct en50221_transport_layer_private *private = NULL;
    int i;
    int j;

    // setup structure
    private = (struct en50221_transport_layer_private*) malloc(sizeof(struct en50221_transport_layer_private));
    if (private == NULL)
        goto error_exit;
    private->max_slots = max_slots;
    private->max_connections_per_slot = max_connections_per_slot;
    private->slots = NULL;
    private->slot_pollfds = NULL;
    private->slots_changed = 1;
    private->callback = NULL;
    private->callback_arg = NULL;
    private->error_slot = 0;
    private->error = 0;
    pthread_mutex_init(&private->global_lock, NULL);
    pthread_mutex_init(&private->setcallback_lock, NULL);

    // create the slots
    private->slots = malloc(sizeof(struct en50221_slot) * max_slots);
    if (private->slots == NULL)
        goto error_exit;

    // set them up
    for(i=0; i< max_slots; i++) {
        private->slots[i].ca_hndl = -1;

        // create the connections for this slot
        private->slots[i].connections = malloc(sizeof(struct en50221_connection) * max_connections_per_slot);
        if (private->slots[i].connections == NULL)
            goto error_exit;

        // create a mutex for the slot
        pthread_mutex_init(&private->slots[i].slot_lock, NULL);

        // set them up
        for(j = 0; j < max_connections_per_slot; j++) {
            private->slots[i].connections[j].state = T_STATE_IDLE;
            private->slots[i].connections[j].tx_time.tv_sec = 0;
            private->slots[i].connections[j].last_poll_time.tv_sec = 0;
            private->slots[i].connections[j].last_poll_time.tv_usec = 0;
            private->slots[i].connections[j].do_poll = 0;
            private->slots[i].connections[j].chain_buffer = NULL;
            private->slots[i].connections[j].buffer_length = 0;
        }
    }

    // create the pollfds
    private->slot_pollfds = malloc(sizeof(struct pollfd) * max_slots);
    if (private->slot_pollfds == NULL) {
        goto error_exit;
    }
    memset(private->slot_pollfds, 0, sizeof(struct pollfd) * max_slots);

    return private;

error_exit:
    en50221_tl_destroy(private);
    return NULL;
}

// Destroy an instance of the transport layer
void en50221_tl_destroy(en50221_transport_layer tl)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;
    int i, j;

    if (private) {
        if (private->slots) {
            for(i=0; i< private->max_slots; i++) {
                if (private->slots[i].connections) {
                    for(j=0; j<private->max_connections_per_slot; j++) {
                        if (private->slots[i].connections[j].chain_buffer) {
                            free(private->slots[i].connections[j].chain_buffer);
                        }
                    }
                    free(private->slots[i].connections);
                    pthread_mutex_destroy(&private->slots[i].slot_lock);
                }
            }
            free(private->slots);
        }
        if (private->slot_pollfds) {
            free(private->slot_pollfds);
        }
        pthread_mutex_destroy(&private->setcallback_lock);
        pthread_mutex_destroy(&private->global_lock);
        free(private);
    }
}

// this can be called from the user-space app to
// register new slots that we should work with
int en50221_tl_register_slot(en50221_transport_layer tl, int ca_hndl,
                             uint32_t response_timeout, uint32_t poll_delay)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;

    // lock
    pthread_mutex_lock(&private->global_lock);

    // we browse through the array of slots
    // to look for the first unused one
    int i;
    int16_t slot_id = -1;
    for(i=0; i < private->max_slots; i++) {
        if (private->slots[i].ca_hndl == -1) {
            slot_id = i;
            break;
        }
    }
    if (slot_id == -1) {
        private->error = EN50221ERR_OUTOFSLOTS;
        pthread_mutex_unlock(&private->global_lock);
        return -1;
    }

    // set up the slot struct
    pthread_mutex_lock(&private->slots[slot_id].slot_lock);
    private->slots[slot_id].ca_hndl = ca_hndl;
    private->slots[slot_id].response_timeout = response_timeout;
    private->slots[slot_id].poll_delay = poll_delay;
    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);

    private->slots_changed = 1;
    pthread_mutex_unlock(&private->global_lock);
    return slot_id;
}

void en50221_tl_destroy_slot(en50221_transport_layer tl, uint8_t slot_id)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;
    int i;

    if (slot_id >= private->max_slots)
        return;

    // lock
    pthread_mutex_lock(&private->global_lock);

    // clear the slot
    pthread_mutex_lock(&private->slots[slot_id].slot_lock);
    private->slots[slot_id].ca_hndl = -1;
    for(i=0; i<private->max_connections_per_slot; i++) {
        private->slots[slot_id].connections[i].state = T_STATE_IDLE;
        private->slots[slot_id].connections[i].tx_time.tv_sec = 0;
        private->slots[slot_id].connections[i].last_poll_time.tv_sec = 0;
        private->slots[slot_id].connections[i].last_poll_time.tv_usec = 0;
        private->slots[slot_id].connections[i].do_poll = 0;
        if (private->slots[slot_id].connections[i].chain_buffer) {
            free(private->slots[slot_id].connections[i].chain_buffer);
        }
        private->slots[slot_id].connections[i].chain_buffer = NULL;
        private->slots[slot_id].connections[i].buffer_length = 0;
    }
    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);

    // tell upper layers
    pthread_mutex_lock(&private->setcallback_lock);
    en50221_tl_callback cb = private->callback;
    void *cb_arg = private->callback_arg;
    pthread_mutex_unlock(&private->setcallback_lock);
    if (cb)
        cb(cb_arg, T_CALLBACK_REASON_SLOTCLOSE, NULL, 0, slot_id, 0);

    private->slots_changed = 1;
    pthread_mutex_unlock(&private->global_lock);
}

int en50221_tl_poll(en50221_transport_layer tl)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;
    uint8_t data[4096];
    int slot_id;
    int j;

    // make up pollfds if the slots have changed
    pthread_mutex_lock(&private->global_lock);
    if (private->slots_changed) {
        for(slot_id = 0; slot_id < private->max_slots; slot_id++) {
            if (private->slots[slot_id].ca_hndl != -1) {
                private->slot_pollfds[slot_id].fd = private->slots[slot_id].ca_hndl;
                private->slot_pollfds[slot_id].events = POLLIN|POLLPRI|POLLERR;
                private->slot_pollfds[slot_id].revents = 0;
            } else {
                private->slot_pollfds[slot_id].fd = 0;
                private->slot_pollfds[slot_id].events = 0;
                private->slot_pollfds[slot_id].revents = 0;
            }
        }
        private->slots_changed = 0;
    }
    pthread_mutex_unlock(&private->global_lock);

    // anything happened?
    if (poll(private->slot_pollfds, private->max_slots, 10) < 0) {
        private->error_slot = -1;
        private->error = EN50221ERR_CAREAD;
        return -1;
    }

    // go through all slots (even though poll may not have reported any events
    for(slot_id = 0; slot_id < private->max_slots; slot_id++) {

        // check if this slot is still used and get its handle
        pthread_mutex_lock(&private->slots[slot_id].slot_lock);
        if (private->slots[slot_id].ca_hndl == -1) {
            pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
            continue;
        }
        int ca_hndl = private->slots[slot_id].ca_hndl;

        if (private->slot_pollfds[slot_id].revents & (POLLPRI | POLLIN)) {
            // read data
            uint8_t connection_id;
            int readcnt = dvbca_link_read(ca_hndl, &connection_id, data, sizeof(data));
            if (readcnt < 0) {
                private->error_slot = slot_id;
                private->error = EN50221ERR_CAREAD;
                pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
                return -1;
            }

            // process it if we got some
            if (readcnt > 0) {
                if (en50221_tl_proc_data_tc(private, slot_id, data, readcnt)) {
                    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
                    return -1;
                }
            }
        } else if (private->slot_pollfds[slot_id].revents & POLLERR) {
            // an error was reported
            private->error_slot = slot_id;
            private->error = EN50221ERR_CAREAD;
            pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
            return -1;
        }

        // poll the connections on this slot + check for timeouts
        for(j=0; j < private->max_connections_per_slot; j++) {
            // ignore connection if idle
            if (private->slots[slot_id].connections[j].state == T_STATE_IDLE) {
                continue;
            }

            // poll it
            if (private->slots[slot_id].connections[j].do_poll &&
                (time_after(private->slots[slot_id].connections[j].last_poll_time, private->slots[slot_id].poll_delay))) {

                gettimeofday(&private->slots[slot_id].connections[j].last_poll_time, 0);
                private->slots[slot_id].connections[j].do_poll = 0;
                if (en50221_tl_poll_tc(private, slot_id, j)) {
                    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
                    return -1;
                }
            }

            // check for timeouts
            if (private->slots[slot_id].connections[j].tx_time.tv_sec &&
                (time_after(private->slots[slot_id].connections[j].tx_time, private->slots[slot_id].response_timeout))) {

                if (private->slots[slot_id].connections[j].state & (T_STATE_IN_CREATION|T_STATE_IN_DELETION)) {
                    private->slots[slot_id].connections[j].state = T_STATE_IDLE;
                } else if (private->slots[slot_id].connections[j].state == T_STATE_ACTIVE) {
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_TIMEOUT;
                    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
                    return -1;
                }
            }
        }
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
    }

    return 0;
}

void en50221_tl_register_callback(en50221_transport_layer tl, en50221_tl_callback callback, void *arg)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;

    pthread_mutex_lock(&private->setcallback_lock);
    private->callback = callback;
    private->callback_arg = arg;
    pthread_mutex_unlock(&private->setcallback_lock);
}

int en50221_tl_get_error_slot(en50221_transport_layer tl)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;
    return private->error_slot;
}

int en50221_tl_get_error(en50221_transport_layer tl)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;
    return private->error;
}

int en50221_tl_send_data(en50221_transport_layer tl, uint8_t slot_id, uint8_t connection_id,
                         uint8_t *data, uint32_t data_size)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;

    /*
    printf("[[[[[[[[[[[[[[[[[[[[\n");
    uint32_t ii=0;
    for(ii=0;ii<data_size;ii++) {
        printf("%02x: %02x\n", ii, data[ii]);
    }
    printf("]]]]]]]]]]]]]]]]]]]]\n");
    */

    if (slot_id >= private->max_slots) {
        private->error = EN50221ERR_BADSLOTID;
        return -1;
    }

    pthread_mutex_lock(&private->slots[slot_id].slot_lock);
    if (private->slots[slot_id].ca_hndl == -1) {
        private->error = EN50221ERR_BADSLOTID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    if (connection_id >= private->max_connections_per_slot) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADCONNECTIONID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    if (private->slots[slot_id].connections[connection_id].state != T_STATE_ACTIVE) {
        private->error = EN50221ERR_BADCONNECTIONID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    int ca_hndl = private->slots[slot_id].ca_hndl;

    // build the header
    int length_field_len;
    uint8_t hdr[10];
    hdr[0] = T_DATA_LAST;
    if ((length_field_len = asn_1_encode(data_size + 1, hdr + 1, 3)) < 0) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_ASNENCODE;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    hdr[1 + length_field_len] = connection_id;

    // build the iovs
    struct iovec iov_out[2];
    iov_out[0].iov_base = hdr;
    iov_out[0].iov_len = 1 + length_field_len + 1;
    iov_out[1].iov_base = data;
    iov_out[1].iov_len = data_size;

    // send it!
    if (dvbca_link_writev(ca_hndl, connection_id, iov_out, 2) < 0) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_CAWRITE;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }

    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
    return 0;
}

int en50221_tl_send_datav(en50221_transport_layer tl, uint8_t slot_id, uint8_t connection_id,
                          struct iovec *vector, int iov_count)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;

    /*
    printf("[[[[[[[[[[[[[[[[[[[[\n");
    uint32_t ii=0;
    uint32_t pos=0;
    for(ii=0;ii<iov_count;ii++) {
        uint32_t jj;
        for(jj=0; jj< vector[ii].iov_len; jj++) {
            printf("%02x: %02x\n", jj+pos, *((uint8_t*) (vector[ii].iov_base) +jj));
        }
        pos += vector[ii].iov_len;
    }
    printf("]]]]]]]]]]]]]]]]]]]]\n");
    */

    if (slot_id >= private->max_slots) {
        private->error = EN50221ERR_BADSLOTID;
        return -1;
    }

    pthread_mutex_lock(&private->slots[slot_id].slot_lock);
    if (private->slots[slot_id].ca_hndl == -1) {
        private->error = EN50221ERR_BADSLOTID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    if (connection_id >= private->max_connections_per_slot) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADCONNECTIONID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    if (private->slots[slot_id].connections[connection_id].state != T_STATE_ACTIVE) {
        private->error = EN50221ERR_BADCONNECTIONID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    int ca_hndl = private->slots[slot_id].ca_hndl;

    // calculate the total length of the data to send
    if (iov_count > 9) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_IOVLIMIT;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    uint32_t length = 0;
    int i;
    for(i=0; i< iov_count; i++) {
        length += vector[i].iov_len;
    }

    // build the header
    struct iovec iov_out[10];
    int length_field_len;
    uint8_t hdr[10];
    hdr[0] = T_DATA_LAST;
    if ((length_field_len = asn_1_encode(length + 1, hdr+1, 3)) < 0) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_ASNENCODE;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    hdr[1+length_field_len] = connection_id;
    iov_out[0].iov_base = hdr;
    iov_out[0].iov_len = 1 + length_field_len + 1;

    // build the data
    memcpy(&iov_out[1], vector, iov_count * sizeof(struct iovec));

    // send it!
    if (dvbca_link_writev(ca_hndl, connection_id, iov_out, iov_count+1) < 0) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_CAWRITE;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }

    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
    return 0;
}

int en50221_tl_new_tc(en50221_transport_layer tl, uint8_t slot_id, uint8_t connection_id)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;

    // check
    if (slot_id >= private->max_slots) {
        private->error = EN50221ERR_BADSLOTID;
        return -1;
    }

    pthread_mutex_lock(&private->slots[slot_id].slot_lock);
    if (private->slots[slot_id].ca_hndl == -1) {
        private->error = EN50221ERR_BADSLOTID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    if (connection_id >= private->max_connections_per_slot) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADCONNECTIONID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    // NOTE: We don't really care the state of the connection we transmit the request on

    // allocate a new connection if possible
    int conid = en50221_tl_alloc_new_tc(private, slot_id);
    if (conid == -1) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_OUTOFCONNECTIONS;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }

    // send command
    uint8_t hdr[10];
    hdr[0] = T_CREATE_T_C;
    hdr[1] = 1;
    hdr[2] = conid;
    if (dvbca_link_write(private->slots[slot_id].ca_hndl, connection_id, hdr, 3) < 0) {
        private->slots[slot_id].connections[conid].state = T_STATE_IDLE;
        private->error_slot = slot_id;
        private->error = EN50221ERR_CAWRITE;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    gettimeofday(&private->slots[slot_id].connections[connection_id].tx_time, 0);

    // do not poll until we receive a response from the CAM
    private->slots[slot_id].connections[connection_id].do_poll = 0;

    // done
    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
    return conid;
}

int en50221_tl_del_tc(en50221_transport_layer tl, uint8_t slot_id, uint8_t connection_id)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;

    // check
    if (slot_id >= private->max_slots) {
        private->error = EN50221ERR_BADSLOTID;
        return -1;
    }

    pthread_mutex_lock(&private->slots[slot_id].slot_lock);
    if (private->slots[slot_id].ca_hndl == -1) {
        private->error = EN50221ERR_BADSLOTID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    if (connection_id >= private->max_connections_per_slot) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADCONNECTIONID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    if (!(private->slots[slot_id].connections[connection_id].state &
        (T_STATE_ACTIVE|T_STATE_IN_DELETION))) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADSTATE;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }

    // update the connection state
    private->slots[slot_id].connections[connection_id].state = T_STATE_IN_DELETION;
    if (private->slots[slot_id].connections[connection_id].chain_buffer) {
        free(private->slots[slot_id].connections[connection_id].chain_buffer);
    }
    private->slots[slot_id].connections[connection_id].chain_buffer = NULL;
    private->slots[slot_id].connections[connection_id].buffer_length = 0;

    // send command
    uint8_t hdr[10];
    hdr[0] = T_DELETE_T_C;
    hdr[1] = 1;
    hdr[2] = connection_id;
    if (dvbca_link_write(private->slots[slot_id].ca_hndl, connection_id, hdr, 3) < 0) {
        private->slots[slot_id].connections[connection_id].state = T_STATE_IDLE;
        // fallthrough
    }
    gettimeofday(&private->slots[slot_id].connections[connection_id].tx_time, 0);

    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
    return 0;
}

int en50221_tl_get_connection_state(en50221_transport_layer tl,
                                    uint8_t slot_id, uint8_t connection_id)
{
    struct en50221_transport_layer_private *private = (struct en50221_transport_layer_private *) tl;

    if (slot_id >= private->max_slots) {
        private->error = EN50221ERR_BADSLOTID;
        return -1;
    }

    pthread_mutex_lock(&private->slots[slot_id].slot_lock);
    if (private->slots[slot_id].ca_hndl == -1) {
        private->error = EN50221ERR_BADSLOTID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    if (connection_id >= private->max_connections_per_slot) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADCONNECTIONID;
        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
        return -1;
    }
    int state = private->slots[slot_id].connections[connection_id].state;
    pthread_mutex_unlock(&private->slots[slot_id].slot_lock);

    return state;
}




// ask the module for new data
static int en50221_tl_poll_tc(struct en50221_transport_layer_private *private, uint8_t slot_id, uint8_t connection_id)
{
    gettimeofday(&private->slots[slot_id].connections[connection_id].tx_time, 0);

    // send command
    uint8_t hdr[10];
    hdr[0] = T_DATA_LAST;
    hdr[1] = 1;
    hdr[2] = connection_id;
    if (dvbca_link_write(private->slots[slot_id].ca_hndl, connection_id, hdr, 3) < 0) {
        private->error_slot = slot_id;
        private->error = EN50221ERR_CAWRITE;
        return -1;
    }
    return 0;
}

// handle incoming data
static int en50221_tl_proc_data_tc(struct en50221_transport_layer_private *private, uint8_t slot_id, uint8_t *data, uint32_t data_length)
{
    struct en50221_tpdu units[2];
    int num_units = 1;

    /*
    printf("-------------------\n");
    uint32_t ii=0;
    for(ii=0; ii< data_length; ii++) {
        printf("%02x: %02x\n", ii, data[ii]);
    }
    printf("+++++++++++++++++++\n");
    */

    // first step is parsing the data into a tpdu block
    // the data is not copied, because it's only a
    // 'frontend' to the data-block and won't be needed
    // any longer then the data-block exists
    if (data_length < 1) {
        print(LOG_LEVEL, ERROR, 1, "Received data with invalid length from module on slot %02x\n", slot_id);
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADCAMDATA;
        return -1;
    }
    units[0].tpdu_tag = data[0];
    if ((units[0].length_field_len = asn_1_decode(&units[0].data_length, data + 1, data_length - 1)) < 0) {
        print(LOG_LEVEL, ERROR, 1, "Received data with invalid length from module on slot %02x\n", slot_id);
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADCAMDATA;
        return -1;
    }
    if ((units[0].data_length < 1) ||
        (units[0].data_length > (data_length-1UL-units[0].length_field_len))) {
        print(LOG_LEVEL, ERROR, 1, "Received data with invalid length from module on slot %02x\n", slot_id);
        private->error_slot = slot_id;
        private->error = EN50221ERR_BADCAMDATA;
        return -1;
    }
    units[0].data_length--;
    units[0].connection_id = data[1 + units[0].length_field_len];
    units[0].data = data + 1 + units[0].length_field_len + 1;

    // all R_TPDUs, except from T_SB have a
    // T_SB attached as second unit, parse that, too
    if (units[0].tpdu_tag != T_SB)
    {
        data = data + 1 + units[0].length_field_len + 1 + units[0].data_length;
        data_length -= (1 + units[0].length_field_len + 1 + units[0].data_length);

        if (data_length < 1) {
            print(LOG_LEVEL, ERROR, 1, "Received data with invalid length from module on slot %02x\n", slot_id);
            private->error_slot = slot_id;
            private->error = EN50221ERR_BADCAMDATA;
            return -1;
        }
        units[1].tpdu_tag = data[0];
        if ((units[1].length_field_len = asn_1_decode(&units[1].data_length, data + 1, data_length - 1)) < 0) {
            print(LOG_LEVEL, ERROR, 1, "Received data with invalid length from module on slot %02x\n", slot_id);
            private->error_slot = slot_id;
            private->error = EN50221ERR_BADCAMDATA;
            return -1;
        }
        if ((units[1].data_length < 1) ||
            (units[1].data_length > (data_length-1UL-units[1].length_field_len))) {
            print(LOG_LEVEL, ERROR, 1, "Received data with invalid length from module on slot %02x\n", slot_id);
            private->error_slot = slot_id;
            private->error = EN50221ERR_BADCAMDATA;
            return -1;
        }
        units[1].data_length--;
        units[1].connection_id = data[1 + units[1].length_field_len];
        units[1].data = data + 1 + units[1].length_field_len + 1;
        num_units = 2;
    }

    // process the units
    uint8_t hdr[10];
    int i;
    int conid;
    for(i = 0; i < num_units; i++)
    {
        if (units[i].connection_id >= private->max_connections_per_slot) {
            print(LOG_LEVEL, ERROR, 1, "Received bad connection id %02x from module on slot %02x\n",
                  units[i].connection_id, slot_id);
            private->error_slot = slot_id;
            private->error = EN50221ERR_BADCONNECTIONID;
            return -1;
        }

        switch(units[i].tpdu_tag)
        {
            case T_C_T_C_REPLY:
                // set this connection to state active
                if (private->slots[slot_id].connections[units[i].connection_id].state == T_STATE_IN_CREATION) {
                    private->slots[slot_id].connections[units[i].connection_id].state = T_STATE_ACTIVE;
                    private->slots[slot_id].connections[units[i].connection_id].tx_time.tv_sec = 0;
                    private->slots[slot_id].connections[units[i].connection_id].do_poll = 1;

                    // tell upper layers
                    pthread_mutex_lock(&private->setcallback_lock);
                    en50221_tl_callback cb = private->callback;
                    void *cb_arg = private->callback_arg;
                    pthread_mutex_unlock(&private->setcallback_lock);
                    if (cb)
                        cb(cb_arg, T_CALLBACK_REASON_CONNECTIONOPEN, NULL, 0, slot_id, units[i].connection_id);
                } else {
                    print(LOG_LEVEL, ERROR, 1, "Received T_C_T_C_REPLY for connection not in "
                            "T_STATE_IN_CREATION from module on slot %02x\n", slot_id);
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_BADCAMDATA;
                    return -1;
                }
                break;
            case T_DELETE_T_C:
                // immediately delete this connection and send D_T_C_REPLY
                if (private->slots[slot_id].connections[units[i].connection_id].state & (T_STATE_ACTIVE|T_STATE_IN_DELETION))
                {
                    // clear down the slot
                    private->slots[slot_id].connections[units[i].connection_id].state = T_STATE_IDLE;
                    if (private->slots[slot_id].connections[units[i].connection_id].chain_buffer) {
                        free(private->slots[slot_id].connections[units[i].connection_id].chain_buffer);
                    }
                    private->slots[slot_id].connections[units[i].connection_id].chain_buffer = NULL;
                    private->slots[slot_id].connections[units[i].connection_id].buffer_length = 0;

                    // send the reply
                    hdr[0] = T_D_T_C_REPLY;
                    hdr[1] = 1;
                    hdr[2] = units[i].connection_id;
                    if (dvbca_link_write(private->slots[slot_id].ca_hndl, units[i].connection_id, hdr, 3) < 0) {
                        private->error_slot = slot_id;
                        private->error = EN50221ERR_CAWRITE;
                        return -1;
                    }

                    // tell upper layers
                    pthread_mutex_lock(&private->setcallback_lock);
                    en50221_tl_callback cb = private->callback;
                    void *cb_arg = private->callback_arg;
                    pthread_mutex_unlock(&private->setcallback_lock);
                    if (cb)
                        cb(cb_arg, T_CALLBACK_REASON_CONNECTIONCLOSE, NULL, 0, slot_id, units[i].connection_id);
                }
                else {
                    print(LOG_LEVEL, ERROR, 1, "Received T_DELETE_T_C for inactive connection from module on slot %02x\n",
                          slot_id);
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_BADCAMDATA;
                    return -1;
                }
                break;
            case T_D_T_C_REPLY:
                // delete this connection, should be in T_STATE_IN_DELETION already
                if (private->slots[slot_id].connections[units[i].connection_id].state == T_STATE_IN_DELETION) {
                    private->slots[slot_id].connections[units[i].connection_id].state = T_STATE_IDLE;
                } else {
                    print(LOG_LEVEL, ERROR, 1, "Received T_D_T_C_REPLY received for connection not in "
                            "T_STATE_IN_DELETION from module on slot %02x\n", slot_id);
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_BADCAMDATA;
                    return -1;
                }
                break;
            case T_REQUEST_T_C:
                // allocate a new connection if possible
                conid = en50221_tl_alloc_new_tc(private, slot_id);
                int ca_hndl = private->slots[slot_id].ca_hndl;
                if (conid == -1) {
                    print(LOG_LEVEL, ERROR, 1, "Too many connections requested by module on slot %02x\n", slot_id);
                    private->slots[slot_id].connections[units[i].connection_id].tx_time.tv_sec = 0;
                    private->slots[slot_id].connections[units[i].connection_id].do_poll = 1;

                    // send the error
                    hdr[0] = T_T_C_ERROR;
                    hdr[1] = 2;
                    hdr[2] = units[i].connection_id;
                    hdr[3] = 1;
                    if (dvbca_link_write(ca_hndl, units[i].connection_id, hdr, 4) < 0) {
                        private->error_slot = slot_id;
                        private->error = EN50221ERR_CAWRITE;
                        return -1;
                    }
                } else {
                    private->slots[slot_id].connections[units[i].connection_id].tx_time.tv_sec = 0;
                    private->slots[slot_id].connections[units[i].connection_id].do_poll = 1;

                    // send the new TC
                    hdr[0] = T_NEW_T_C;
                    hdr[1] = 2;
                    hdr[2] = units[i].connection_id;
                    hdr[3] = conid;
                    if (dvbca_link_write(ca_hndl, units[i].connection_id, hdr, 4) < 0) {
                        private->slots[slot_id].connections[conid].state = T_STATE_IDLE;
                        private->error_slot = slot_id;
                        private->error = EN50221ERR_CAWRITE;
                        return -1;
                    }

                    // mark it active
                    private->slots[slot_id].connections[conid].state = T_STATE_ACTIVE;
                    pthread_mutex_lock(&private->setcallback_lock);
                    en50221_tl_callback cb = private->callback;
                    void *cb_arg = private->callback_arg;
                    pthread_mutex_unlock(&private->setcallback_lock);
                    if (cb)
                        cb(cb_arg, T_CALLBACK_REASON_CAMCONNECTIONOPEN, NULL, 0, slot_id, conid);
                }
                break;
            case T_DATA_MORE:
                // connection in correct state?
                if (private->slots[slot_id].connections[units[i].connection_id].state != T_STATE_ACTIVE) {
                    print(LOG_LEVEL, ERROR, 1, "Received T_DATA_MORE for connection not in "
                            "T_STATE_ACTIVE from module on slot %02x\n", slot_id);
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_BADCAMDATA;
                    return -1;
                }

                // a chained data packet is coming in, save
                // it to the buffer and wait for more
                private->slots[slot_id].connections[units[i].connection_id].tx_time.tv_sec = 0;
                private->slots[slot_id].connections[units[i].connection_id].do_poll = 1;
                int new_data_length =
                        private->slots[slot_id].connections[units[i].connection_id].buffer_length +
                        units[i].data_length;
                uint8_t *new_data_buffer =
                        realloc(private->slots[slot_id].connections[units[i].connection_id].chain_buffer,
                                new_data_length);
                if (new_data_buffer == NULL) {
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_OUTOFMEMORY;
                    return -1;
                }
                private->slots[slot_id].connections[units[i].connection_id].chain_buffer = new_data_buffer;

                memcpy(private->slots[slot_id].connections[units[i].connection_id].chain_buffer +
                       private->slots[slot_id].connections[units[i].connection_id].buffer_length,
                       units[i].data, units[i].data_length);
                private->slots[slot_id].connections[units[i].connection_id].buffer_length = new_data_length;

                break;
            case T_DATA_LAST:
                // connection in correct state?
                if (private->slots[slot_id].connections[units[i].connection_id].state != T_STATE_ACTIVE) {
                    print(LOG_LEVEL, ERROR, 1, "Received T_DATA_LAST received for connection not in "
                            "T_STATE_ACTIVE from module on slot %02x\n", slot_id);
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_BADCAMDATA;
                    return -1;
                }

                // last package of a chain or single package comes in
                private->slots[slot_id].connections[units[i].connection_id].tx_time.tv_sec = 0;
                private->slots[slot_id].connections[units[i].connection_id].do_poll = 1;
                if (private->slots[slot_id].connections[units[i].connection_id].chain_buffer == NULL)
                {
                    // single package => dispatch immediately
                    pthread_mutex_lock(&private->setcallback_lock);
                    en50221_tl_callback cb = private->callback;
                    void *cb_arg = private->callback_arg;
                    pthread_mutex_unlock(&private->setcallback_lock);

                    if (cb && units[i].data_length) {
                        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
                        cb(cb_arg, T_CALLBACK_REASON_DATA, units[i].data, units[i].data_length,
                           slot_id, units[i].connection_id);
                        pthread_mutex_lock(&private->slots[slot_id].slot_lock);
                    }
                }
                else
                {
                    int new_data_length =
                            private->slots[slot_id].connections[units[i].connection_id].buffer_length + units[i].data_length;
                    uint8_t *new_data_buffer =
                            realloc(private->slots[slot_id].connections[units[i].connection_id].chain_buffer,
                                    new_data_length);
                    if (new_data_buffer == NULL) {
                        private->error_slot = slot_id;
                        private->error = EN50221ERR_OUTOFMEMORY;
                        return -1;
                    }

                    memcpy(new_data_buffer + private->slots[slot_id].connections[units[i].connection_id].buffer_length,
                           units[i].data, units[i].data_length);

                    // clean the buffer position
                    private->slots[slot_id].connections[units[i].connection_id].chain_buffer = NULL;
                    private->slots[slot_id].connections[units[i].connection_id].buffer_length = 0;

                    // tell the upper layers
                    pthread_mutex_lock(&private->setcallback_lock);
                    en50221_tl_callback cb = private->callback;
                    void *cb_arg = private->callback_arg;
                    pthread_mutex_unlock(&private->setcallback_lock);
                    if (cb && units[i].data_length) {
                        pthread_mutex_unlock(&private->slots[slot_id].slot_lock);
                        cb(cb_arg, T_CALLBACK_REASON_DATA, new_data_buffer, new_data_length,
                           slot_id, units[i].connection_id);
                        pthread_mutex_lock(&private->slots[slot_id].slot_lock);
                    }

                    free(new_data_buffer);
                }
                break;
            case T_SB:
                // is the connection id ok?
                if (private->slots[slot_id].connections[units[i].connection_id].state != T_STATE_ACTIVE) {
                    print(LOG_LEVEL, ERROR, 1, "Received T_SB for connection not in T_STATE_ACTIVE from module on slot %02x\n",
                          slot_id);
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_BADCAMDATA;
                    return -1;
                }

                // did we get enough data in the T_SB?
                if (units[i].data_length != 1) {
                    print(LOG_LEVEL, ERROR, 1, "Recieved T_SB with invalid length from module on slot %02x\n", slot_id);
                    private->error_slot = slot_id;
                    private->error = EN50221ERR_BADCAMDATA;
                    return -1;
                }

                // tell it to send the data if it says there is some
                if (units[i].data[0] & 0x80) {
                    gettimeofday(&private->slots[slot_id].connections[units[i].connection_id].tx_time, 0);
                    private->slots[slot_id].connections[units[i].connection_id].do_poll = 0;
                    int ca_hndl = private->slots[slot_id].ca_hndl;

                    // send the RCV
                    hdr[0] = T_RCV;
                    hdr[1] = 1;
                    hdr[2] = units[i].connection_id;
                    if (dvbca_link_write(ca_hndl, units[i].connection_id, hdr, 3) < 0) {
                        private->error_slot = slot_id;
                        private->error = EN50221ERR_CAWRITE;
                        return -1;
                    }
                } else {
                    private->slots[slot_id].connections[units[i].connection_id].tx_time.tv_sec = 0;
                    private->slots[slot_id].connections[units[i].connection_id].do_poll = 1;
                }

                break;

            default:
                print(LOG_LEVEL, ERROR, 1, "Recieved unexpected TPDU tag %02x from module on slot %02x\n",
                      units[i].tpdu_tag, slot_id);
                private->error_slot = slot_id;
                private->error = EN50221ERR_BADCAMDATA;
                return -1;
        }
    }

    return 0;
}

static int en50221_tl_alloc_new_tc(struct en50221_transport_layer_private *private, uint8_t slot_id)
{
    // we browse through the array of connection
    // types, to look for the first unused one
    int i, conid = -1;
    for(i=1; i < private->max_connections_per_slot; i++) {
        if (private->slots[slot_id].connections[i].state == T_STATE_IDLE) {
            conid = i;
            break;
        }
    }
    if (conid == -1) {
        print(LOG_LEVEL, ERROR, 1, "CREATE_T_C failed: no more connections available\n");
        return -1;
    }

    // set up the connection struct
    private->slots[slot_id].connections[conid].state = T_STATE_IN_CREATION;
    private->slots[slot_id].connections[conid].chain_buffer = NULL;
    private->slots[slot_id].connections[conid].buffer_length = 0;

    return conid;
}
