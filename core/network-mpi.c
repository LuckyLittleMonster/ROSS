#include <ross.h>
#include <mpi.h>

// for udp
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <assert.h>

MPI_Comm MPI_COMM_ROSS = MPI_COMM_WORLD;
int custom_communicator = 0;

/**
 * @struct act_q
 * @brief Keeps track of posted send or recv operations.
 */
struct act_q
{
    const char *name;

    tw_event **event_list;    /**< list of event pointers in this queue */
    MPI_Request *req_list;    /**< list of MPI request handles */
    int *idx_list;            /**< indices in this queue of finished operations */
    MPI_Status *status_list;  /**< list of MPI_Status handles */
    unsigned int cur;         /**< index of first open spot in the queue */
};

#define EVENT_TAG 1

#define EVENT_SIZE(e) g_tw_event_msg_sz

// Author: Huanyi Qin
#define USE_UDP 1
#define SOCKET_BUFFER_SIZE 4096
#define INTERFACE_NAME_SIZE 128

static int sockets[SOCKET_BUFFER_SIZE]; // for sockets
static struct sockaddr_in sockets_address[SOCKET_BUFFER_SIZE];
static int local_socket;
static int processes_per_node = 1;
static int send_udp = 0; 
static int recv_udp = 0;
static udp_send_self = 0;


const char INTERFACE_NAME[INTERFACE_NAME_SIZE][INTERFACE_NAME_SIZE] = {
    "em1",
    "ens802f0",
    "ens785f0"
};



static struct act_q posted_sends;
static struct act_q posted_recvs;
static tw_eventq outq;

static unsigned int read_buffer = 16;
static unsigned int send_buffer = 1024;
static int world_size = 1;

static const tw_optdef mpi_opts[] = {
    TWOPT_GROUP("ROSS MPI Kernel"),
    TWOPT_UINT(
        "read-buffer",
        read_buffer,
        "network read buffer size in # of events"),
    TWOPT_UINT(
        "send-buffer",
        send_buffer,
        "network send buffer size in # of events"),
    TWOPT_END()
};

// Forward declarations of functions used in MPI network message processing
static int recv_begin(tw_pe *me);
static void recv_finish(tw_pe *me, tw_event *e, char * buffer);
static int send_begin(tw_pe *me);
static void send_finish(tw_pe *me, tw_event *e, char * buffer);

// Start of implmentation of network processing routines/functions
void tw_comm_set(MPI_Comm comm)
{
    MPI_COMM_ROSS = comm;
    custom_communicator = 1;
}

const tw_optdef *
tw_net_init(int *argc, char ***argv)
{
    int my_rank;
    int initialized;
    MPI_Initialized(&initialized);

    if (!initialized) {
        if (MPI_Init(argc, argv) != MPI_SUCCESS)
            tw_error(TW_LOC, "MPI_Init failed.");
    }

    if (MPI_Comm_rank(MPI_COMM_ROSS, &my_rank) != MPI_SUCCESS)
        tw_error(TW_LOC, "Cannot get MPI_Comm_rank(MPI_COMM_ROSS)");

    g_tw_masternode = 0;
    g_tw_mynode = my_rank;

    // set up the sockets
    if (USE_UDP) {
        int sz;
        if (MPI_Comm_size(MPI_COMM_ROSS, &sz) != MPI_SUCCESS)
            tw_error(TW_LOC, "Cannot get MPI_Comm_size(MPI_COMM_ROSS)");
        if (sz > SOCKET_BUFFER_SIZE)
            tw_error(TW_LOC, "MPI_Comm_size is larger than SOCKET_BUFFER_SIZE");

        if ((local_socket = sockets[my_rank] = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)) < 0)
            tw_error(TW_LOC, "Cannot create socket");

        struct ifreq ifr;
        ifr.ifr_addr.sa_family = AF_INET;
        int fd_ip = 0;
        int i;
        for (i = 0; i < INTERFACE_NAME_SIZE; ++i)
        {
            strncpy(ifr.ifr_name, INTERFACE_NAME[i], IFNAMSIZ-1);
            if (ioctl(sockets[my_rank], SIOCGIFADDR, &ifr) == 0) {
                fd_ip = 1;
                break;
            }
        }

        if (fd_ip != 1)
            tw_error(TW_LOC, "Cannot find ip");

        struct sockaddr_in local_sdr = {0};

        local_sdr.sin_family = AF_INET;
        // local_sdr.sin_addr.s_addr = htonl(INADDR_ANY);
        local_sdr.sin_addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
        local_sdr.sin_port = 0;

        if (bind((sockets[my_rank]), (struct sockaddr *)&local_sdr, sizeof(struct sockaddr_in)) != 0)
            tw_error(TW_LOC, "can't bind server socket");

        socklen_t len = sizeof(struct sockaddr_in);

        if (getsockname((sockets[my_rank]), (struct sockaddr *)&(sockets_address[my_rank]), &len) != 0)
            tw_error(TW_LOC, "can't getsockname");

        printf("process: %d, bind port:%d\n", my_rank, ntohs(sockets_address[my_rank].sin_port));

        MPI_Allreduce(MPI_IN_PLACE, sockets_address, sizeof(struct sockaddr_in) * (SOCKET_BUFFER_SIZE), MPI_CHAR, MPI_BOR, MPI_COMM_ROSS);
    
        MPI_Barrier(MPI_COMM_ROSS);
        if (0)  { 
            // test
                // 1 -> 0
            if (my_rank == 1) {
                int id = 0;
                while (1) {sendto(local_socket, &(id), sizeof(int), 0, 
                            &sockets_address[0], sizeof(struct sockaddr_in));
                id++;
                }//printf("send %s\n", t_msg);
            } 
            if (my_rank == 0) {
                int id;
                int i = 0;
                while (1) {
                if (sizeof(int) == recvfrom(local_socket, &id, sizeof(int), 0, NULL, NULL))
                printf("\r rv: %d -> %d", id, i++);}

        }
        while(1);
    }

    }

    return mpi_opts;
}

/**
 * @brief Initializes queues used for posted sends and receives
 *
 * @param[in] q pointer to the queue to be initialized
 * @param[in] name name of the queue
 */
static void
init_q(struct act_q *q, const char *name)
{
    unsigned int n;

    if(q == &posted_sends)
        n = send_buffer;
    else
        n = read_buffer;

    q->name = name;
    q->event_list = (tw_event **) tw_calloc(TW_LOC, name, sizeof(*q->event_list), n);
    q->req_list = (MPI_Request *) tw_calloc(TW_LOC, name, sizeof(*q->req_list), n);
    q->idx_list = (int *) tw_calloc(TW_LOC, name, sizeof(*q->idx_list), n);
    q->status_list = (MPI_Status *) tw_calloc(TW_LOC, name, sizeof(*q->status_list), n);
}

unsigned int
tw_nnodes(void)
{
    return world_size;
}

void
tw_net_start(void)
{
    if (MPI_Comm_size(MPI_COMM_ROSS, &world_size) != MPI_SUCCESS)
        tw_error(TW_LOC, "Cannot get MPI_Comm_size(MPI_COMM_ROSS)");

    if( g_tw_mynode == 0)
    {
        printf("tw_net_start: Found world size to be %d \n", world_size );
    }

    // Check after tw_nnodes is defined
    if(tw_nnodes() == 1) {
        // force the setting of SEQUENTIAL protocol
        if (g_tw_synchronization_protocol == NO_SYNCH) {
                g_tw_synchronization_protocol = SEQUENTIAL;
        } else if(g_tw_synchronization_protocol == CONSERVATIVE || g_tw_synchronization_protocol == OPTIMISTIC) {
                g_tw_synchronization_protocol = SEQUENTIAL;
                fprintf(stderr, "Warning: Defaulting to Sequential Simulation, not enough PEs defined.\n");
        }
    }

    tw_pe_init();

    //If we're in (some variation of) optimistic mode, we need this hash
    if (g_tw_synchronization_protocol == OPTIMISTIC ||
        g_tw_synchronization_protocol == OPTIMISTIC_DEBUG ||
        g_tw_synchronization_protocol == OPTIMISTIC_REALTIME) 
    {
        g_tw_pe->hash_t = tw_hash_create();
    } else {
        g_tw_pe->hash_t = NULL;
    }

    if (send_buffer < 1)
        tw_error(TW_LOC, "network send buffer must be >= 1");
    if (read_buffer < 1)
        tw_error(TW_LOC, "network read buffer must be >= 1");

    init_q(&posted_sends, "MPI send queue");
    init_q(&posted_recvs, "MPI recv queue");

    g_tw_net_device_size = read_buffer;

    // pre-post all the Irecv operations
    recv_begin(g_tw_pe);
}

void
tw_net_abort(void)
{
    MPI_Abort(MPI_COMM_ROSS, 1);
    exit(1);
}

void
tw_net_stop(void)
{
    if (USE_UDP) {
	//printf("start cal\n");
        //printf("rank: %d, send_udp: %d, recv_udp: %d\n", g_tw_mynode, send_udp, recv_udp);
        MPI_Allreduce(MPI_IN_PLACE, &recv_udp, 1, MPI_INT, MPI_SUM, MPI_COMM_ROSS);
        MPI_Allreduce(MPI_IN_PLACE, &send_udp, 1, MPI_INT, MPI_SUM, MPI_COMM_ROSS);
        // printf("\n");

        MPI_Barrier(MPI_COMM_ROSS);

        if (g_tw_mynode == 0) printf("send_udp: %d, recv_udp: %d\n", send_udp, recv_udp);
    }
#ifdef USE_DAMARIS
    if (g_st_damaris_enabled)
        st_damaris_ross_finalize();
    else
    {
        if (!custom_communicator) {
            if (MPI_Finalize() != MPI_SUCCESS)
                tw_error(TW_LOC, "Failed to finalize MPI");
        }
    }
#else
    if (!custom_communicator) {
        if (MPI_Finalize() != MPI_SUCCESS)
            tw_error(TW_LOC, "Failed to finalize MPI");
    }
#endif
}

void
tw_net_barrier(void)
{
    if (MPI_Barrier(MPI_COMM_ROSS) != MPI_SUCCESS)
        tw_error(TW_LOC, "Failed to wait for MPI_Barrier");
}

tw_stime
tw_net_minimum(void)
{
    tw_stime m = DBL_MAX;
    tw_event *e;
    unsigned int i;

    e = outq.head;
    while (e) {
        if (m > e->recv_ts)
            m = e->recv_ts;
        e = e->next;
    }

    for (i = 0; i < posted_sends.cur; i++) {
        e = posted_sends.event_list[i];
        if (m > e->recv_ts)
            m = e->recv_ts;
    }

    return m;
}

/**
 * @brief Calls MPI_Testsome on the provided queue, to check for finished operations.
 *
 * @param[in] q queue to check
 * @param[in] me pointer to the PE
 * @param[in] finish pointer to function that will perform the appropriate send/recv
 * finish functionality
 *
 * @return 0 if MPI_Testsome did not return any finished operations, 1 otherwise.
 */
static int
test_q(
        struct act_q *q,
        tw_pe *me,
        void (*finish)(tw_pe *, tw_event *, char *))
{
    int ready, i, n;

    if (!q->cur)
        return 0;

    if (MPI_Testsome(
                     q->cur,
                     q->req_list,
                     &ready,
                     q->idx_list,
                     q->status_list) != MPI_SUCCESS) {
        tw_error(
                 TW_LOC,
                 "MPI_testsome failed with %u items in %s",
                 q->cur,
                 q->name);
    }

    if (1 > ready)
        return 0;

    for (i = 0; i < ready; i++)
    {
        tw_event *e;

        n = q->idx_list[i];
        e = q->event_list[n];
        q->event_list[n] = NULL;

        finish(me, e, NULL);
    }

    /* Collapse the lists to remove any holes we left. */
    for (i = 0, n = 0; (unsigned int)i < q->cur; i++)
    {
        if (q->event_list[i])
        {
            if (i != n)
            {
            // swap the event pointers
            q->event_list[n] = q->event_list[i];

            // copy the request handles
            memcpy(
                    &q->req_list[n],
                    &q->req_list[i],
                    sizeof(q->req_list[0]));

            } // endif (i != n)
            n++;
        } // endif (q->event_list[i])
    }
    q->cur -= ready;

    return 1;
}

/**
 * @brief If there are any openings in the posted_recvs queue, post more Irecvs.
 *
 * @param[in] me pointer to the PE
 * @return 0 if no changes are made to the queue, 1 otherwise.
 */
static int
recv_begin(tw_pe *me)
{
    tw_event  *e = NULL;

    int changed = 0;

    while (posted_recvs.cur < read_buffer)
    {
        //printf("recv_begin\n");
        unsigned id = posted_recvs.cur;
        int rv; // debug for udp
	static int queue_udp = 0;

        if (!(e = tw_event_grab(me)))
        {
            if(tw_gvt_inprogress(me))
                    tw_error(TW_LOC, "Out of events in GVT! Consider increasing --extramem");
            return changed;
        }
        //printf("start recv\n");

        if ( USE_UDP && (int)EVENT_SIZE(e) == (rv = recvfrom(local_socket, e, (int)EVENT_SIZE(e), 0,
            NULL, NULL)))
        {
            // printf("recv\n");
            // {
            //     posted_recvs.event_list[id] = e;
            //     posted_recvs.cur++;
            //     changed = 1;
            // }
            // recv_udp++;
            // queue_udp++;
            if (MPI_Isend(e,
                        (int)EVENT_SIZE(e),
                        MPI_BYTE,
                        (int)g_tw_mynode,
                        EVENT_TAG,
                        MPI_COMM_ROSS,
                        MPI_REQUEST_NULL) != MPI_SUCCESS) {
                // error
            }
        }

        if ( MPI_Irecv(e,
                        (int)EVENT_SIZE(e),
                        MPI_BYTE,
                        MPI_ANY_SOURCE,
                        EVENT_TAG,
                        MPI_COMM_ROSS,
                        &posted_recvs.req_list[id]) == MPI_SUCCESS)
        {
            //printf("mpi_irecv\n");
            if (e->send_pe == (tw_peid) g_tw_mynode && udp_send_self > 0) {
                // recv from self
                udp_send_self--;
                
            } else {
                posted_recvs.event_list[id] = e;
                posted_recvs.cur++;
                changed = 1;
            }
            // if () {

            //     queue_udp--;
            // }
            // else {
            //     posted_recvs.event_list[id] = e;
            //     posted_recvs.cur++;
            //     changed = 1;
            // }
        }
        else
        {
        // printf("rv: %d, %d, %s\n", rv, errno, strerror(errno));
            tw_event_free(me, e);
            return changed;
        }

        

        // if( MPI_Irecv(e,
        //                 (int)EVENT_SIZE(e),
        //                 MPI_BYTE,
        //                 MPI_ANY_SOURCE,
        //                 EVENT_TAG,
        //                 MPI_COMM_ROSS,
        //                 &posted_recvs.req_list[id]) != MPI_SUCCESS)
        // {
        //     tw_event_free(me, e);
        //     return changed;
        // }

        // posted_recvs.event_list[id] = e;
        // posted_recvs.cur++;
        // changed = 1;
    }

    return changed;
}

/**
 * @brief Determines how to handle the newly received event.
 *
 * @param[in] me pointer to PE
 * @param[in] e pointer to event that we just received
 * @param[in] buffer not currently used
 */
static void
recv_finish(tw_pe *me, tw_event *e, char * buffer)
{
    (void) buffer;
    tw_pe     *dest_pe;
    tw_clock start;

    me->stats.s_nread_network++;
    me->s_nwhite_recv++;

    //  printf("recv_finish: remote event [cancel %u] FROM: LP %lu, PE %lu, TO: LP %lu, PE %lu at TS %lf \n",
    //     e->state.cancel_q, (tw_lpid)e->src_lp, e->send_pe, (tw_lpid)e->dest_lp, me->id, e->recv_ts);

    e->dest_lp = tw_getlocal_lp((tw_lpid) e->dest_lp);
    dest_pe = e->dest_lp->pe;
    // instrumentation
    e->dest_lp->kp->kp_stats->s_nread_network++;
    e->dest_lp->lp_stats->s_nread_network++;

    if(e->send_pe > tw_nnodes()-1)
        tw_error(TW_LOC, "bad sendpe_id: %d", e->send_pe);

    e->cancel_next = NULL;
    e->caused_by_me = NULL;
    e->cause_next = NULL;



    if(e->recv_ts < me->GVT)
        tw_error(TW_LOC, "%d: Received straggler from %d: %lf (%d)",
                 me->id,  e->send_pe, e->recv_ts, e->state.cancel_q);

    if(tw_gvt_inprogress(me))
        me->trans_msg_ts = ROSS_MIN(me->trans_msg_ts, e->recv_ts);

    // if cancel event, retrieve and flush
    // else, store in hash table
    if(e->state.cancel_q)
    {
        tw_event *cancel = tw_hash_remove(me->hash_t, e, e->send_pe);

        // NOTE: it is possible to cancel the event we
        // are currently processing at this PE since this
        // MPI module lets me read cancel events during
        // event sends over the network.

        cancel->state.cancel_q = 1;
        cancel->state.remote = 0;

        cancel->cancel_next = dest_pe->cancel_q;
        dest_pe->cancel_q = cancel;

        tw_event_free(me, e);

        return;
    }

    if (g_tw_synchronization_protocol == OPTIMISTIC ||
            g_tw_synchronization_protocol == OPTIMISTIC_DEBUG ||
            g_tw_synchronization_protocol == OPTIMISTIC_REALTIME ) {
        tw_hash_insert(me->hash_t, e, e->send_pe);
        e->state.remote = 1;
    }

    /* NOTE: the final check in the if conditional below was added to make sure
     * that we do not execute the fast case unless the cancellation queue is
     * empty on the destination PE.  Otherwise we need to invoke the normal
     * scheduling routines to make sure that a forward event doesn't bypass a
     * cancellation event with an earlier timestamp.  This is helpful for
     * stateful models that produce incorrect results when presented with
     * duplicate messages with no rollback between them.
     */
    if(me == dest_pe && e->dest_lp->kp->last_time <= e->recv_ts && !dest_pe->cancel_q) {
        /* Fast case, we are sending to our own PE and
         * there is no rollback caused by this send.
         */
        start = tw_clock_read();
        tw_pq_enqueue(dest_pe->pq, e);
        dest_pe->stats.s_pq += tw_clock_read() - start;
        return;
    }

    if (me->id == dest_pe->id) {
        /* Slower, but still local send, so put into top
         * of dest_pe->event_q.
         */
        e->state.owner = TW_pe_event_q;
        tw_eventq_push(&dest_pe->event_q, e);
        return;
    }

    /* Never should happen; MPI should have gotten the
     * message to the correct node without needing us
     * to redirect the message there for it.  This is
     * probably a serious bug with the event headers
     * not being formatted right.
     */
    tw_error(
             TW_LOC,
             "Event recived by PE %u but meant for PE %u",
             me->id,
             dest_pe->id);
}

/**
 * @brief If there are any openings in the posted_sends queue, start sends
 * for events in the outgoing queue.
 *
 * @param[in] me pointer to the PE
 * @return 0 if no changes are made to the posted_sends queue, 1 otherwise.
 */
static int
send_begin(tw_pe *me)
{
    int changed = 0;

    while (posted_sends.cur < send_buffer)
    {
    //printf("send_begin\n");
        tw_event *e = tw_eventq_peek(&outq);
        tw_peid dest_pe;

        unsigned id = posted_sends.cur;
        int sd; // debug for udp

        if (!e)
            break;

        if(e == me->abort_event)
            tw_error(TW_LOC, "Sending abort event!");

        dest_pe = (*e->src_lp->type->map) ((tw_lpid) e->dest_lp);

        e->send_pe = (tw_peid) g_tw_mynode;
        e->send_lp = e->src_lp->gid;

        if (e->send_pe / processes_per_node == dest_pe / processes_per_node) {
            // send msg to the same node, use mpi

            if (MPI_Isend(e,
                        (int)EVENT_SIZE(e),
                        MPI_BYTE,
                        (int)dest_pe,
                        EVENT_TAG,
                        MPI_COMM_ROSS,
                        &posted_sends.req_list[id]) != MPI_SUCCESS) {
                return changed;
            }
            
        } else {
            // printf("udp send begin\n");
            // posted_sends.req_list[id] = MPI_REQUEST_NULL;
            if (USE_UDP && (int)EVENT_SIZE(e) == (sd = sendto(local_socket, e, (int)EVENT_SIZE(e), 0, 
                        &sockets_address[(int)dest_pe], sizeof(struct sockaddr_in)))) 
            {
                send_udp++;
                // return changed;
                // printf("send error: %i, %s\n", errno, strerror(errno));
            }
            // send msg to self
            if (MPI_Isend(e,
                        (int)EVENT_SIZE(e),
                        MPI_BYTE,
                        (int)e->send_pe,
                        EVENT_TAG,
                        MPI_COMM_ROSS,
                        &posted_sends.req_list[id]) != MPI_SUCCESS) {
                return changed;
            }
            udp_send_self++;
            // static int count = 3;
            // if (count -- > 0 )
            //     printf("send: changed: %d\n", changed);
            // return changed;
        }

        

        tw_eventq_pop(&outq);
        e->state.owner = e->state.cancel_q
                        ? TW_net_acancel
                        : TW_net_asend;
        
        //if (e->send_pe / processes_per_node == dest_pe / processes_per_node)
    {
        posted_sends.event_list[id] = e;
        posted_sends.cur++;
        me->s_nwhite_sent++;

        changed = 1;
    }
    }
    return changed;
}

/**
 * @brief Determines how to handle the buffer of event whose send operation
 * just finished.
 *
 * @param[in] me pointer to PE
 * @param[in] e pointer to event that we just received
 * @param[in] buffer not currently used
 */
static void
send_finish(tw_pe *me, tw_event *e, char * buffer)
{
    (void) buffer;
    me->stats.s_nsend_network++;
    // instrumentation
    e->src_lp->kp->kp_stats->s_nsend_network++;
    e->src_lp->lp_stats->s_nsend_network++;

    if (e->state.owner == TW_net_asend) {
        if (e->state.cancel_asend) {
            /* Event was cancelled during transmission.  We must
             * send another message to pass the cancel flag to
             * the other node.
             */
            e->state.cancel_asend = 0;
            e->state.cancel_q = 1;
            tw_eventq_push(&outq, e);
        } else {
            /* Event finished transmission and was not cancelled.
             * Add to our sent event queue so we can retain the
             * event in case we need to cancel it later.  Note it
             * is currently in remote format and must be converted
             * back to local format for fossil collection.
             */
            e->state.owner = TW_pe_sevent_q;
            if( g_tw_synchronization_protocol == CONSERVATIVE )
        tw_event_free(me, e);
        }

        return;
    }

    if (e->state.owner == TW_net_acancel) {
        /* We just finished sending the cancellation message
         * for this event.  We need to free the buffer and
         * make it available for reuse.
         */
        tw_event_free(me, e);
        return;
    }

    /* Never should happen, not unless we somehow broke this
     * module's other functions related to sending an event.
     */

    tw_error(
             TW_LOC,
             "Don't know how to finish send of owner=%u, cancel_q=%d",
             e->state.owner,
             e->state.cancel_q);

}

/**
 * @brief Start checks for finished operations in send/recv queues,
 * and post new sends/recvs if possible.
 * @param[in] me pointer to PE
 */
static void
service_queues(tw_pe *me)
{
    int changed;
    do {
    //printf("service_queues\n");
        changed  = test_q(&posted_recvs, me, recv_finish);
    //printf("sq_changed: %d\n", changed);
        changed |= test_q(&posted_sends, me, send_finish);
    //printf("sq_changed: %d\n", changed);
        changed |= recv_begin(me);
    //printf("sq_changed: %d\n", changed);
        changed |= send_begin(me);
    //printf("sq_changed: %d\n", changed);
    } while (changed);
}

/*
 * NOTE: Chris believes that this network layer is too aggressive at
 * reading events out of the network.. so we are modifying the algorithm
 * to only send events when tw_net_send it called, and only read events
 * when tw_net_read is called.
 */
void
tw_net_read(tw_pe *me)
{
    //printf("tw_net_read\n");
    service_queues(me);
}

void
tw_net_send(tw_event *e)
{
    tw_pe * me = e->src_lp->pe;
    int changed = 0;

    e->state.remote = 0;
    e->state.owner = TW_net_outq;
    tw_eventq_unshift(&outq, e);

    do
        {
            changed = test_q(&posted_sends, me, send_finish);
            changed |= send_begin(me);
        } while (changed);
}

void
tw_net_cancel(tw_event *e)
{
    tw_pe *src_pe = e->src_lp->pe;

    switch (e->state.owner) {
    case TW_net_outq:
        /* Cancelled before we could transmit it.  Do not
         * transmit the event and instead just release the
         * buffer back into our own free list.
         */
        tw_eventq_delete_any(&outq, e);
        tw_event_free(src_pe, e);

        return;

        break;

    case TW_net_asend:
        /* Too late.  We've already let MPI start to send
         * this event over the network.  We can't pull it
         * back now without sending another message to do
         * the cancel.
         *
         * Setting the cancel_q flag will signal us to do
         * another message send once the current send of
         * this message is completed.
         */
        e->state.cancel_asend = 1;
        break;

    case TW_pe_sevent_q:
        /* Way late; the event was already sent and is in
         * our sent event queue.  Mark it as a cancel and
         * place it at the front of the outq.
         */
        e->state.cancel_q = 1;
        tw_eventq_unshift(&outq, e);
        break;

    default:
        /* Huh?  Where did you come from?  Why are we being
         * told about you?  We did not send you so we cannot
         * cancel you!
         */
        tw_error(
                 TW_LOC,
                 "Don't know how to cancel event owned by %u",
                 e->state.owner);
    }
    //printf("tw_net_cancel\n");
    service_queues(src_pe);
}

/**
 * tw_net_statistics
 * @brief Function to output the statistics
 * @attention Notice that the MPI_Reduce "count" parameter is greater than one.
 * We are reducing on multiple variables *simultaneously* so if you change
 * this function or the struct tw_statistics, you must update the other.
 **/
tw_statistics   *
tw_net_statistics(tw_pe * me, tw_statistics * s)
{
    if(MPI_Reduce(&(s->s_max_run_time),
                &me->stats.s_max_run_time,
                1,
                MPI_DOUBLE,
                MPI_MAX,
                (int)g_tw_masternode,
                MPI_COMM_ROSS) != MPI_SUCCESS)
        tw_error(TW_LOC, "Unable to reduce statistics!");

    if(MPI_Reduce(&(s->s_net_events),
                &me->stats.s_net_events,
                17,
                MPI_UNSIGNED_LONG_LONG,
                MPI_SUM,
                (int)g_tw_masternode,
                MPI_COMM_ROSS) != MPI_SUCCESS)
        tw_error(TW_LOC, "Unable to reduce statistics!");

    if(MPI_Reduce(&s->s_min_detected_offset,
                &me->stats.s_min_detected_offset,
                1,
                MPI_DOUBLE,
                MPI_MIN,
                (int)g_tw_masternode,
                MPI_COMM_ROSS) != MPI_SUCCESS)
        tw_error(TW_LOC, "Unable to reduce statistics!");

    if(MPI_Reduce(&(s->s_total),
                &me->stats.s_total,
                16,
                MPI_UNSIGNED_LONG_LONG,
                MPI_MAX,
                (int)g_tw_masternode,
                MPI_COMM_ROSS) != MPI_SUCCESS)
        tw_error(TW_LOC, "Unable to reduce statistics!");

        if (MPI_Reduce(&s->s_events_past_end,
                &me->stats.s_events_past_end,
                3,
                MPI_UNSIGNED_LONG_LONG,
                MPI_SUM,
                (int)g_tw_masternode,
                MPI_COMM_ROSS) != MPI_SUCCESS)
        tw_error(TW_LOC, "Unable to reduce statistics!");

#ifdef USE_RIO
        if (MPI_Reduce(&s->s_rio_load,
                        &me->stats.s_rio_load,
                        1,
                        MPI_UNSIGNED_LONG_LONG,
                        MPI_MAX,
                        (int)g_tw_masternode,
                        MPI_COMM_ROSS) != MPI_SUCCESS)
                tw_error(TW_LOC, "Unable to reduce statistics!");
        if (MPI_Reduce(&s->s_rio_lp_init,
                        &me->stats.s_rio_lp_init,
                        1,
                        MPI_UNSIGNED_LONG_LONG,
                        MPI_MAX,
                        (int)g_tw_masternode,
                        MPI_COMM_ROSS) != MPI_SUCCESS)
                tw_error(TW_LOC, "Unable to reduce statistics!");
#endif

    return &me->stats;
}
