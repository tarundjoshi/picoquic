/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include <stdlib.h>
#include <string.h>
#include "cc_common.h"

/* Implementation of L4S/Prague, derived from New Reno
 */

/*
 * We implement here the Prague algotithm as a simple modification of New Reno,
 * with the following changes:
 * 
 * - maintain a coefficient "alpha", exponentially smoothed value of "frac", the
 *   fraction of EC/(ECT+ECT1) notifications in previous RTT.
 *       As a slight deviation from the base prague specification, we set
 *       alpha to frac if frac is more than alpha + 0.5. This addresses the
 *       issue of sudden onset of congestion.
 * - modify HyStart to not exit immediately on ECN notification, unless "frac"
 *   is larger than 0.5 (i.e., 512 since computations are fixed point with 10
 *   bits of precision)
 * - use alpha in HyStart, i.e. increase window by (1-alpha)*acked_data instead
 *   of full increase.
 * - use alpha in New Reno: control amount of window increase or decrease, as
 *   in Prague spec.
 * 
 */

/* Observations and issues:
 *
 * Exit hystart one RTT too late. Hystart ends when the first EC markings appear.
 * These are the marks cause by traffic of epoch N-1. The traffic of epoch N
 * is already in flight, will cause congestion and losses. Increasing 
 * the pacing rate or the quantum value does cause an earlier exit from
 * slow start, but the window ends up too small -- maybe due to the
 * redundant loss signal mentioned above.
 * 
 * Window shrinking after idle. There are no data in flight at the beginning
 * of the epoch. The leaky-bucket based pacing allows a quick initial flight
 * to come in. The queue increases, many packets are marked. As a consequence,
 * the window shrinks, even in the absence of losses.
 * 
 * This variant overrides the smoothing if there are sudden onset of marks.
 * Not doing that improves performance, but also causes a sharp increase in
 * the number of losses.
 * 
 * Redundant loss signals. Marks are detected at epoch[N]. Very likely, this 
 * correlates with losses one RTO timer later. The window shrunk once because
 * of the marks, shrinks again when the loss happens -- value is then too low.
 * Something similar happens in the other direction as well. Slow start exits
 * due to increased delays, observed before the end of the epoch. Shortly
 * after that, congestion marks are reported at end of epoch, causing window
 * to shrink further. Same could happen if losses are observed, followed
 * by CE marks.
 * 
 * Correlated CE marks. If CE marks happen at epoch N, the traffic in flight
 * correpond to the old window, before the window is reduced. CE marks will
 * very likely be detected in next window, causing too much reduction. This
 * effect is much reduced if dirctly using "frac" instead of computing "alpha".
 * 
 * L4S threshold is hard to set for the AQM. Too low, and the throughput
 * drops. Too high, and the amount of losses increases too much. In the
 * tests, the threshold is set approximately BDP/4. This may be dues to
 * inefficient solutions of the issues mentioned above.
 * 
 * Current implementation relies on stack to measure "frac". This is probably
 * a bad idea, as the "frac" epoch is not synchronized with other signals,
 * such as exit of hystart, delay detections, or packet losses. It would
 * be better to move that computation inside the "prague" code.
 * 
 * Restarted the implementation using the code from the prague PR. Issue:
 * the callbacks for ECN do not seem to happen! Also, the computation seems
 * to only work by computing "acknowledged data", but the frequency indices
 * correspond to number of marks, not number of bytes.
 */

#include "picoquic_internal.h"
#include <stdlib.h>
#include <string.h>

typedef enum {
    picoquic_prague_alg_slow_start = 0,
    picoquic_prague_alg_congestion_avoidance
} picoquic_prague_alg_state_t;

#define NB_RTT_RENO 4
#define PRAGUE_SHIFT_G 4 /* g = 1/2^4, gain parameter for alpha EWMA */
#define PRAGUE_G_INV (1<<PRAGUE_SHIFT_G)

typedef struct st_picoquic_prague_state_t {
    picoquic_prague_alg_state_t alg_state;
    // double alpha;
    uint64_t alpha_shifted;
    uint64_t alpha;
    uint64_t acked_bytes_ecn;
    uint64_t acked_bytes_total;
    uint64_t loss_cwnd;
    uint64_t residual_ack;
    uint64_t ssthresh;
    uint64_t recovery_start;
    uint64_t min_rtt;
    uint64_t last_rtt[NB_RTT_RENO];

    uint64_t l4s_epoch_send;
    uint64_t l4s_epoch_ect0;
    uint64_t l4s_epoch_ce;
    int nb_rtt;
    
    picoquic_min_max_rtt_t rtt_filter;

    uint64_t flags;
} picoquic_prague_state_t;

void picoquic_prague_init(picoquic_path_t* path_x, uint64_t current_time)
{
    /* Initialize the state of the congestion control algorithm */
    picoquic_prague_state_t* pr_state = (picoquic_prague_state_t*)malloc(sizeof(picoquic_prague_state_t));

    if (pr_state != NULL) {
        memset(pr_state, 0, sizeof(picoquic_prague_state_t));
        path_x->congestion_alg_state = (void*)pr_state;
        pr_state->alg_state = picoquic_prague_alg_slow_start;
        pr_state->ssthresh = (uint64_t)((int64_t)-1);
        pr_state->alpha = 0;
        path_x->cwin = PICOQUIC_CWIN_INITIAL;
    }
    else {
        path_x->congestion_alg_state = NULL;
    }
}

/* The recovery state last 1 RTT, during which parameters will be frozen
 */
static void picoquic_prague_enter_recovery(
    picoquic_cnx_t* cnx,
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    picoquic_prague_state_t* pr_state,
    uint64_t current_time)
{
    pr_state->ssthresh = path_x->cwin / 2;
    if (pr_state->ssthresh < PICOQUIC_CWIN_MINIMUM) {
        pr_state->ssthresh = PICOQUIC_CWIN_MINIMUM;
    }

    if (notification == picoquic_congestion_notification_timeout) {
        path_x->cwin = PICOQUIC_CWIN_MINIMUM;
        pr_state->alg_state = picoquic_prague_alg_slow_start;
    }
    else {
        path_x->cwin = pr_state->ssthresh;
        pr_state->alg_state = picoquic_prague_alg_congestion_avoidance;
    }

    pr_state->recovery_start = current_time;

    pr_state->residual_ack = 0;
    
    picoquic_packet_context_t* pkt_ctx = &cnx->pkt_ctx[picoquic_packet_context_application];

    /* Reset the L3S measurement context to the current value */
    if (cnx->is_multipath_enabled) {
        /* TODO: if the RCID index has changed, reset the counters. */
        picoquic_remote_cnxid_t* r_cid = path_x->p_remote_cnxid;

        if (r_cid != NULL) {
            pkt_ctx = &r_cid->pkt_ctx;
        }
    }
    pr_state->l4s_epoch_send = pkt_ctx->send_sequence;
    pr_state->l4s_epoch_ect0 = pkt_ctx->ecn_ect0_total_remote;
    pr_state->l4s_epoch_ce = pkt_ctx->ecn_ce_total_remote;
    pr_state->alpha = 0;
    pr_state->alpha_shifted = 0;
}


static void picoquic_prague_reset(picoquic_prague_state_t* pr_state)
{
    pr_state->acked_bytes_ecn = 0;
    pr_state->acked_bytes_total = 0;
}

static void picoquic_prague_update_alpha(picoquic_cnx_t* cnx,
    picoquic_path_t* path_x, picoquic_prague_state_t* pr_state, uint64_t nb_bytes_acknowledged, uint64_t current_time)
{
    /* Check the L4S epoch, based on first number sent in previous epoch */
    picoquic_packet_context_t* pkt_ctx = &cnx->pkt_ctx[picoquic_packet_context_application];

    if (cnx->is_multipath_enabled) {
        /* TODO: if the RCID index has changed, reset the counters. */
        picoquic_remote_cnxid_t* r_cid = path_x->p_remote_cnxid;

        if (r_cid != NULL) {
            pkt_ctx = &r_cid->pkt_ctx;
        }
    }

    if (path_x->path_packet_acked_number >= pr_state->l4s_epoch_send) {
        /* The epoch packet has been acked. Time to update alpha. */
        uint64_t frac = 0;
        pr_state->l4s_epoch_send = pkt_ctx->send_sequence;
        uint64_t delta_ect0 = pkt_ctx->ecn_ect0_total_remote - pr_state->l4s_epoch_ect0;
        uint64_t delta_ce = pkt_ctx->ecn_ce_total_remote - pr_state->l4s_epoch_ce;

        if (delta_ce > 0) {
            frac = (delta_ce * 1024) / (delta_ce + delta_ect0);
        }
        else {
            frac = 0;
        }

        if (delta_ce > 0 || delta_ect0 > 0) {
            if (frac > 512) {
                pr_state->alpha = frac;
                pr_state->alpha_shifted = frac << PRAGUE_SHIFT_G;
            }
            else {
                int64_t delta_frac = frac - pr_state->alpha;

                pr_state->alpha_shifted += delta_frac;
                pr_state->alpha = pr_state->alpha_shifted >> PRAGUE_SHIFT_G;
            }
        }

        pr_state->l4s_epoch_send = pkt_ctx->send_sequence;
        pr_state->l4s_epoch_ect0 = pkt_ctx->ecn_ect0_total_remote;
        pr_state->l4s_epoch_ce = pkt_ctx->ecn_ce_total_remote;

        if (delta_ce > 0) {
            if (pr_state->alpha > 512) {
                /* If we got many ECN marks in the last RTT, treat as full on congestion */
                picoquic_prague_enter_recovery(cnx, path_x, picoquic_congestion_notification_ecn_ec, pr_state, current_time);
            }
            else {
                /* If we got ECN marks in the last RTT, update the ssthresh and the CWIN */
                pr_state->loss_cwnd = path_x->cwin;
                uint64_t reduction = (path_x->cwin * pr_state->alpha) / 2048;
                pr_state->ssthresh = path_x->cwin - reduction;
                if (pr_state->ssthresh < PICOQUIC_CWIN_MINIMUM) {
                    pr_state->ssthresh = PICOQUIC_CWIN_MINIMUM;
                }
                uint64_t old_cwin = path_x->cwin;
                path_x->cwin = pr_state->ssthresh;
                pr_state->alg_state = picoquic_prague_alg_congestion_avoidance;

                picoquic_log_app_message(cnx, "Prague alpha: %" PRIu64 ", cwin, was % " PRIu64 " is now % " PRIu64 "\n",
                    pr_state->alpha, old_cwin, path_x->cwin);
            }
        }
    }
}

/* Callback management for Prague
 */
void picoquic_prague_notify(
    picoquic_cnx_t* cnx,
    picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    uint64_t rtt_measurement,
    uint64_t one_way_delay,
    uint64_t nb_bytes_acknowledged,
    uint64_t lost_packet_number,
    uint64_t current_time)
{
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(lost_packet_number);
#endif
    picoquic_prague_state_t* pr_state = (picoquic_prague_state_t*)path_x->congestion_alg_state;

    if (pr_state != NULL) {
        switch (notification) {
        case picoquic_congestion_notification_acknowledgement: {
            if (nb_bytes_acknowledged) {
                pr_state->acked_bytes_total += nb_bytes_acknowledged;
            }
            // Regardless of the alg state, update alpha
            picoquic_prague_update_alpha(cnx, path_x, pr_state, nb_bytes_acknowledged, current_time);
            switch (pr_state->alg_state) {
            case picoquic_prague_alg_slow_start:
                if (path_x->smoothed_rtt <= PICOQUIC_TARGET_RENO_RTT) {
                    path_x->cwin += (nb_bytes_acknowledged * (1024 - pr_state->alpha)) / 1024;
                }
                else {
                    uint64_t delta = nb_bytes_acknowledged;
                    delta *= path_x->smoothed_rtt;
                    delta *= (1024 - pr_state->alpha);
                    delta /= PICOQUIC_TARGET_RENO_RTT;
                    delta /= 1024;
                    path_x->cwin += delta;
                }
                /* if cnx->cwin exceeds SSTHRESH, exit and go to CA */
                if (path_x->cwin >= pr_state->ssthresh) {
                    pr_state->alg_state = picoquic_prague_alg_congestion_avoidance;
                }
                break;
            case picoquic_prague_alg_congestion_avoidance:
            default: {
                uint64_t complete_delta = nb_bytes_acknowledged * path_x->send_mtu + pr_state->residual_ack;
                pr_state->residual_ack = complete_delta % path_x->cwin;
                uint64_t delta = complete_delta / path_x->cwin;
                delta = (delta * (1024 - pr_state->alpha)) / 1024;
                path_x->cwin += delta;
                break;
            }
            }
            break;
        }
        case picoquic_congestion_notification_ecn_ec:
            // picoquic_prague_update_alpha(cnx, path_x, pr_state, nb_bytes_acknowledged, current_time);
            if (pr_state->alg_state == picoquic_prague_alg_slow_start &&
                pr_state->ssthresh == UINT64_MAX) {
                if (path_x->cwin > path_x->send_mtu) {
                    path_x->cwin -= path_x->send_mtu;
                }
                pr_state->ssthresh = path_x->cwin;
                pr_state->alg_state = picoquic_prague_alg_congestion_avoidance;
                path_x->is_ssthresh_initialized = 1;
            }
            break;
        case picoquic_congestion_notification_repeat:
        case picoquic_congestion_notification_timeout:
            /* enter recovery */
            if (current_time - pr_state->recovery_start > path_x->smoothed_rtt) {
                picoquic_prague_enter_recovery(cnx, path_x, notification, pr_state, current_time);
            }
            break;
        case picoquic_congestion_notification_spurious_repeat:
            if (current_time - pr_state->recovery_start < path_x->smoothed_rtt) {
                /* If spurious repeat of initial loss detected,
                 * exit recovery and reset threshold to pre-entry cwin.
                 */
                if (path_x->cwin < 2 * pr_state->ssthresh) {
                    path_x->cwin = 2 * pr_state->ssthresh;
                    pr_state->alg_state = picoquic_prague_alg_congestion_avoidance;
                }
            }
            break;
        case picoquic_congestion_notification_rtt_measurement:
            /* Using RTT increases as signal to get out of initial slow start */
            if (pr_state->alg_state == picoquic_prague_alg_slow_start &&
                pr_state->ssthresh == UINT64_MAX) {

                if (path_x->rtt_min > PICOQUIC_TARGET_RENO_RTT) {
                    uint64_t min_win;

                    if (path_x->rtt_min > PICOQUIC_TARGET_SATELLITE_RTT) {
                        min_win = (uint64_t)((double)PICOQUIC_CWIN_INITIAL * (double)PICOQUIC_TARGET_SATELLITE_RTT / (double)PICOQUIC_TARGET_RENO_RTT);
                    }
                    else {
                        /* Increase initial CWIN for long delay links. */
                        min_win = (uint64_t)((double)PICOQUIC_CWIN_INITIAL * (double)path_x->rtt_min / (double)PICOQUIC_TARGET_RENO_RTT);
                    }
                    if (min_win > path_x->cwin) {
                        path_x->cwin = min_win;
                    }
                }

                if (picoquic_hystart_test(&pr_state->rtt_filter, (cnx->is_time_stamp_enabled) ? one_way_delay : rtt_measurement,
                    cnx->path[0]->pacing_packet_time_microsec, current_time,
                    cnx->is_time_stamp_enabled)) {
                    /* RTT increased too much, get out of slow start! */
                    pr_state->ssthresh = path_x->cwin;
                    pr_state->alg_state = picoquic_prague_alg_congestion_avoidance;
                    path_x->is_ssthresh_initialized = 1;
                }
            }
            break;
        case picoquic_congestion_notification_bw_measurement:
            if (pr_state->alg_state == picoquic_prague_alg_slow_start &&
                pr_state->ssthresh == UINT64_MAX) {
                /* RTT measurements will happen after the bandwidth is estimated */
                uint64_t max_win = path_x->max_bandwidth_estimate * path_x->smoothed_rtt / 1000000;
                uint64_t min_win = max_win /= 2;
                if (path_x->cwin < min_win) {
                    path_x->cwin = min_win;
                }
            }
            break;
        case picoquic_congestion_notification_reset:
            picoquic_prague_reset(pr_state);
            break;
        default:
            /* ignore */
            break;
        }
    }

    /* Compute pacing data */
    picoquic_update_pacing_data(cnx, path_x, pr_state->alg_state == picoquic_prague_alg_slow_start &&
        pr_state->ssthresh == UINT64_MAX);
}

/* Release the state of the congestion control algorithm */
void picoquic_prague_delete(picoquic_path_t* path_x)
{
    if (path_x->congestion_alg_state != NULL) {
        free(path_x->congestion_alg_state);
        path_x->congestion_alg_state = NULL;
    }
}

/* Observe the state of congestion control */

void picoquic_prague_observe(picoquic_path_t* path_x, uint64_t* cc_state, uint64_t* cc_param)
{
    picoquic_prague_state_t* pr_state = (picoquic_prague_state_t*)path_x->congestion_alg_state;
    *cc_state = (uint64_t)pr_state->alg_state;
    *cc_param = (pr_state->ssthresh == UINT64_MAX) ? 0 : pr_state->ssthresh;
}

/* Definition record for the Prague algorithm */

#define PICOQUIC_PRAGUE_ID "prague" 

picoquic_congestion_algorithm_t picoquic_prague_algorithm_struct = {
    PICOQUIC_PRAGUE_ID, PICOQUIC_CC_ALGO_NUMBER_PRAGUE,
    picoquic_prague_init,
    picoquic_prague_notify,
    picoquic_prague_delete,
    picoquic_prague_observe
};

picoquic_congestion_algorithm_t* picoquic_prague_algorithm = &picoquic_prague_algorithm_struct;
