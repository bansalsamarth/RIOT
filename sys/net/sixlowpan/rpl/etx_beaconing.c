/*
 * etx_beaconing.c
 *
 *  Created on: Feb 26, 2013
 *      Author: stephan
 */
#include "etx_beaconing.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <hwtimer.h>
#include <vtimer.h>
#include <thread.h>
#include <transceiver.h>

#include "sys/net/sixlowpan/sixlowmac.h"
#include "sys/net/sixlowpan/ieee802154_frame.h"

//prototytpes
static uint8_t etx_count_packet_tx(etx_neighbor_t * candidate);
static void etx_set_packets_received(void);
static bool etx_equal_id(ipv6_addr_t *id1, ipv6_addr_t *id2);

//Buffer
char etx_beacon_buf[ETX_BEACON_STACKSIZE] = { 0 };
char etx_radio_buf[ETX_RADIO_STACKSIZE] = { 0 };
char etx_clock_buf[ETX_CLOCK_STACKSIZE] = { 0 };

uint8_t etx_send_buf[ETX_BUF_SIZE] = { 0 };
uint8_t etx_rec_buf[ETX_BUF_SIZE] = { 0 };

//PIDs
int etx_beacon_pid = 0;
int etx_radio_pid = 0;
int etx_clock_pid = 0;

/*
 * xxx If you get a -Wmissing-braces warning here:
 * A -Wmissing-braces warning at this point is a gcc-bug!
 * Please delete this information once it's fixed
 * See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119
 */
//Message queue for radio
msg_t msg_que[ETX_RCV_QUEUE_SIZE] = { 0 };

/*
 * The counter for the current 'round'. An ETX beacon is sent every ETX_INTERVAL
 * u-seconds and a node computes the ETX value by comparing the the received
 * probes vs the expected probes from a neighbor every ETX_ROUND intervals.
 */
static uint8_t cur_round = 0;

/*
 * If we have not yet reached WINDOW intervals, won't calculate the ETX just yet
 */
static char reached_window = 0;

/*
 * This could (and should) be done differently, once the RPL implementation
 * deals with candidate neighbors in another way than just defining that every
 * possible neighbor we hear from is a parent.
 * Right now, we need to keep track of the ETX values of other nodes without
 * needing them to be in our parent array, so we have another array here in
 * which we put all necessary info for up to ETX_MAX_CANDIDATE_NEIHGBORS
 * candidates.
 *
 * xxx If you get a -Wmissing-braces warning here:
 * A -Wmissing-braces warning at this point is a gcc-bug!
 * Please delete this information once it's fixed
 * See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119
 */
//Candidate array
static etx_neighbor_t candidates[ETX_MAX_CANDIDATE_NEIGHBORS] = { 0 };

/*
 * Each time we send a beacon packet we need to reset some values for the
 * current 'round' (a round being the time between each sent beacon packet).
 *
 * In this time, no packet may be handled, otherwise it could assume values
 * from the last round to count for this round.
 */
mutex_t etx_mutex;
//Transceiver command for sending ETX probes
transceiver_command_t tcmd;

//Message to send probes with
msg_t mesg;

//RPL-address
static ipv6_addr_t * own_address;

static etx_probe_t * etx_get_send_buf(void) {
    return ((etx_probe_t *) &(etx_send_buf[0]));
}
static etx_probe_t * etx_get_rec_buf(void) {
    return ((etx_probe_t *) &(etx_rec_buf[0]));
}

void show_candidates(void) {
    etx_neighbor_t * candidate;
    etx_neighbor_t *end;

    for (candidate = &candidates[0], end = candidates
            + ETX_MAX_CANDIDATE_NEIGHBORS; candidate < end;
            candidate++) {
        if (candidate->used == 0) {
            break;
        }
        printf("Candidates Addr:%d\n"
                "\t cur_etx:%f\n"
                "\t packets_rx:%d\n"
                "\t packets_tx:%d\n"
                "\t used:%d\n", candidate->addr.uint8[ETX_IPV6_LAST_BYTE],
                candidate->cur_etx, candidate->packets_rx,
                etx_count_packet_tx(candidate),
                candidate->used);
    }
}

void etx_init_beaconing(ipv6_addr_t * address) {
    mutex_init(&etx_mutex);
    own_address = address;
    //set code
    puts("ETX BEACON INIT");
    etx_send_buf[0] = ETX_PKT_OPTVAL;

    etx_beacon_pid = thread_create(etx_beacon_buf, ETX_BEACON_STACKSIZE,
            PRIORITY_MAIN - 1, CREATE_STACKTEST,
            etx_beacon, "etx_beacon");

    etx_radio_pid = thread_create(etx_radio_buf, ETX_RADIO_STACKSIZE,
            PRIORITY_MAIN - 1, CREATE_STACKTEST,
            etx_radio, "etx_radio");

    etx_clock_pid = thread_create(etx_clock_buf, ETX_CLOCK_STACKSIZE,
            PRIORITY_MAIN - 1, CREATE_STACKTEST,
            etx_clock, "etx_clock");
    //register at transceiver
    transceiver_register(TRANSCEIVER_CC1100, etx_radio_pid);
    puts("...[DONE]");
}

void etx_beacon(void) {
    /*
     * Sends a message every ETX_INTERVAL +/- a jitter-value (default is 10%) .
     * A correcting variable is needed to stay at a base interval of
     * ETX_INTERVAL between the wakeups. It takes the old jittervalue in account
     * and modifies the time to wait accordingly.
     */
    etx_probe_t * packet = etx_get_send_buf();
    uint8_t p_length = 0;

    /*
     * xxx If you get a -Wmissing-braces warning here:
     * A -Wmissing-braces warning at this point is a gcc-bug!
     * Please delete this information once it's fixed
     * See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119
     */
    ieee_802154_long_t empty_addr = { 0 };

    while (true) {
        thread_sleep();
        mutex_lock(&etx_mutex);
        //Build etx packet
        p_length = 0;
        for (uint8_t i = 0; i < ETX_BEST_CANDIDATES; i++) {
            if (candidates[i].used != 0) {
                packet->data[i * ETX_TUPLE_SIZE] =
                        candidates[i].addr.uint8[ETX_IPV6_LAST_BYTE];
                packet->data[i * ETX_TUPLE_SIZE + ETX_PKT_REC_OFFSET] =
                        etx_count_packet_tx(&candidates[i]);
                p_length = p_length + ETX_PKT_HDR_LEN;
            }
        }
        packet->length = p_length;
        send_ieee802154_frame(&empty_addr, &etx_send_buf[0],
                ETX_DATA_MAXLEN+ETX_PKT_HDR_LEN, 1);
        DEBUG("sent beacon!\n");
        etx_set_packets_received();
        cur_round++;
        if (cur_round == ETX_WINDOW) {
            if (reached_window != 1) {
                //first round is through
                reached_window = 1;
            }
            cur_round = 0;
        }
        mutex_unlock(&etx_mutex,0);
    }
}

etx_neighbor_t * etx_find_candidate(ipv6_addr_t * address) {
    /*
     * find the candidate with address address and returns it, or returns NULL
     * if no candidate having this address was found.
     */
    for (uint8_t i = 0; i < ETX_MAX_CANDIDATE_NEIGHBORS; i++) {
        if (candidates[i].used
                && (etx_equal_id(&candidates[i].addr, address))) {
            return &candidates[i];
        }
    }
    return NULL ;
}

void etx_clock(void) {
    /*
     * Manages the etx_beacon thread to wake up every full second +- jitter
     */

    /*
     * The jittercorrection and jitter variables keep usecond values divided
     * through 1000 to fit into uint8 variables.
     *
     * That is why they are multiplied by 1000 when used for hwtimer_wait.
     */
    uint8_t jittercorrection = ETX_DEF_JIT_CORRECT;
    uint8_t jitter = (uint8_t) (rand() % ETX_JITTER_MOD);

    while (true) {
        thread_wakeup(etx_beacon_pid);

        /*
         * Vtimer is buggy, but I seem to have no hwtimers left, so using this
         * for now.
         */
        vtimer_usleep(
                ((ETX_INTERVAL - ETX_MAX_JITTER)*MS)+ jittercorrection*MS + jitter*MS - ETX_CLOCK_ADJUST);

        //hwtimer_wait(
        //        HWTIMER_TICKS(((ETX_INTERVAL - ETX_MAX_JITTER)*MS) + jittercorrection*MS + jitter*MS - ETX_CLOCK_ADJUST));

        jittercorrection = (ETX_MAX_JITTER) - jitter;
        jitter = (uint8_t) (rand() % ETX_JITTER_MOD);
    }
}

double etx_get_metric(ipv6_addr_t * address) {
    etx_neighbor_t * candidate = etx_find_candidate(address);
    if (candidate != NULL ) {
        if (etx_count_packet_tx(candidate) > 0) {
            //this means the current etx_value is not outdated
            return candidate->cur_etx;
        } else {
            //The last time I received a packet is too long ago to give a
            //good estimate of the etx value
            return 0;
        }
    }
    return 0;
}

etx_neighbor_t * etx_add_candidate(ipv6_addr_t * address) {
    DEBUG("add candidate\n");
    /*
     * Pre-Condition:   etx_add_candidate should only be called when the
     *                  candidate is not yet in the list.
     *                  Otherwise the candidate will be added a second time,
     *                  leading to unknown behavior.
     *
     *      Check if there is still enough space to add this candidate
     *
     *      a)
     *          Space is available:
     *              Add candidate
     *
     *      b)
     *          Space is not available:
     *              ignore new candidate
     *              This shouldn't really happen though, since we have enough
     *              place in the array.
     *
     * Returns the pointer to the candidate if it was added, or a NULL-pointer
     * otherwise.
     */
    etx_neighbor_t * candidate;
    etx_neighbor_t * end;

    for (candidate = &candidates[0], end = candidates
            + ETX_MAX_CANDIDATE_NEIGHBORS; candidate < end;
            candidate++) {
        if (candidate->used) {
            //skip
            continue;
        } else {
            //We still have a free place add the new candidate
            memset(candidate, 0, sizeof(*candidate));
            candidate->addr = *address;
            candidate->cur_etx = 0;
            candidate->packets_rx = 0;
            candidate->used = 1;
            return candidate;
        }
    }
    return NULL ;
}

void etx_handle_beacon(ipv6_addr_t * candidate_address) {
    /*
     * Handle the ETX probe that has been received and update all infos.
     * If the candidate address is unknown, try to add it to my struct.
     */

    DEBUG(
            "ETX beacon package received with following values:\n"
            "\tPackage Option:%x\n"
            "\t   Data Length:%u\n"
            "\tSource Address:%d\n\n", etx_rec_buf[ETX_PKT_OPT], etx_rec_buf[ETX_PKT_LEN],
            candidate_address->uint8[ETX_IPV6_LAST_BYTE]);

    etx_neighbor_t* candidate = etx_find_candidate(candidate_address);
    if (candidate == NULL ) {
        //Candidate was not found in my list, I should add it
        candidate = etx_add_candidate(candidate_address);
        if (candidate == NULL ) {
            puts("[ERROR] Candidate could not get added");
            puts("Increase the constant ETX_MAX_CANDIDATE_NEIHGBORS");
            return;
        }
    }

    //I have received 1 packet from this candidate in this round
    //This value will be reset by etx_update to 0
    candidate->tx_cur_round = 1;

    // If i find my address in this probe, update the packet_rx value for
    // this candidate.
    etx_probe_t * rec_pkt = etx_get_rec_buf();

    for (uint8_t i = 0; i < rec_pkt->length / ETX_TUPLE_SIZE; i++) {
        DEBUG("\tIPv6 short Addr:%u\n"
        "\tPackets f. Addr:%u\n\n", rec_pkt->data[i * ETX_TUPLE_SIZE],
            rec_pkt->data[i * ETX_TUPLE_SIZE + ETX_PKT_REC_OFFSET]);

        if (rec_pkt->data[i * ETX_TUPLE_SIZE]
                == own_address->uint8[ETX_IPV6_LAST_BYTE]) {

            candidate->packets_rx = rec_pkt->data[i * ETX_TUPLE_SIZE
                    + ETX_PKT_REC_OFFSET];
        }
    }

    //Last, update the ETX value for this candidate
    etx_update(candidate);
}

void etx_radio(void) {
    msg_t m;
    radio_packet_t *p;

    ieee802154_frame_t frame;

    msg_init_queue(msg_que, ETX_RCV_QUEUE_SIZE);

    ipv6_addr_t ll_address;
    ipv6_addr_t candidate_addr;

    ipv6_set_ll_prefix(&ll_address);
    ipv6_get_saddr(&candidate_addr, &ll_address);

    while (1) {
        msg_receive(&m);
        if (m.type == PKT_PENDING) {
            p = (radio_packet_t*) m.content.ptr;

            read_802154_frame(p->data, &frame, p->length);

            if (frame.payload[0] == ETX_PKT_OPTVAL) {
                //copy to receive buffer
                memcpy(etx_rec_buf, &frame.payload[0], frame.payload_len);

                //create IPv6 address from radio packet
                //we can do the cast here since rpl nodes can only have addr
                //up to 8 bits
                candidate_addr.uint8[ETX_IPV6_LAST_BYTE] = (uint8_t) p->src;
                //handle the beacon
                mutex_lock(&etx_mutex);
                etx_handle_beacon(&candidate_addr);
                mutex_unlock(&etx_mutex,1);
            }

            p->processing--;
        }
        else if (m.type == ENOBUFFER) {
            puts("Transceiver buffer full");
        }
        else {
            //packet is not for me, whatever
        }
    }
}

void etx_update(etx_neighbor_t * candidate) {
    DEBUG("update!\n");
    /*
     * Update the current ETX value of a candidate
     */
    double d_f;
    double d_r;

    if (reached_window != 1 || candidate == NULL ) {
        //We will wait at least ETX_WINDOW beacons until we decide to
        //calculate an ETX value, so that we have a good estimate
        return;
    }

    /*
     * Calculate d_f (the forward PDR) from ME to this candidate.
     */
    d_f = candidate->packets_rx / (double) ETX_WINDOW;

    /*
     * Calculate d_r (the backwards PDR) from this candidate to ME
     */
    d_r = etx_count_packet_tx(candidate) / (double) ETX_WINDOW;

    /*
     * Calculate the current ETX value for my link to this candidate.
     */
    if (d_f * d_r != 0) {
        candidate->cur_etx = 1 / (d_f * d_r);
    } else {
        candidate->cur_etx = 0;
    }

    DEBUG(
            "Estimated ETX Metric  is %f for candidate w/ addr %d\n"
            "Estimated PDR_forward is %f\n"
            "Estimated PDR_backwrd is %f\n"
            "\n"
            "Received Packets: %d\n"
            "Sent Packets    : %d\n\n",
            candidate->cur_etx, candidate->addr.uint8[ETX_IPV6_LAST_BYTE],
            d_f, d_r, candidate->packets_rx, etx_count_packet_tx(candidate));
}

static uint8_t etx_count_packet_tx(etx_neighbor_t * candidate) {
    /*
     *  Counts the number of packets that were received for this candidate
     *  in the last ETX_WINDOW intervals.
     */
    DEBUG("counting packets");
    uint8_t pkt_count = 0;
    DEBUG("[");
    for (uint8_t i = 0; i < ETX_WINDOW; i++) {
        if (i != cur_round) {
            pkt_count = pkt_count + candidate->packets_tx[i];
#ifdef ENABLE_DEBUG
            DEBUG("%d",candidate->packets_tx[i]);
            if (i < ETX_WINDOW - 1) {
                DEBUG(",");
            }
#endif
        } else {
            //Check if I received something for the current round
            if (candidate->tx_cur_round == 0) {
                //Didn't receive a packet, zero the field and don't add
                candidate->packets_tx[i] = 0;
#ifdef ENABLE_DEBUG
                DEBUG("%d!",candidate->packets_tx[i]);
                if (i < ETX_WINDOW - 1) {
                    DEBUG(",");
                }
#endif
            } else {
                //Add 1 and set field
                pkt_count = pkt_count + 1;
                candidate->packets_tx[i] = 1;
#ifdef ENABLE_DEBUG
                DEBUG("%d!",candidate->packets_tx[i]);
                if (i < ETX_WINDOW - 1) {
                    DEBUG(",");
                }
#endif
            }
        }
    }
    DEBUG("]\n");
    return pkt_count;
}

static void etx_set_packets_received(void) {
    /*
     * Set for all candidates if they received a packet this round or not
     */
    for (uint8_t i = 0; i < ETX_MAX_CANDIDATE_NEIGHBORS; i++) {
        if (candidates[i].used) {
            if (candidates[i].tx_cur_round != 0) {
                candidates[i].packets_tx[cur_round] = 1;
                candidates[i].tx_cur_round = 0;
            }
        }
    }
}

bool etx_equal_id(ipv6_addr_t *id1, ipv6_addr_t *id2){
    for(uint8_t i=0;i<4;i++){
        if(id1->uint32[i] != id2->uint32[i]){
            return false;
        }
    }
    return true;

}