/*
 * transport.c
 *
 * CS244a HW#3 (Reliable Transport)
 *
 * This file implements the STCP layer that sits between the
 * mysocket and network layers. You are required to fill in the STCP
 * functionality in this file.
 *
 */


#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"


enum {
    CSTATE_CLOSED,
    CSTATE_LISTEN,
    CSTATE_SYN_SENT,
    CSTATE_SYN_RECEIVED,
    CSTATE_ESTABLISHED,
    CSTATE_FIN_WAIT_1,
    CSTATE_FIN_WAIT_2,
    CSTATE_CLOSING,
    CSTATE_CLOSE_WAIT,
    CSTATE_LAST_ACK,
};

#define WINDOW_SIZE 3072
// 0.1s ~ 10s
#define INIT_RTO 1000000000
#define MIN_RTO   100000000
#define MAX_RTO 10000000000

typedef struct stcp_segment {
    struct stcp_segment *next;

    // retransmit
    struct timespec sent;
    int trans_count;

    // header
    tcp_seq seq;
    tcp_seq ack; // only significant on recv, automatically set on send
    uint8_t flags;

    ssize_t len;
    char data[];
} STCPSegment;

static tcp_seq segment_len(STCPSegment *seg)
{
    return seg->len + (seg->flags & (TH_SYN | TH_FIN) ? 1 : 0);
}

static tcp_seq segment_end(STCPSegment *seg)
{
    return seg->seq + segment_len(seg);
}

static int seq_in(tcp_seq seq, tcp_seq start, tcp_seq end)
{
    if (start <= end)
        return seq >= start && seq <= end;
    else
        return seq >= start || seq <= end;
}

static void segment_dump(STCPSegment *seg)
{
#ifdef DEBUG
    dprintf("<seq:%u:%u>", seg->seq, segment_end(seg));
    if (seg->flags & TH_ACK)
        dprintf("<ack:%u>", seg->ack);
    if (seg->len)
        dprintf("<len:%u>", seg->len);
    dprintf("<ctl=");
    if (seg->flags & TH_FIN)
        dprintf("FIN");
    if (seg->flags & TH_SYN)
        dprintf("SYN");
    if (seg->flags & TH_ACK)
        dprintf("ACK", seg->ack);
    dprintf(">\n");
#endif
}

/* this structure is global to a mysocket descriptor */
typedef struct
{
    int connection_state; // state of the connection (established, etc.)
    tcp_seq initial_sequence_num;

    // reusable buffer
    // Allocating & releasing memory is a PITA, and we're single-threaded
    char buffer[sizeof(STCPHeader) + STCP_MSS];

    // seqs
    tcp_seq snd_una;
    tcp_seq snd_nxt;
    tcp_seq rcv_nxt;

    // queues
    STCPSegment *send_queue;
    STCPSegment *recv_queue;

    // rto state
    int64_t srtt;
    int64_t rttvar;
    int64_t rto;
} context_t;


static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);

// timespec arithmetic
static int64_t ts_to_ns(struct timespec ts)
{
    return ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static struct timespec ns_to_ts(int64_t ns)
{
    return (struct timespec) {
        .tv_sec = ns / 1000000000,
        .tv_nsec = ns % 1000000000,
    };
}
static struct timespec timespec_add(struct timespec a, struct timespec b)
{
    return ns_to_ts(ts_to_ns(a) + ts_to_ns(b));
}

static int enqueue(STCPSegment **queue, STCPSegment *seg)
{
    while (*queue && (*queue)->seq < seg->seq)
        queue = &(*queue)->next;
    if (*queue && (*queue)->seq == seg->seq && segment_len(seg) <= segment_len(*queue))
        // duplicate
        // TODO: timers? coalesce?
        return 0;
    seg->next = *queue;
    *queue = seg;
    return 1;
}

static void free_list(STCPSegment *queue) {
    while (queue) {
        STCPSegment *next = queue->next;
        free(queue);
        queue = next;
    }
}

static void send_segment(mysocket_t sd, context_t *ctx, STCPSegment *seg)
{
    seg->ack = ctx->rcv_nxt;

    clock_gettime(CLOCK_REALTIME, &seg->sent);
    seg->trans_count++;

    dprintf("SEND ");
    segment_dump(seg);

    tcp_seq len = segment_len(seg);
    if (len)
        // nonzero (not just ACK), enqueue
        enqueue(&ctx->send_queue, seg);

    // advance window
    ctx->snd_nxt = seg->seq + len;

    STCPHeader header = {
        .th_seq = seg->seq,
        .th_ack = seg->ack,
        .th_off = sizeof(STCPHeader) / 4,
        .th_flags = seg->flags,
        .th_win = WINDOW_SIZE,
    };
    stcp_network_send(sd, &header, sizeof header, seg->data, seg->len, NULL);

    if (!len)
        // zero len was not queued, free immediately
        free(seg);
}

static STCPSegment *recv_segment(mysocket_t sd, context_t *ctx)
{
    ssize_t packet_len = stcp_network_recv(sd, ctx->buffer, sizeof ctx->buffer);

    STCPHeader *hdr = (STCPHeader *) ctx->buffer;
    ssize_t data_len = packet_len - TCP_DATA_START(hdr);

    STCPSegment *seg = calloc(1, sizeof *seg + data_len);
    seg->seq = hdr->th_seq;
    seg->ack = hdr->th_ack;
    seg->flags = hdr->th_flags;
    seg->len = data_len;
    memcpy(seg->data, ctx->buffer + TCP_DATA_START(ctx->buffer), seg->len);

    dprintf("RECV ");
    segment_dump(seg);
    return seg;
}

// return
static void process_ack(context_t *ctx, tcp_seq ack)
{
    if (!seq_in(ack, ctx->snd_una, ctx->snd_nxt))
        // weird ACK, ignore
        return;

    // acceptable ack, advance window
    ctx->snd_una = ack;
    // pop from retrans queue
    while (ctx->send_queue) {
        tcp_seq end = segment_end(ctx->send_queue);
        if (seq_in(end-1, ctx->snd_una, ctx->snd_nxt))
            break;

        // RTO calculation
        if (ctx->send_queue->trans_count == 1) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            int64_t r = ts_to_ns(now) - ts_to_ns(ctx->send_queue->sent);

            if (ctx->rto == 0) {
                // first RTT
                ctx->srtt = r;
                ctx->rttvar = r / 2;
                ctx->rto = ctx->srtt + MAX(1, 4 * ctx->rttvar);
            } else {
                ctx->rttvar = ctx->rttvar * 3/4 + llabs(ctx->srtt - r) / 4;
                ctx->srtt = ctx->srtt * 7/8 + r / 8;
                ctx->rto = ctx->srtt + MAX(1, 4 * ctx->rttvar);
            }
            ctx->rto = MAX(MIN_RTO, ctx->rto);
            ctx->rto = MIN(MAX_RTO, ctx->rto);
        }

        if (ctx->send_queue->flags & TH_FIN) {
            // our FIN was ACKed
            // NOTE: FIN in FIN-ACK handled in process_data
            if (ctx->connection_state == CSTATE_FIN_WAIT_1)
                ctx->connection_state = CSTATE_FIN_WAIT_2;
            else if (ctx->connection_state == CSTATE_LAST_ACK || ctx->connection_state == CSTATE_CLOSING)
                ctx->connection_state = CSTATE_CLOSED;
        }

        STCPSegment *next = ctx->send_queue->next;
        free(ctx->send_queue);
        ctx->send_queue = next;
    }
}

static void trim_segment(context_t *ctx, STCPSegment *seg) {
    // shift segment start to receive window
    if (!seq_in(seg->seq, ctx->rcv_nxt, ctx->rcv_nxt + WINDOW_SIZE-1)) {
        tcp_seq shift = ctx->rcv_nxt - seg->seq;
        memcpy(seg->data, &seg->data[shift], seg->len - shift);
        seg->seq += shift;
        seg->len -= shift;
    }

    // trim end to receive window
    if (!seq_in(segment_end(seg), ctx->rcv_nxt+1, ctx->rcv_nxt + WINDOW_SIZE)) {
        seg->flags &= ~(TH_SYN | TH_FIN);
        tcp_seq shift = ctx->rcv_nxt + WINDOW_SIZE - segment_end(seg);
        seg->len -= shift;
    }
}

static void process_data(mysocket_t sd, context_t *ctx, STCPSegment *seg)
{
    if (!segment_len(seg)) {
        // nothing to do
        free(seg);
        return;
    }
    // otherwise, we need to at least retransmit an ACK

    if (segment_len(seg) > WINDOW_SIZE) {
        // RFC disallows len > window
        // TODO: exit? or ACK
        free(seg);
        return;
    }

    // either seq or seq+len must be in window
    if (seq_in(seg->seq, ctx->rcv_nxt, ctx->rcv_nxt + WINDOW_SIZE-1) ||
        seq_in(segment_end(seg), ctx->rcv_nxt+1, ctx->rcv_nxt + WINDOW_SIZE)) {
        trim_segment(ctx, seg);
        if (!enqueue(&ctx->recv_queue, seg))
            free(seg);
    }

    while ((seg = ctx->recv_queue)) {
        // might be out of window, if a larger segment was accepted
        if (!seq_in(seg->seq, ctx->rcv_nxt, ctx->rcv_nxt + WINDOW_SIZE-1) &&
            seq_in(segment_end(seg), ctx->rcv_nxt+1, ctx->rcv_nxt + WINDOW_SIZE))
            goto pop_segment;
        if (!seq_in(ctx->rcv_nxt, ctx->recv_queue->seq, segment_end(ctx->recv_queue)-1))
            break;
        trim_segment(ctx, ctx->recv_queue);

        if (ctx->recv_queue->len)
            stcp_app_send(sd, ctx->recv_queue->data, ctx->recv_queue->len);

        // TODO: reject post-FIN data
        if (seg->flags & TH_FIN) {
            stcp_fin_received(sd);
            if (ctx->connection_state == CSTATE_ESTABLISHED)
                // passive close
                ctx->connection_state = CSTATE_CLOSE_WAIT;
            else if (ctx->connection_state == CSTATE_FIN_WAIT_1)
                // simultaneous close
                ctx->connection_state = CSTATE_CLOSING;
            else if (ctx->connection_state == CSTATE_FIN_WAIT_2)
                // peer close in active close
                ctx->connection_state = CSTATE_CLOSED;
        }

        ctx->rcv_nxt = segment_end(ctx->recv_queue);

    pop_segment:
        ctx->recv_queue = seg->next;
        free(seg);
    }

    // ACK, may be retransmission
    seg = calloc(1, sizeof *seg);
    seg->seq = ctx->snd_nxt;
    seg->flags = TH_ACK;
    send_segment(sd, ctx, seg);

    return;
}

/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void transport_init(mysocket_t sd, bool_t is_active)
{
    context_t *ctx = calloc(1, sizeof *ctx);
    assert(ctx);

    generate_initial_seq_num(ctx);
    ctx->snd_una = ctx->initial_sequence_num;
    ctx->snd_nxt = ctx->initial_sequence_num;
    ctx->rcv_nxt = ctx->initial_sequence_num;

    if (is_active) {
        STCPSegment *syn = calloc(1, sizeof *syn);
        syn->seq = ctx->snd_nxt;
        syn->flags = TH_SYN;
        send_segment(sd, ctx, syn);
        ctx->connection_state = CSTATE_SYN_SENT;
    } else {
        ctx->connection_state = CSTATE_LISTEN;
    }

    while (1) {
        struct timespec timeout, *tp = NULL;
        if (ctx->send_queue) {
            timeout = timespec_add(ctx->send_queue->sent, ns_to_ts(ctx->rto ? ctx->rto : INIT_RTO));
            tp = &timeout;
        }
        unsigned int event = stcp_wait_for_event(sd, NETWORK_DATA, tp);

        if (!event) {
            // retransmit
            ctx->rto = 2 * (ctx->rto ? ctx->rto : INIT_RTO);
            STCPSegment *seg = ctx->send_queue;
            while (seg) {
                if (seg->trans_count >= 6) {
                    errno = is_active ? ECONNREFUSED : ECONNABORTED;
                    goto cleanup;
                }
                send_segment(sd, ctx, seg);
                seg = seg->next;
            }
            continue;
        }

        STCPSegment *seg = recv_segment(sd, ctx);
        switch (ctx->connection_state) {
            case CSTATE_LISTEN:
                // SYN
                if (seg->flags != TH_SYN) {
                    free(seg);
                    // ignore, no RST
                    break;
                }

                ctx->rcv_nxt = seg->seq + 1; // seq + SYN
                free(seg);

                // respond SYN-ACK
                seg = calloc(1, sizeof *seg);
                seg->seq = ctx->snd_nxt;
                seg->flags = TH_SYN | TH_ACK;
                send_segment(sd, ctx, seg);
                ctx->connection_state = CSTATE_SYN_RECEIVED;
                break;

            case CSTATE_SYN_SENT:
                // SYN-ACK
                if (seg->flags != (TH_SYN | TH_ACK)) {
                    free(seg);
                    // ignore, no simultaneous open, no RST
                    break;
                }

                process_ack(ctx, seg->ack);
                // did they ACK our syn?
                if (ctx->snd_una != ctx->snd_nxt) {
                    free(seg);
                    // ignore, weird ACK
                    break;
                }

                ctx->rcv_nxt = seg->seq + 1; // seq + SYN
                free(seg);

                // respond ACK
                seg = calloc(1, sizeof *seg);
                seg->seq = ctx->snd_nxt;
                seg->flags = TH_ACK;
                send_segment(sd, ctx, seg);
                ctx->connection_state = CSTATE_ESTABLISHED;
                goto unblock;

            case CSTATE_SYN_RECEIVED:
                // ACK
                if (!(seg->flags & TH_ACK)) {
                    free(seg);
                    // ignore, they lost our SYN-ACK
                    break;
                }

                process_ack(ctx, seg->ack);
                // did they ACK our syn?
                if (ctx->snd_una != ctx->snd_nxt) {
                    free(seg);
                    // ignore, weird ACK
                    break;
                }

                ctx->connection_state = CSTATE_ESTABLISHED;
                // may already contain data, even FIN
                process_data(sd, ctx, seg);
                goto unblock;
        }
    }

unblock:
    stcp_unblock_application(sd);
    control_loop(sd, ctx);

cleanup:
    free_list(ctx->send_queue);
    free_list(ctx->recv_queue);
    free(ctx);
}


/* generate initial sequence number for an STCP connection */
static void generate_initial_seq_num(context_t *ctx)
{
    assert(ctx);
    ctx->initial_sequence_num = 1;
}


/* control_loop() is the main STCP loop; it repeatedly waits for one of the
 * following to happen:
 *   - incoming data from the peer
 *   - new data from the application (via mywrite())
 *   - the socket to be closed (via myclose())
 *   - a timeout
 */
static void control_loop(mysocket_t sd, context_t *ctx)
{
    assert(ctx);

    // It's possible the window is closed before we can send the FIN
    int close_requested = 0;

    while (1) {
        unsigned int event_mask = ANY_EVENT;
        if (ctx->snd_nxt - ctx->snd_una >= WINDOW_SIZE)
            event_mask &= ~APP_DATA;

        switch (ctx->connection_state) {
            case CSTATE_FIN_WAIT_1:
            case CSTATE_FIN_WAIT_2:
            case CSTATE_CLOSING:
                // active close, we can't send any more data
                event_mask &= ~APP_DATA;
                break;
        }

        struct timespec timeout, *tp = NULL;
        if (ctx->send_queue) {
            timeout = timespec_add(ctx->send_queue->sent, ns_to_ts(ctx->rto ? ctx->rto : INIT_RTO));
            tp = &timeout;
        }
        unsigned int event = stcp_wait_for_event(sd, event_mask, tp);

        if (!event) {
            // retransmit
            ctx->rto = 2 * (ctx->rto ? ctx->rto : INIT_RTO);
            STCPSegment *seg = ctx->send_queue;
            while (seg) {
                if (seg->trans_count >= 6) {
                    errno = EPIPE; // TODO: is this right?
                    return;
                }
                send_segment(sd, ctx, seg);
                seg = seg->next;
            }
        }

        if (event & APP_DATA) {
            size_t data_len = MIN(STCP_MSS, WINDOW_SIZE - (ctx->snd_nxt - ctx->snd_una));
            data_len = stcp_app_recv(sd, ctx->buffer, data_len);
            STCPSegment *seg = calloc(1, sizeof *seg + data_len);
            seg->seq = ctx->snd_nxt;
            seg->flags = TH_ACK;
            seg->len = data_len;
            memcpy(seg->data, ctx->buffer, data_len);
            send_segment(sd, ctx, seg);
        }

        if (event & NETWORK_DATA) {
            STCPSegment *seg = recv_segment(sd, ctx);
            if (seg->flags & TH_ACK)
                process_ack(ctx, seg->ack);
            if (segment_len(seg))
                process_data(sd, ctx, seg);
            else
                free(seg);
        }

        if (event & APP_CLOSE_REQUESTED)
            close_requested = 1;
        // wait until we can send the FIN
        if (close_requested && ctx->snd_nxt - ctx->snd_una < WINDOW_SIZE) {
            STCPSegment *fin = calloc(1, sizeof *fin);
            fin->seq = ctx->snd_nxt;
            fin->flags = TH_FIN;
            send_segment(sd, ctx, fin);
            close_requested = 0;

            if (ctx->connection_state == CSTATE_ESTABLISHED)
                ctx->connection_state = CSTATE_FIN_WAIT_1;
            else if (ctx->connection_state == CSTATE_CLOSE_WAIT)
                ctx->connection_state = CSTATE_LAST_ACK;
        }

        if (ctx->connection_state == CSTATE_CLOSED)
            break;
    }
}


/**********************************************************************/
/* our_dprintf
 *
 * Send a formatted message to stdout.
 *
 * format               A printf-style format string.
 *
 * This function is equivalent to a printf, but may be
 * changed to log errors to a file if desired.
 *
 * Calls to this function are generated by the dprintf amd
 * dperror macros in transport.h
 */
void our_dprintf(const char *format,...)
{
    va_list argptr;
    char buffer[1024];

    assert(format);
    va_start(argptr, format);
    vsnprintf(buffer, sizeof(buffer), format, argptr);
    va_end(argptr);
    fputs(buffer, stdout);
    fflush(stdout);
}
