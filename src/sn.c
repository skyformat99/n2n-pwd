/* Supernode for n2n-2.x */

/* (c) 2009 Richard Andrews <andrews@ntop.org> 
 *
 * Contributions by:
 *    Lukasz Taczuk
 *    Struan Bartlett
 */
 #define MYSQL_ENABLE 0
 #if MYSQL_ENABLE

#endif

#include "sql.h"
#include "n2n.h"
#define N2N_SN_LPORT_DEFAULT 7654
#define N2N_SN_PKTBUF_SIZE   2048

#define N2N_SN_MGMT_PORT                5645

char table_ip[] = "n2n_register_ip";
char table_user[] = "n2n_register_user";
//id = userid
char table_ip_descr[] = "ID BIGINT ,  \
                     IP BIGINT NOT NULL PRIMARY KEY ,\
                     ERROR BIGINT DEFAULT 0,\
                     MAC BIGINT, \
                     DEP INT,\
                     update_time timestamp default current_timestamp on update current_timestamp" ;

char table_user_descr[] = " USERID BIGINT NOT NULL PRIMARY KEY ,\
                           PASSWD VARCHAR(20),\
                           IP BIGINT ,\
                           DEP INT,\
                           update_time timestamp default current_timestamp on update current_timestamp";


struct sn_stats
{
    size_t errors;              /* Number of errors encountered. */
    size_t reg_super;           /* Number of REGISTER_SUPER requests received. */
    size_t reg_super_nak;       /* Number of REGISTER_SUPER requests declined. */
    size_t fwd;                 /* Number of messages forwarded. */
    size_t broadcast;           /* Number of messages broadcast to a community. */
    time_t last_fwd;            /* Time when last message was forwarded. */
    time_t last_reg_super;      /* Time when last REGISTER_SUPER was received. */
};

typedef struct sn_stats sn_stats_t;

struct n2n_sn
{
    time_t              start_time;     /* Used to measure uptime. */
    sn_stats_t          stats;
    int                 daemon;         /* If non-zero then daemonise. */
    uint16_t            lport;          /* Local UDP port to bind to. */
    int                 sock;           /* Main socket for UDP traffic with edges. */
    int                 mgmt_sock;      /* management socket. */
    time_t              last_purge;     /* last purge time */
    peer_info_t *		edges[PEER_HASH_TAB_SIZE];          /* Link list of registered edges. */
};

typedef struct n2n_sn n2n_sn_t;


static int try_forward( n2n_sn_t * sss, 
                        const n2n_common_t * cmn,
                        const n2n_mac_t dstMac,
                        const uint8_t * pktbuf,
                        size_t pktsize );

static int try_broadcast( n2n_sn_t * sss, 
                          const n2n_common_t * cmn,
                          const n2n_mac_t srcMac,
                          const uint8_t * pktbuf,
                          size_t pktsize );



/** Initialise the supernode structure */
static int init_sn( n2n_sn_t * sss )
{
#ifdef WIN32
    initWin32();
#endif
    memset( sss, 0, sizeof(n2n_sn_t) );

    sss->daemon = 1; /* By defult run as a daemon. */
    sss->lport = N2N_SN_LPORT_DEFAULT;
    sss->sock = -1;
    sss->mgmt_sock = -1;
    sss->last_purge = 0;
	sglib_hashed_peer_info_t_init(sss->edges);

    return 0; /* OK */
}

/** Deinitialise the supernode structure and deallocate any memory owned by
 *  it. */
static void deinit_sn( n2n_sn_t * sss )
{
    if (sss->sock >= 0)
    {
        closesocket(sss->sock);
    }
    sss->sock=-1;

    if ( sss->mgmt_sock >= 0 )
    {
        closesocket(sss->mgmt_sock);
    }
    sss->mgmt_sock=-1;

    purge_hashed_peer_list_t(sss->edges, 0xffffffff);
}


/** Determine the appropriate lifetime for new registrations.
 *
 *  If the supernode has been put into a pre-shutdown phase then this lifetime
 *  should not allow registrations to continue beyond the shutdown point.
 */
static uint16_t reg_lifetime( n2n_sn_t * sss )
{
    return 30;
}


/** Update the edge table with the details of the edge which contacted the
 *  supernode. */
static int update_edge( n2n_sn_t * sss, 
                        const n2n_REGISTER_SUPER_t * reg,
                        const n2n_community_t community,
                        const n2n_sock_t * sender_sock,
                        time_t now)
{
    macstr_t            mac_buf;
    n2n_sock_str_t      sockbuf;
    struct peer_info *  scan;

    traceEvent( TRACE_DEBUG, "update_edge for %s [%s]",
                macaddr_str( mac_buf, reg->edgeMac ),
                sock_to_cstr( sockbuf, sender_sock ) );

    scan = find_peer_by_mac( sss->edges, reg->edgeMac );

    if ( NULL == scan )
    {
        /* Not known */

        scan = (struct peer_info*)calloc(1, sizeof(struct peer_info)); /* deallocated in purge_expired_registrations */

        memcpy(scan->community_name, community, sizeof(n2n_community_t) );
        memcpy(&(scan->mac_addr), reg->edgeMac, sizeof(n2n_mac_t));
        memcpy(&(scan->sock), sender_sock, sizeof(n2n_sock_t));

        scan->timeout = reg->timeout;
        if(reg->aflags & N2N_AFLAGS_LOCAL_SOCKET) {
            scan->num_sockets = 2;
            scan->sockets = malloc(scan->num_sockets*sizeof(n2n_sock_t));
            scan->sockets[1] = reg->local_sock;
        } else {
            scan->num_sockets = 1;
            scan->sockets = malloc(scan->num_sockets*sizeof(n2n_sock_t));
        }
        scan->sockets[0] = scan->sock;

        /* insert this guy at the head of the edges list */
		sglib_hashed_peer_info_t_add(sss->edges, scan);

        traceEvent( TRACE_INFO, "update_edge created   %s ==> %s",
                    macaddr_str( mac_buf, reg->edgeMac ),
                    sock_to_cstr( sockbuf, sender_sock ) );
    }
    else
    {
        /* Known */
        int num_changes = 0;
        scan->timeout = reg->timeout;
        if (0 != memcmp(community, scan->community_name, sizeof(n2n_community_t)))
        {
            memcpy(scan->community_name, community, sizeof(n2n_community_t) );
            num_changes++;
        }
        if (0 != sock_equal(sender_sock, &(scan->sock) )) {
            memcpy(&(scan->sock), sender_sock, sizeof(n2n_sock_t));
            memcpy(scan->sockets, sender_sock, sizeof(n2n_sock_t));
            num_changes++;
        }
        if (scan->num_sockets == 1) {
            if (reg->aflags & N2N_AFLAGS_LOCAL_SOCKET) {
                scan->num_sockets = 2;
                free(scan->sockets);
                scan->sockets = malloc(scan->num_sockets*sizeof(n2n_sock_t));
                scan->sockets[0] = scan->sock;
                scan->sockets[1] = reg->local_sock;
            }
        } else {
            if (reg->aflags & N2N_AFLAGS_LOCAL_SOCKET) {
                if (0 != sock_equal(&(reg->local_sock), scan->sockets+1 )) {
                    memcpy(scan->sockets+1, &(reg->local_sock), sizeof(n2n_sock_t));
                }
            } else {
                scan->num_sockets = 1;
                free(scan->sockets);
                scan->sockets = malloc(scan->num_sockets*sizeof(n2n_sock_t));
                scan->sockets[0] = scan->sock;
            }
        }
        if (num_changes) {
            traceEvent( TRACE_INFO, "update_edge updated   %s ==> %s",
                        macaddr_str( mac_buf, reg->edgeMac ),
                        sock_to_cstr( sockbuf, sender_sock ) );
        } else {
            traceEvent( TRACE_DEBUG, "update_edge unchanged %s ==> %s",
                        macaddr_str( mac_buf, reg->edgeMac ),
                        sock_to_cstr( sockbuf, sender_sock ) );
        }

    }

    scan->last_seen = now;
    return 0;
}


/** Send a datagram to the destination embodied in a n2n_sock_t.
 *
 *  @return -1 on error otherwise number of bytes sent
 */
static ssize_t sendto_sock(n2n_sn_t * sss, 
                           const n2n_sock_t * sock, 
                           const uint8_t * pktbuf, 
                           size_t pktsize)
{
    n2n_sock_str_t      sockbuf;

    if ( AF_INET == sock->family )
    {
        struct sockaddr_in udpsock;

        udpsock.sin_family = AF_INET;
        udpsock.sin_port = htons( sock->port );
        memcpy( &(udpsock.sin_addr.s_addr), &(sock->addr.v4), IPV4_SIZE );

        traceEvent( TRACE_DEBUG, "sendto_sock %lu to [%s]",
                    pktsize,
                    sock_to_cstr( sockbuf, sock ) );

        return sendto( sss->sock, pktbuf, pktsize, 0, 
                       (const struct sockaddr *)&udpsock, sizeof(struct sockaddr_in) );
    }
    else
    {
        /* AF_INET6 not implemented */
        errno = EAFNOSUPPORT;
        return -1;
    }
}



/** Try to forward a message to a unicast MAC. If the MAC is unknown then
 *  broadcast to all edges in the destination community.
 */
static int try_forward( n2n_sn_t * sss, 
                        const n2n_common_t * cmn,
                        const n2n_mac_t dstMac,
                        const uint8_t * pktbuf,
                        size_t pktsize )
{
    struct peer_info *  scan;
    macstr_t            mac_buf;
    n2n_sock_str_t      sockbuf;

    scan = find_peer_by_mac( sss->edges, dstMac );

    if ( NULL != scan )
    {
        int data_sent_len;
        data_sent_len = sendto_sock( sss, &(scan->sock), pktbuf, pktsize );

        if ( data_sent_len == pktsize )
        {
            ++(sss->stats.fwd);
            traceEvent(TRACE_DEBUG, "unicast %lu to [%s] %s",
                       pktsize,
                       sock_to_cstr( sockbuf, &(scan->sock) ),
                       macaddr_str(mac_buf, scan->mac_addr));
        }
        else
        {
            ++(sss->stats.errors);
            traceEvent(TRACE_ERROR, "unicast %lu to [%s] %s FAILED (%d: %s)",
                       pktsize,
                       sock_to_cstr( sockbuf, &(scan->sock) ),
                       macaddr_str(mac_buf, scan->mac_addr),
                       errno, strerror(errno) );
        }
    }
    else
    {
        traceEvent( TRACE_DEBUG, "try_forward unknown MAC" );

        /* Not a known MAC so drop. */
    }
    
    return 0;
}


/** Try and broadcast a message to all edges in the community.
 *
 *  This will send the exact same datagram to zero or more edges registered to
 *  the supernode.
 */
static int try_broadcast( n2n_sn_t * sss, 
                          const n2n_common_t * cmn,
                          const n2n_mac_t srcMac,
                          const uint8_t * pktbuf,
                          size_t pktsize )
{
    peer_info_t *		ll;
	struct sglib_hashed_peer_info_t_iterator it;
    macstr_t            mac_buf;
    n2n_sock_str_t      sockbuf;

    traceEvent( TRACE_DEBUG, "try_broadcast" );

	for(ll=sglib_hashed_peer_info_t_it_init(&it,sss->edges); ll!=NULL; ll=sglib_hashed_peer_info_t_it_next(&it)) {
        if( 0 == (memcmp(ll->community_name, cmn->community, sizeof(n2n_community_t)) )
            && (0 != memcmp(srcMac, ll->mac_addr, sizeof(n2n_mac_t)) ) )
            /* REVISIT: exclude if the destination socket is where the packet came from. */
        {
            int data_sent_len;
          
            data_sent_len = sendto_sock(sss, &(ll->sock), pktbuf, pktsize);

            if(data_sent_len != pktsize)
            {
                ++(sss->stats.errors);
                traceEvent(TRACE_WARNING, "multicast %lu to [%s] %s failed %s",
                           pktsize,
                           sock_to_cstr( sockbuf, &(ll->sock) ),
                           macaddr_str(mac_buf, ll->mac_addr),
                           strerror(errno));
            }
            else 
            {
                ++(sss->stats.broadcast);
                traceEvent(TRACE_DEBUG, "multicast %lu to [%s] %s",
                           pktsize,
                           sock_to_cstr( sockbuf, &(ll->sock) ),
                           macaddr_str(mac_buf, ll->mac_addr));
            }
        }
    } /* for */
    
    return 0;
}


static int process_mgmt( n2n_sn_t * sss, 
                         const struct sockaddr_in * sender_sock,
                         const uint8_t * mgmt_buf, 
                         size_t mgmt_size,
                         time_t now)
{
    char resbuf[N2N_SN_PKTBUF_SIZE];
    size_t ressize=0;
    ssize_t r;

    traceEvent( TRACE_DEBUG, "process_mgmt" );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "----------------\n" );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "uptime    %lu\n", (now - sss->start_time) );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "edges     %u\n", 
						 (unsigned int)hashed_peer_list_t_size( sss->edges ));

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "errors    %u\n", 
			 (unsigned int)sss->stats.errors );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "reg_sup   %u\n", 
			 (unsigned int)sss->stats.reg_super );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "reg_nak   %u\n", 
			 (unsigned int)sss->stats.reg_super_nak );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "fwd       %u\n",
			 (unsigned int) sss->stats.fwd );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "broadcast %u\n",
			 (unsigned int) sss->stats.broadcast );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "last fwd  %lu sec ago\n", 
			 (long unsigned int)(now - sss->stats.last_fwd) );

    ressize += snprintf( resbuf+ressize, N2N_SN_PKTBUF_SIZE-ressize, 
                         "last reg  %lu sec ago\n",
			 (long unsigned int) (now - sss->stats.last_reg_super) );


    r = sendto( sss->mgmt_sock, resbuf, ressize, 0/*flags*/, 
                (struct sockaddr *)sender_sock, sizeof(struct sockaddr_in) );

    if ( r <= 0 )
    {
        ++(sss->stats.errors);
        traceEvent( TRACE_ERROR, "process_mgmt : sendto failed. %s", strerror(errno) );
    }

    return 0;
}


/** Examine a datagram and determine what to do with it.
 *
 */
static int process_udp( n2n_sn_t * sss, 
                        const struct sockaddr_in * sender_sock,
                        const uint8_t * udp_buf, 
                        size_t udp_size,
                        time_t now)
{
    n2n_common_t        cmn; /* common fields in the packet header */
    n2n_common_t        cmn2; /* common fields of newly generated packets */
    size_t              rem;
    size_t              idx;
    size_t              msg_type;
    uint8_t             from_supernode;
    macstr_t            mac_buf;
    macstr_t            mac_buf2;
    n2n_sock_str_t      sockbuf;
    const uint8_t *     rec_buf; /* either udp_buf or encbuf */
    int                 unicast; /* non-zero if unicast */
    size_t              encx=0;
    uint8_t             encbuf[N2N_SN_PKTBUF_SIZE];
    n2n_ETHFRAMEHDR_t   eth;
    int                 i;

    /* for PACKET packages */
    n2n_PACKET_t                    pkt; 

    /* for QUERY_PEER packages */
    n2n_QUERY_PEER_t                query;
    struct peer_info *              scan;
    n2n_PEER_INFO_t                 pi;

    /* for REGISTER packages */
    n2n_REGISTER_t                  reg;

    /* for REGISTER_SUPER packages */
    n2n_REGISTER_SUPER_t            regs;
    n2n_REGISTER_SUPER_ACK_t        ack;

    traceEvent( TRACE_DEBUG, "process_udp(%lu)", udp_size );

    /* Use decode_common() to determine the kind of packet then process it:
     *
     * REGISTER_SUPER adds an edge and generate a return REGISTER_SUPER_ACK
     *
     * REGISTER, REGISTER_ACK and PACKET messages are forwarded to their
     * destination edge. If the destination is not known then PACKETs are
     * broadcast.
     */

    rem = udp_size; /* Counts down bytes of packet to protect against buffer overruns. */
    idx = 0; /* marches through packet header as parts are decoded. */
    if ( decode_common(&cmn, udp_buf, &rem, &idx) < 0 )
    {
        traceEvent( TRACE_ERROR, "Failed to decode common section" );
        return -1; /* failed to decode packet */
    }

    msg_type = cmn.pc; /* packet code */
    from_supernode= cmn.flags & N2N_FLAGS_FROM_SUPERNODE;

    if ( cmn.ttl < 1 )
    {
        traceEvent( TRACE_WARNING, "Expired TTL" );
        return 0; /* Don't process further */
    }

    --(cmn.ttl); /* The value copied into all forwarded packets. */

    switch(msg_type) {
    case MSG_TYPE_PACKET:
        /* PACKET from one edge to another edge via supernode. */

        /* pkt will be modified in place and recoded to an output of potentially
         * different size due to addition of the socket.*/


        sss->stats.last_fwd=now;
        decode_PACKET( &pkt, &cmn, udp_buf, &rem, &idx );
        decode_ETHFRAMEHDR( &eth, udp_buf+idx );

        unicast = (0 == is_multi_broadcast(eth.dstMac) );

        traceEvent( TRACE_DEBUG, "Rx PACKET (%s) %s -> %s %s",
                    (unicast?"unicast":"multicast"),
                    macaddr_str( mac_buf, eth.srcMac ),
                    macaddr_str( mac_buf2, eth.dstMac ),
                    (from_supernode?"from sn":"local") );

        if ( !from_supernode )
        {
            memcpy( &cmn2, &cmn, sizeof( n2n_common_t ) );

            /* We are going to add socket even if it was not there before */
            cmn2.flags |= N2N_FLAGS_FROM_SUPERNODE;

            rec_buf = encbuf;

            /* Re-encode the header. */
            encode_PACKET( encbuf, &encx, &cmn2, &pkt );

            /* Copy the original payload unchanged */
            encode_buf( encbuf, &encx, (udp_buf + idx), (udp_size - idx ) );
        }
        else
        {
            /* Already from a supernode. Nothing to modify, just pass to
             * destination. */

            traceEvent( TRACE_DEBUG, "Rx PACKET fwd unmodified" );

            rec_buf = udp_buf;
            encx = udp_size;
        }

        /* Common section to forward the final product. */
        if ( unicast )
        {
            try_forward( sss, &cmn, eth.dstMac, rec_buf, encx );
        }
        else
        {
            try_broadcast( sss, &cmn, eth.srcMac, rec_buf, encx );
        }
        break;
    case MSG_TYPE_QUERY_PEER:
        decode_QUERY_PEER( &query, &cmn, udp_buf, &rem, &idx );

        traceEvent( TRACE_DEBUG, "Rx QUERY_PEER from %s for %s",
                    macaddr_str( mac_buf,  query.srcMac ),
                    macaddr_str( mac_buf2, query.targetMac ) );

        scan = find_peer_by_mac( sss->edges, query.targetMac );
        if (scan && 0 == memcmp(cmn.community, scan->community_name,
                                sizeof(n2n_community_t))) {
            cmn2.ttl = N2N_DEFAULT_TTL;
            cmn2.pc = n2n_peer_info;
            cmn2.flags = N2N_FLAGS_FROM_SUPERNODE;
            memcpy( cmn2.community, cmn.community, sizeof(n2n_community_t) );

            pi.aflags = 0;
            pi.timeout = scan->timeout;
            memcpy( pi.mac, query.targetMac, sizeof(n2n_mac_t) );
            for(i=0; i<scan->num_sockets; i++)
                pi.sockets[i] = scan->sockets[i];
            if(scan->num_sockets > 1)
                pi.aflags |= N2N_AFLAGS_LOCAL_SOCKET;

            encode_PEER_INFO( encbuf, &encx, &cmn2, &pi );

            sendto( sss->sock, encbuf, encx, 0, 
                    (struct sockaddr *)sender_sock, sizeof(struct sockaddr_in) );

            traceEvent( TRACE_DEBUG, "Tx PEER_INFO to %s",
                        macaddr_str( mac_buf, query.srcMac ) );
        } else {
            traceEvent( TRACE_DEBUG, "Ignoring QUERY_PEER for unknown edge %s",
                        macaddr_str( mac_buf, query.targetMac ) );
        }

        break;
    case MSG_TYPE_REGISTER:
        /* Forwarding a REGISTER from one edge to the next */


        sss->stats.last_fwd=now;
        decode_REGISTER( &reg, &cmn, udp_buf, &rem, &idx );

        unicast = (0 == is_multi_broadcast(reg.dstMac) );
        
        if ( unicast )
        {
        traceEvent( TRACE_DEBUG, "Rx REGISTER %s -> %s %s",
                    macaddr_str( mac_buf, reg.srcMac ),
                    macaddr_str( mac_buf2, reg.dstMac ),
                    ((cmn.flags & N2N_FLAGS_FROM_SUPERNODE)?"from sn":"local") );

        if ( 0 != (cmn.flags & N2N_FLAGS_FROM_SUPERNODE) )
        {
            memcpy( &cmn2, &cmn, sizeof( n2n_common_t ) );

            /* We are going to add socket even if it was not there before */
            cmn2.flags |= N2N_FLAGS_FROM_SUPERNODE;

            reg.sock.family = AF_INET;
            reg.sock.port = ntohs(sender_sock->sin_port);
            memcpy( reg.sock.addr.v4, &(sender_sock->sin_addr.s_addr), IPV4_SIZE );

            rec_buf = encbuf;

            /* Re-encode the header. */
            encode_REGISTER( encbuf, &encx, &cmn2, &reg );

            /* Copy the original payload unchanged */
            encode_buf( encbuf, &encx, (udp_buf + idx), (udp_size - idx ) );
        }
        else
        {
            /* Already from a supernode. Nothing to modify, just pass to
             * destination. */

            rec_buf = udp_buf;
            encx = udp_size;
        }

        try_forward( sss, &cmn, reg.dstMac, rec_buf, encx ); /* unicast only */
        }
        else
        {
            traceEvent( TRACE_ERROR, "Rx REGISTER with multicast destination" );
        }
        break;
    case MSG_TYPE_REGISTER_ACK:
        traceEvent( TRACE_DEBUG, "Rx REGISTER_ACK (NOT IMPLEMENTED) SHould not be via supernode" );
        break;
    case MSG_TYPE_REGISTER_SUPER:
        /* Edge requesting registration with us.  */
        
        sss->stats.last_reg_super=now;
        ++(sss->stats.reg_super);
        decode_REGISTER_SUPER( &regs, &cmn, udp_buf, &rem, &idx );

        /*do something here */
       // memcmp( regs.account , "10086",strlen() )

    #if 1
        if(NumRow(table_ip,"ID",(char *)regs.account)>50)//一个ID可以带50台设备
            return 0;

        char* sender_ip = inet_ntoa( sender_sock->sin_addr);//网络地址转换
        char* pstr;
        for(pstr= sender_ip ;*pstr != 0 ;pstr++ )
        {
            if(*pstr == '.')
                *pstr = '0';
        }

        if(Find(table_ip,"IP",sender_ip) == 0){//没有记录当前IP，则记录当前IP
            Insert(table_ip,"IP",sender_ip);
        }

        if(Query(table_ip,"ERROR",sender_ip)>12)//查询错误次数，错误大于n，则返回
            return 0;

        if(Find(table_user,"USERID",(char *)regs.account )==0){//账号错误，则返回
            Addup(table_ip,"ERROR",sender_ip);
            return 0;
        }

        if(Query(table_ip,"ID",sender_ip) != atoi((char *)regs.account))//查询当前IP的账号，如果
            Update(table_ip,(char *)regs.account ,sender_ip);
  
    #endif
     
        cmn2.ttl = N2N_DEFAULT_TTL;
        cmn2.pc = n2n_register_super_ack;
        cmn2.flags = N2N_FLAGS_FROM_SUPERNODE;
        memcpy( cmn2.community, cmn.community, sizeof(n2n_community_t) );

        memcpy( &(ack.cookie), &(regs.cookie), sizeof(n2n_cookie_t) );
        memcpy( ack.edgeMac, regs.edgeMac, sizeof(n2n_mac_t) );
        ack.lifetime = reg_lifetime( sss );

        ack.sock.family = AF_INET;
        ack.sock.port = ntohs(sender_sock->sin_port);
        memcpy( ack.sock.addr.v4, &(sender_sock->sin_addr.s_addr), IPV4_SIZE );

        ack.num_sn=0; /* No backup */
        memset( &(ack.sn_bak), 0, sizeof(n2n_sock_t) );

        traceEvent( TRACE_DEBUG, "Rx REGISTER_SUPER for %s [%s]",
                    macaddr_str( mac_buf, regs.edgeMac ),
                    sock_to_cstr( sockbuf, &(ack.sock) ) );

        update_edge( sss, &regs, cmn.community, &(ack.sock), now );

        encode_REGISTER_SUPER_ACK( encbuf, &encx, &cmn2, &ack );

        sendto( sss->sock, encbuf, encx, 0, 
                (struct sockaddr *)sender_sock, sizeof(struct sockaddr_in) );

        traceEvent( TRACE_DEBUG, "Tx REGISTER_SUPER_ACK for %s [%s]",
                    macaddr_str( mac_buf, regs.edgeMac ),
                    sock_to_cstr( sockbuf, &(ack.sock) ) );
        break;
    default:
        /* Not a known message type */
        traceEvent(TRACE_WARNING, "Unable to handle packet type %d: ignored", (signed int)msg_type);
        break;
    }

    return 0;
}


/** Help message to print if the command line arguments are not valid. */
static void exit_help(int argc, char * const argv[])
{
    fprintf( stderr, "%s usage\n", argv[0] );
    fprintf( stderr, "-l <lport>\tSet UDP main listen port to <lport>\n" );

#if defined(N2N_HAVE_DAEMON)
    fprintf( stderr, "-f        \tRun in foreground.\n" );
#endif /* #if defined(N2N_HAVE_DAEMON) */
#ifndef WIN32
    fprintf( stderr, "-u <UID>  \tUser ID (numeric) to use when privileges are dropped.\n");
    fprintf( stderr, "-g <GID>  \tGroup ID (numeric) to use when privileges are dropped.\n");
#endif /* ifndef WIN32 */
    fprintf( stderr, "-v        \tIncrease verbosity. Can be used multiple times.\n" );
    fprintf( stderr, "-h        \tThis help message.\n" );
    fprintf( stderr, "\n" );
    exit(1);
}

static int run_loop( n2n_sn_t * sss );

/* *********************************************** */

static const struct option long_options[] = {
  { "foreground",      no_argument,       NULL, 'f' },
  { "local-port",      required_argument, NULL, 'l' },
  { "help"   ,         no_argument,       NULL, 'h' },
  { "verbose",         no_argument,       NULL, 'v' },
  { NULL,              0,                 NULL,  0  }
};






/** Main program entry point from kernel. */
int main( int argc, char * const argv[] )
{
    n2n_sn_t sss;

#ifndef WIN32
    uid_t   userid=0; /* root is the only guaranteed ID */
    gid_t   groupid=0; /* root is the only guaranteed ID */
#endif

#if 1//MYSQL_ENABLE
    Connection("localhost", "root", "1007030237", "test"); 

    if(Exist(table_ip) == 0)
        Create( table_ip,table_ip_descr );
    if(Exist(table_user)==0)
        Create(table_user,table_user_descr);

#endif
    init_sn( &sss );

    {
        int opt;

        while((opt = getopt_long(argc, argv, "fl:u:g:vh", long_options, NULL)) != -1) 
        {
            switch (opt) 
            {
            case 'l': /* local-port */
                sss.lport = atoi(optarg);
                break;
            case 'f': /* foreground */
                sss.daemon = 0;
                break;
#ifndef WIN32
	    case 'u': /* unprivileged uid */
		{
			userid = atoi(optarg);
			break;
		}
	    case 'g': /* unprivileged uid */
		{
			groupid = atoi(optarg);
			break;
		}
#endif 
            case 'h': /* help */
                exit_help(argc, argv);
                break;
            case 'v': /* verbose */
                ++traceLevel;
                break;
            }
        }
        
    }

#if defined(N2N_HAVE_DAEMON)
    if (sss.daemon)
    {
        useSyslog=1; /* traceEvent output now goes to syslog. */
        if ( -1 == daemon( 0, 0 ) )
        {
            traceEvent( TRACE_ERROR, "Failed to become daemon." );
            exit(-5);
        }
    }
#endif /* #if defined(N2N_HAVE_DAEMON) */

    traceEvent( TRACE_DEBUG, "traceLevel is %d", traceLevel);

    sss.sock = open_socket(sss.lport, 1 /*bind ANY*/ );
    if ( -1 == sss.sock )
    {
        traceEvent( TRACE_ERROR, "Failed to open main socket. %s", strerror(errno) );
        exit(-2);
    }
    else
    {
        traceEvent( TRACE_NORMAL, "supernode is listening on UDP %u (main)", sss.lport );
    }

    sss.mgmt_sock = open_socket(N2N_SN_MGMT_PORT, 0 /* bind LOOPBACK */ );
    if ( -1 == sss.mgmt_sock )
    {
        traceEvent( TRACE_ERROR, "Failed to open management socket. %s", strerror(errno) );
        exit(-2);
    }
    else
    {
        traceEvent( TRACE_NORMAL, "supernode is listening on UDP %u (management)", N2N_SN_MGMT_PORT );
    }

#ifndef WIN32
    if ( (userid != 0) || (groupid != 0 ) ) {
	    traceEvent(TRACE_NORMAL, "Interface up. Dropping privileges to uid=%d, gid=%d",
			    (signed int)userid, (signed int)groupid);

	    /* Finished with the need for root privileges. Drop to unprivileged user. */
	    setreuid( userid, userid );
	    setregid( groupid, groupid );
    }
#endif
    traceEvent(TRACE_NORMAL, "supernode started");

    return run_loop(&sss);
}


/** Long lived processing entry point. Split out from main to simply
 *  daemonisation on some platforms. */
static int run_loop( n2n_sn_t * sss )
{
    uint8_t pktbuf[N2N_SN_PKTBUF_SIZE];
    int keep_running=1;

    sss->start_time = time(NULL);

    while(keep_running) 
    {
        int rc;
        ssize_t bread;
        int max_sock;
        fd_set socket_mask;
        struct timeval wait_time;
        time_t now=0;

        FD_ZERO(&socket_mask);
        max_sock = MAX(sss->sock, sss->mgmt_sock);

        FD_SET(sss->sock, &socket_mask);
        FD_SET(sss->mgmt_sock, &socket_mask);

        wait_time.tv_sec = 10; wait_time.tv_usec = 0;
        rc = select(max_sock+1, &socket_mask, NULL, NULL, &wait_time);

        now = time(NULL);

        if(rc > 0) 
        {
            if (FD_ISSET(sss->sock, &socket_mask)) 
            {
                struct sockaddr_in  sender_sock;
                socklen_t           i;

                i = sizeof(sender_sock);
                bread = recvfrom( sss->sock, pktbuf, N2N_SN_PKTBUF_SIZE, 0/*flags*/,
				  (struct sockaddr *)&sender_sock, (socklen_t*)&i);

                if ( bread < 0 ) /* For UDP bread of zero just means no data (unlike TCP). */
                {
                    /* The fd is no good now. Maybe we lost our interface. */
                    traceEvent( TRACE_ERROR, "recvfrom() failed %d errno %d (%s)", bread, errno, strerror(errno) );
                    keep_running=0;
                    break;
                }

                /* We have a datagram to process */
                if ( bread > 0 )
                {
                    /* And the datagram has data (not just a header) */
                    process_udp( sss, &sender_sock, pktbuf, bread, now );
                }
            }

            if (FD_ISSET(sss->mgmt_sock, &socket_mask)) 
            {
                struct sockaddr_in  sender_sock;
                size_t              i;

                i = sizeof(sender_sock);
                bread = recvfrom( sss->mgmt_sock, pktbuf, N2N_SN_PKTBUF_SIZE, 0/*flags*/,
				  (struct sockaddr *)&sender_sock, (socklen_t*)&i);

                if ( bread <= 0 )
                {
                    traceEvent( TRACE_ERROR, "recvfrom() failed %d errno %d (%s)", bread, errno, strerror(errno) );
                    keep_running=0;
                    break;
                }

                /* We have a datagram to process */
                process_mgmt( sss, &sender_sock, pktbuf, bread, now );
            }
        }
        else
        {
            traceEvent( TRACE_DEBUG, "timeout" );
        }
        if ((now - sss->last_purge) >= PURGE_REGISTRATION_FREQUENCY) {
            hashed_purge_expired_registrations( sss->edges );
            sss->last_purge = now;
        }

    } /* while */

    CloseSql();
    deinit_sn( sss );

    return 0;
}

