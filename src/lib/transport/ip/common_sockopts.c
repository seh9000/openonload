/* SPDX-License-Identifier: GPL-2.0 */
/* X-SPDX-Copyright-Text: (c) Solarflare Communications Inc */
/**************************************************************************\
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author  stg
**  \brief  getsockopt & setsockopt code commont to all protocols
**   \date  2005/07/19
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/

/*! \cidoxg_lib_transport_ip */

#include "ip_internal.h"
#include "l3xudp_encap.h"
#include <ci/internal/ip_stats.h>
#include <ci/net/sockopts.h>




/* 
 * IP_MTU
 * ------
 *
 * NOTE: This is a linux only sockopt but we have already included the
 *       netinet/in.h header so we cannot blindly include linux/in.h.
 *       Hence we have little choice but to duplicate the definition here.
 */
#define IP_MTU  14
/* Duplicate IPV6_AUTOFLOWLABEL definition from linux/in6.h */
#define IPV6_AUTOFLOWLABEL 70

#define VERB(x)

#ifndef NDEBUG
#define STG_VERB(x) x
#else
#define STG_VERB(x)
#endif

#define REPORT_CASE(sym) case sym:

#ifndef __KERNEL__
#include <limits.h>
#include <net/if.h>

/* Emulate Linux mapping between priority and TOS field */
#include <linux/types.h>
#include <linux/pkt_sched.h>

static unsigned ci_tos2priority[] = {
    /*  0 */ TC_PRIO_BESTEFFORT,
    /*  1 */ TC_PRIO_FILLER,
    /*  2 */ TC_PRIO_BESTEFFORT,
    /*  3 */ TC_PRIO_BESTEFFORT,
    /*  4 */ TC_PRIO_BULK,
    /*  5 */ TC_PRIO_BULK,
    /*  6 */ TC_PRIO_BULK,
    /*  7 */ TC_PRIO_BULK,
    /*  8 */ TC_PRIO_INTERACTIVE,
    /*  9 */ TC_PRIO_INTERACTIVE,
    /* 10 */ TC_PRIO_INTERACTIVE,
    /* 11 */ TC_PRIO_INTERACTIVE,
    /* 12 */ TC_PRIO_INTERACTIVE_BULK,
    /* 13 */ TC_PRIO_INTERACTIVE_BULK,
    /* 14 */ TC_PRIO_INTERACTIVE_BULK,
    /* 15 */ TC_PRIO_INTERACTIVE_BULK
};


int ci_sock_rx_bind2dev(ci_netif* ni, ci_sock_cmn* s, ci_ifid_t ifindex)
{
  cicp_hwport_mask_t hwports = 0;           /* shut up gcc */
  cicp_encap_t encap = {0,};                /* shut up gcc */
  int rc;

  /* Can we accelerate this interface?  If not, best to handover now. */
  rc = oo_cp_find_llap(ni->cplane, ifindex, NULL/*mtu*/, NULL /*tx_hwports*/,
                       &hwports /*rx_hwports*/, NULL/*mac*/, &encap);
  if( rc != 0 ) {
    /* non-Ethernet interface */
    return CI_SOCKET_HANDOVER;
  }
  if( hwports == 0 )
    return CI_SOCKET_HANDOVER;
  if( (hwports & ~ci_netif_get_hwport_mask(ni)) != 0)
    return CI_SOCKET_HANDOVER;

  s->rx_bind2dev_ifindex = ifindex;
  s->rx_bind2dev_hwports = hwports;
  s->rx_bind2dev_vlan = encap.vlan_id;
  ci_ip_cache_invalidate(&s->pkt);
  if( s->b.state == CI_TCP_STATE_UDP )
    /* ?? TODO: replace w ci_udp_invalidate_ip_caches(); */
    ci_ip_cache_invalidate(&SOCK_TO_UDP(s)->ephemeral_pkt);
  return 0;
}


static int ci_sock_bindtodevice(ci_netif* ni, ci_sock_cmn* s,
                                const void* optval, socklen_t optlen)
{
  ci_ifid_t ifindex;
  struct ifreq ifr;

  if( optlen == 0 || ((char*)optval)[0] == '\0' ) {
    /* Unbind. */
    s->cp.so_bindtodevice = CI_IFID_BAD;
    s->rx_bind2dev_ifindex = CI_IFID_BAD;
    /* These don't really need to be initialised, as only significant when
     * rx_bind2dev_ifindex != CI_IFID_BAD.  But makes stackdump output
     * cleaner this way...
     */
    s->rx_bind2dev_hwports = 0;
    s->rx_bind2dev_vlan = 0;
    return 0;
  }

  if( NI_OPTS(ni).bindtodevice_handover )
    goto handover;

  /* Find the ifindex of the interface. */
  memset(&ifr, 0, sizeof(ifr));
  memcpy(ifr.ifr_name, optval, CI_MIN(optlen, sizeof(ifr.ifr_name)));
  ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';
  ifindex = if_nametoindex(ifr.ifr_name);
  if( ifindex == 0 ) {
    /* Unexpected, because it worked when we applied this sockopt to the
     * kernel socket.  (Although don't forget that some sockets do not have
     * a kernel socket).
     */
    LOG_E(ci_log("%s: ERROR: if_nametoindex(%s) failed",
                 __FUNCTION__, ifr.ifr_name));
    return -ENODEV;
  }

  /* We set cp.so_bindtodevice even if we will fail later, because "fail"
   * means "hand over" here.  In case of real handover, it is harmless to
   * set cp.so_bindtodevice.  In complicated cases, such as TCP listen,
   * we want to remember this value and report it via getsockopt(). */
  s->cp.so_bindtodevice = ifindex;
  return ci_sock_rx_bind2dev(ni, s, ifindex);

 handover:
  /* You might be tempted to think "Handing a connected TCP socket to the
   * kernel is a bad idea, because we'll remove filters, so the kernel
   * stack will see packets for a socket it doesn't know about and reply
   * with RST."
   *
   * True, but that is exactly what will happen if we keep this socket in
   * Onload.  If packets arrive at an Onload interface, Onload will reply
   * with RST.  If packets arrive at a non-Onload interface, kernel will
   * reply with RST.  There is nothing we can do to improve on this.
   */
  return CI_SOCKET_HANDOVER;
}



/* Get OS socket option value */
ci_inline int
ci_get_os_sockopt(ci_fd_t fd, int level, int optname, void *optval,
                  socklen_t *optlen )
{
  int rc;
  ci_fd_t os_sock = ci_get_os_sock_fd(fd);

  if (CI_IS_VALID_SOCKET(os_sock) ) { 
    rc = ci_sys_getsockopt(os_sock, level, optname, optval, optlen);
    ci_rel_os_sock_fd(os_sock);
    if (rc != 0)
      RET_WITH_ERRNO(errno);
    return 0;
  } else {
    /* Caller should care about this case if necessary. */
    RET_WITH_ERRNO(ENOPROTOOPT); 
  }
}
#endif /* ifndef __KERNEL__ */

/*
 * The handlers in this module must conform to the following:
 * 1. Be common to all protocols
 * 2. Not be performance critical
 *
 * Performance-critical and protocol-unique handlers must be handled from
 * the switch block in ci_xxx_getsockopt() & ci_xxx_setsockopt().
 */

#ifndef __KERNEL__
static
#endif
int ci_ip_mtu_discover_from_sflags(int s_flags)
{
  switch( s_flags & (CI_SOCK_FLAG_PMTU_DO | CI_SOCK_FLAG_ALWAYS_DF) ) {
    case CI_SOCK_FLAG_PMTU_DO | CI_SOCK_FLAG_ALWAYS_DF:
      return IP_PMTUDISC_DO;
    case CI_SOCK_FLAG_PMTU_DO:
      return IP_PMTUDISC_WANT;
    case CI_SOCK_FLAG_ALWAYS_DF:
#ifdef IP_PMTUDISC_PROBE
      return IP_PMTUDISC_PROBE;
#else
      ci_assert(0);
      return IP_PMTUDISC_DO;
#endif
    case 0:
      return IP_PMTUDISC_DONT;
  }
  /* Unreachanble */
  return IP_PMTUDISC_DO;
}

#ifndef __KERNEL__
/* Handler for common getsockopt:SOL_IP options. */
int ci_get_sol_ip( ci_netif* ni, ci_sock_cmn* s, ci_fd_t fd,
                   int optname, void *optval, socklen_t *optlen )
{
  unsigned u;

  /* NOTE: "break" from this switch block will exit through code
   * that passes the value in [u] back to the caller.  */

  switch(optname) {
  case IP_OPTIONS:
    /* gets the IP options to be sent with every packet from this socket */
    LOG_U(ci_log("%s: "NS_FMT" unhandled IP_OPTIONS", __FUNCTION__,
                 NS_PRI_ARGS(ni, s)));
    goto fail_unsup;

  case IP_TOS:
    /* gets the IP ToS options sent with every packet from this socket */
    ci_assert((s->b.state & CI_TCP_STATE_TCP) ||
	      s->b.state == CI_TCP_STATE_UDP);

    u = s->cp.ip_tos;
    break;

  case IP_TTL:
    /* gets the IP TTL set in every packet sent on this socket */
    ci_assert((s->b.state & CI_TCP_STATE_TCP) ||
	      s->b.state == CI_TCP_STATE_UDP);

    u = s->cp.ip_ttl;
    break;

  case IP_MTU:
    if( s->b.state == CI_TCP_STATE_UDP ) {
#ifndef __KERNEL__
      if( sock_raddr_be32(s) != 0 ) {
        ci_ip_cache_invalidate(&s->pkt);
        cicp_user_retrieve(ni, &s->pkt, &s->cp);
        if( oo_cp_ipcache_is_valid(ni, &s->pkt) ) {
          u = s->pkt.mtu;
          break;
        }
      }
      return ci_get_os_sockopt(fd, IPPROTO_IP, optname, optval, optlen);
#else
      /* We should only be calling this function in the kernel for TCP
       * sockets.
       */
      ci_assert(0);
#endif
    }
    /* gets the current known path MTU of the current socket */
    /*! \todo Can we improve on the flagging here (other than
     * purging udp_state with extreme prejudice :-) ) */
    else if( s->b.state <= CI_TCP_LISTEN || 
             s->b.state >= CI_TCP_TIME_WAIT )  {
      /* The socket is not connected */
      RET_WITH_ERRNO(ENOTCONN);
    }
    u = ci_tcp_get_pmtu(ni, SOCK_TO_TCP(s));
    break;

  case IP_MTU_DISCOVER:
    /* gets the status of Path MTU discovery on this socket */
    u = ci_ip_mtu_discover_from_sflags(s->s_flags);
    break;

  case IP_RECVTOS:
    u = !!(s->cmsg_flags & CI_IP_CMSG_TOS);
    break;

  case IP_PKTOPTIONS:
    {
      struct msghdr msg;
      struct cmsg_state cmsg_state;

      /* On Linux, IP_PKTOPTIONS is stream-only */
      if( s->b.state == CI_TCP_STATE_UDP )
        RET_WITH_ERRNO(ENOPROTOOPT);
      /* ci_put_cmsg checks that optval is long enough */

      /* set all cmsg_len fields to 0 */
      memset(optval, 0, *optlen);

      msg.msg_control = optval;
      msg.msg_controllen = *optlen;
      msg.msg_flags = 0;
      cmsg_state.msg = &msg;
      cmsg_state.cm = CMSG_FIRSTHDR(&msg);
      cmsg_state.cmsg_bytes_used = 0;
      cmsg_state.p_msg_flags = &msg.msg_flags;

      if (s->cmsg_flags & CI_IP_CMSG_PKTINFO) {
        struct in_pktinfo info;
        info.ipi_addr.s_addr = info.ipi_spec_dst.s_addr = sock_laddr_be32(s);

        info.ipi_ifindex = s->cp.ip_multicast_if < 0 ?
            0 : s->cp.ip_multicast_if;
        ci_put_cmsg(&cmsg_state, IPPROTO_IP, IP_PKTINFO, sizeof(info), &info);
        if(msg.msg_flags & MSG_CTRUNC)
          goto fail_inval;
      }

      if (s->cmsg_flags & CI_IP_CMSG_TTL) {
        int ttl = s->cp.ip_mcast_ttl;
        ci_put_cmsg(&cmsg_state, IPPROTO_IP, IP_TTL, sizeof(ttl), &ttl);
        if(msg.msg_flags & MSG_CTRUNC)
          goto fail_inval;
      }

      *optlen = cmsg_state.cmsg_bytes_used;
      return 0;
    }

  case IP_RECVERR:
    u = !!(s->so.so_debug & CI_SOCKOPT_FLAG_IP_RECVERR);
    break;

#ifdef IP_TRANSPARENT
  case IP_TRANSPARENT:
    u = !!(s->s_flags & CI_SOCK_FLAG_TPROXY);
    break;
#endif


  case IP_RECVTTL:
    u = !!(s->cmsg_flags & CI_IP_CMSG_TTL);
    break;

  case IP_RECVOPTS:
    u = !!(s->cmsg_flags & CI_IP_CMSG_RECVOPTS);
    break;

  case IP_RETOPTS:
    u = !!(s->cmsg_flags & CI_IP_CMSG_RETOPTS);
    break;

#ifndef __KERNEL__
  /* UDP is handled in UDP-specific functions. */
  REPORT_CASE(IP_MULTICAST_IF)
  REPORT_CASE(IP_MULTICAST_LOOP)
  REPORT_CASE(IP_MULTICAST_TTL)
    /* Doing a getsockopt of these options on a TCP socket seems sufficiently
     * unlikely that I'm just returning an error rather than handling this
     * properly if there's no os socket to give us the answer.
     */
    if( s->b.sb_aflags & CI_SB_AFLAG_OS_BACKED )
      return ci_get_os_sockopt(fd, IPPROTO_IP, optname, optval, optlen);
    else
      RET_WITH_ERRNO(ENOPROTOOPT);
#endif

  case IP_PKTINFO:
    u = !!(s->cmsg_flags & CI_IP_CMSG_PKTINFO);
    break;



  default:
    goto fail_noopt;
  }

  return ci_getsockopt_final(optval, optlen, SOL_IP, &u, sizeof(u));

 fail_inval:
  LOG_SC( log("%s: "NS_FMT" invalid option: %i (EINVAL)",
             __FUNCTION__, NS_PRI_ARGS(ni, s), optname));
  RET_WITH_ERRNO(EINVAL);

 fail_noopt:
  LOG_SC( log("%s: "NS_FMT" unimplemented/bad option: %i (ENOPROTOOPT)",
             __FUNCTION__, NS_PRI_ARGS(ni, s), optname));

 fail_unsup:
  RET_WITH_ERRNO(ENOPROTOOPT);
}

#if CI_CFG_FAKE_IPV6
/* Handler for common getsockopt:SOL_IPV6 options. */
int ci_get_sol_ip6(ci_netif* ni, ci_sock_cmn* s, ci_fd_t fd, int optname, void *optval,
                   socklen_t *optlen )
{
#if CI_CFG_IPV6
  unsigned u;

  /* NOTE: "break" from this switch block will exit through code
   * that passes the value in [u] back to the caller.  */

  switch(optname) {
  case IPV6_V6ONLY:
    u = !!(s->s_flags & CI_SOCK_FLAG_V6ONLY);
    break;

  case IPV6_RECVPKTINFO:
    u = !!(s->cmsg_flags & CI_IPV6_CMSG_PKTINFO);
    break;

  case IPV6_TCLASS:
    u = s->tclass;
    break;

  case IPV6_RECVERR:
    u = !!(s->so.so_debug & CI_SOCKOPT_FLAG_IPV6_RECVERR);
    break;

  case IPV6_AUTOFLOWLABEL:
    u = !!(s->s_flags & CI_SOCK_FLAG_AUTOFLOWLABEL_OPT);
    break;

  default:
    LOG_U(log("%s: "NS_FMT" unimplemented/bad SOL_IPV6 option: %i",
              __FUNCTION__, NS_PRI_ARGS(ni, s), optname));
    if( s->b.sb_aflags & CI_SB_AFLAG_OS_BACKED )
      return ci_get_os_sockopt(fd, IPPROTO_IPV6, optname, optval, optlen);
    else
      RET_WITH_ERRNO(ENOPROTOOPT);
  }

  return ci_getsockopt_final(optval, optlen, SOL_IPV6, &u, sizeof(u));
#endif

  return ci_get_os_sockopt(fd, IPPROTO_IPV6, optname, optval, optlen);
}
#endif
#endif

/* Handler for common getsockopt:SOL_SOCKET options. */
int ci_get_sol_socket( ci_netif* netif, ci_sock_cmn* s,
                       int optname, void *optval, socklen_t *optlen )
{
  int u;

  switch(optname) {
#if CI_CFG_TCP_SOCK_STATS
  case CI_SO_L5_GET_SOCK_STATS:
    /* Way to get access to our socket statistics data
     * optval is a pointer to memory & optval should be at least
     * 2 * sizeof(ci_ip_sock_stats)
     */
    if(*optlen < (sizeof(ci_ip_sock_stats)<<1) )
      goto fail_inval;
    ci_tcp_stats_action(netif, (ci_tcp_state*) s, CI_IP_STATS_REPORT,
                        CI_IP_STATS_OUTPUT_NONE, optval, optlen );
    break;

  case CI_SO_L5_DUMP_SOCK_STATS:
# if CI_CFG_SEND_STATS_TO_LOG==0
    /* TODO check that optval is long enough? */
    if(*optlen == 0)
      goto fail_inval;
# endif
    if( ! (s->b.state & CI_TCP_STATE_TCP_CONN) )
      goto fail_inval;
    ci_tcp_stats_action(netif, SOCK_TO_TCP(s), CI_IP_STATS_REPORT,
                        CI_IP_STATS_OUTPUT_NONE, optval, optlen );
    break;
#endif

#if CI_CFG_SUPPORT_STATS_COLLECTION
  case CI_SO_L5_GET_NETIF_STATS:
    /* Way to get access to our netif statistics data
     * optval is a pointer to memory & optval should be at least
     * 2 * sizeof(ci_ip_stats)
     */
    if(*optlen < (sizeof(ci_ip_stats)<<1) )
      goto fail_inval;

    ci_netif_stats_action(netif, CI_IP_STATS_REPORT,
                          CI_IP_STATS_OUTPUT_NONE, optval, optlen );
    break;

  case CI_SO_L5_DUMP_NETIF_STATS:
# if CI_CFG_SEND_STATS_TO_LOG==0
    /* TODO check that optval is long enough? */
    if(*optlen == 0)
      goto fail_inval;
# endif
    /* Get the report in text or xml format */
    ci_netif_stats_action(netif, CI_IP_STATS_REPORT,
                          CI_IP_STATS_OUTPUT_NONE, optval, optlen );
    break;
#endif

  case SO_KEEPALIVE:
    u = !!(s->s_flags & CI_SOCK_FLAG_KALIVE);
    goto u_out;

  case SO_OOBINLINE:
    /* if enabled out-of-band data is directly placed in receive stream */

    u = !!(s->s_flags & CI_SOCK_FLAG_OOBINLINE);
    goto u_out;

  case SO_RCVLOWAT:
    u = s->so.rcvlowat;
    goto u_out;

  case SO_SNDLOWAT:
    /* unchangable on always set to 1 byte */
    u = 1u;
    goto u_out;

  case SO_RCVTIMEO: {
    /* BUG2725: Windows isn't BSD compatible at all! */
    struct timeval tv;
    tv.tv_sec = s->so.rcvtimeo_msec / 1000;
    tv.tv_usec = (s->so.rcvtimeo_msec - (tv.tv_sec * 1000ULL)) * 1000ULL;
    return ci_getsockopt_final(optval, optlen, SOL_SOCKET, &tv, sizeof(tv));
  }

  case SO_SNDTIMEO: {
    /* BUG2725: Windows isn't BSD compatible at all! */
    struct timeval tv;
    tv.tv_sec = s->so.sndtimeo_msec / 1000;
    tv.tv_usec = (s->so.sndtimeo_msec - (tv.tv_sec * 1000ULL)) * 1000ULL;
    return ci_getsockopt_final(optval, optlen, SOL_SOCKET, &tv, sizeof(tv));
  }

  case SO_REUSEADDR:
    /* Allow bind to reuse local addresses */
    u = !!(s->s_flags & CI_SOCK_FLAG_REUSEADDR);
    goto u_out;

  case SO_TYPE:
    /* get socket type */
    ci_assert((s->b.state & CI_TCP_STATE_TCP) ||
	      s->b.state == CI_TCP_STATE_UDP);
    u = (s->b.state & CI_TCP_STATE_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    goto u_out;

#ifdef SO_PROTOCOL
  case SO_PROTOCOL:
    /* get protocol type - mapped from socket type */
    ci_assert((s->b.state & CI_TCP_STATE_TCP) ||
	      s->b.state == CI_TCP_STATE_UDP);
    u = (s->b.state & CI_TCP_STATE_TCP) ? IPPROTO_TCP : IPPROTO_UDP;
    goto u_out;
#endif


  case SO_DONTROUTE:
    /* don't send via gateway, only directly connected machine */
    /*! ?? \TODO */
    goto fail_noopt;

  case SO_BROADCAST:
    /* get current broadcast rx state */
    /* Note: while this is unused by TCP it's always accessible */
    u = !!(s->s_flags & CI_SOCK_FLAG_BROADCAST);
    goto u_out;

  case SO_SNDBUF:
    /* gets the maximum socket send buffer in bytes */
    u = s->so.sndbuf;
    goto u_out;

  case SO_RCVBUF:
    /* gets the maximum socket receive buffer in bytes */
    u = s->so.rcvbuf;
    goto u_out;

  case SO_LINGER:
    {
      struct linger l;
      memset(&l, 0, sizeof(l));


      if( s->s_flags & CI_SOCK_FLAG_LINGER ) {
        l.l_onoff = 1;
        l.l_linger = s->so.linger;
      } else {
        l.l_onoff = 0;
      }
      VERB(ci_log("%s: onoff:%d fl:%x", __FUNCTION__,
                  l.l_onoff, s->s_flags));
      return ci_getsockopt_final(optval, optlen, SOL_SOCKET, &l, sizeof(l));
    }

  case SO_PRIORITY:
    u = (unsigned) s->so_priority;
    goto u_out;

  case SO_BINDTODEVICE:
    u = 0;
    if( s->cp.so_bindtodevice == CI_IFID_BAD ) {
      *optlen = 0;
      return 0;
    }

    {
      struct cp_mibs* mib;
      cicp_rowid_t id;
      cp_version_t version;
      char ifname[IFNAMSIZ+1];

      CP_VERLOCK_START(version, mib, netif->cplane)
        id = cp_llap_find_row(mib, s->cp.so_bindtodevice);
        if( id != CICP_ROWID_BAD )
          strcpy(ifname, mib->llap[id].name);
      CP_VERLOCK_STOP(version, mib)

      if( id == CICP_ROWID_BAD ) {
        *optlen = 0;
        return 0;
      }

      return ci_getsockopt_final(optval, optlen, SOL_SOCKET,
                                 ifname, strlen(ifname) + 1);
    }
    /*unreachable*/

  case SO_ERROR:
    /* Gets the pending socket error and reset the pending error */
    u = ci_get_so_error(s);
    goto u_out;

  case SO_ACCEPTCONN:
    u = (s->b.state == CI_TCP_LISTEN);
    goto u_out;

  case SO_DEBUG:
    u = !!(s->so.so_debug & CI_SOCKOPT_FLAG_SO_DEBUG);
    goto u_out;


  case SO_TIMESTAMP:
    u = !!(s->cmsg_flags & CI_IP_CMSG_TIMESTAMP);
    goto u_out;

  case SO_TIMESTAMPNS:
    u = !!(s->cmsg_flags & CI_IP_CMSG_TIMESTAMPNS);
    goto u_out;

#if CI_CFG_TIMESTAMPING
  case ONLOAD_SO_TIMESTAMPING:
    u = s->timestamping_flags;
    /* Only report the flags if they were set with `setsockopt`. If the
     * behaviour was overridden with `onload_timestamping_request` then the
     * flags have different meanings which might cause confusion.
     */
    if( u & ONLOAD_SOF_TIMESTAMPING_ONLOAD )
      u = 0;
    goto u_out;
#endif

  case SO_REUSEPORT:
    u = !!(s->s_flags & CI_SOCK_FLAG_REUSEPORT);
    goto u_out;

  case ONLOAD_SO_BUSY_POLL:
  {
    unsigned val = oo_cycles64_to_usec(netif, s->b.spin_cycles);

    if( val > INT_MAX )
      u = INT_MAX;
    else
      u = val;
    goto u_out;
  }

#ifdef SO_SELECT_ERR_QUEUE
  case SO_SELECT_ERR_QUEUE:
    if( s->s_aflags & CI_SOCK_AFLAG_SELECT_ERR_QUEUE )
      u = 1;
    else
      u = 0;
    goto u_out;
#endif

  default: /* Unexpected & known invalid options end up here */
    goto fail_noopt;
  }

  return 0;

 u_out:
  if( (int)*optlen >= 0 ) {
    int minlen = CI_MIN(sizeof(u), (int)*optlen);
    memcpy(optval, (char*)&u, minlen);
    *optlen = minlen;
    return 0;
  } 
  /* deliberate drop through */ 

 fail_inval:
  LOG_SC(log("%s: "NS_FMT" option %i ptr/len error (EINVAL or EFAULT)",
             __FUNCTION__, NS_PRI_ARGS(netif, s), optname));
  RET_WITH_ERRNO(EINVAL);

 fail_noopt:
  LOG_SC(log("%s: "NS_FMT" unimplemented/bad option %i (ENOPROTOOPT)",
            __FUNCTION__, NS_PRI_ARGS(netif, s), optname));
  RET_WITH_ERRNO(ENOPROTOOPT);
}

#ifndef __KERNEL__
static int ci_set_recverr(ci_netif* ni, ci_sock_cmn* s, ci_int32 flag,
                          const void *optval, socklen_t optlen)
{
  int rc;
  ci_assert(flag == CI_SOCKOPT_FLAG_IP_RECVERR ||
            flag == CI_SOCKOPT_FLAG_IPV6_RECVERR);
  if( (rc = opt_not_ok(optval, optlen, char)) )
    return rc;
  if( ci_get_optval(optval, optlen) )
    s->so.so_debug |= flag;
  else {
    s->so.so_debug &= ~flag;
    if( s->os_sock_status & OO_OS_STATUS_ERR ) {
      oo_sp sock_id = SC_SP(s);
      oo_resource_op(ci_netif_get_driver_handle(ni),
                     OO_IOC_OS_POLLERR_CLEAR, &sock_id);
    }
  }
  return 0;
}

/* Handler for common setsockopt:SOL_IP handlers */
int ci_set_sol_ip( ci_netif* netif, ci_sock_cmn* s,
                   int optname, const void *optval, socklen_t optlen)
{
  int rc = 0; /* Shut up compiler warning */

  ci_assert(netif);

  /* IP level options valid for TCP */
  switch(optname) {
  case IP_OPTIONS:
    /* sets the IP options to be sent with every packet from this socket */
    /*! ?? \TODO is this possible ? */
    LOG_U(ci_log("%s: "NS_FMT" unhandled IP_OPTIONS", __FUNCTION__,
                 NS_PRI_ARGS(netif, s)));
    goto fail_unhan;

  case IP_TOS:
  {
    unsigned val;

    /* sets the IP ToS options sent with every packet from this socket   */
    /* Note: currently we do not interpret this value in determining our */
    /*       delivery strategy                                           */
    if( (rc = opt_not_ok( optval, optlen, char)) ) {
      if( optlen == 0 )
        return 0;
      goto fail_fault;
    }
    val = ci_get_optval(optval, optlen);


    if( s->b.state & CI_TCP_STATE_TCP ) {
      /* Bug3172: do not allow to change 2 and 1 bits of TOS for TCP socket. */
      val &= ~3;
      val |= s->cp.ip_tos & 3;
    }
    val = CI_MIN(val, CI_IP_MAX_TOS);
    s->cp.ip_tos = (ci_uint8)val;
    if( ! ipcache_is_ipv6(&s->pkt) ) {
      s->pkt.ipx.ip4.ip_tos = (ci_uint8)val;
      if( s->b.state == CI_TCP_STATE_UDP )
        SOCK_TO_UDP(s)->ephemeral_pkt.ipx.ip4.ip_tos = (ci_uint8)val;
    }

    LOG_TV(log("%s: "NS_FMT" TCP IP_TOS = %u", __FUNCTION__,
               NS_PRI_ARGS(netif, s), s->pkt.ipx.ip4.ip_tos));

    /* Set SO_PRIORITY */
    s->so_priority = ci_tos2priority[(((val)>>1) & 0xf)];
    break;
  }

  case IP_TTL: {
    int v;
    /* Set the TTL on this socket */
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_fault;
    v = ci_get_optval(optval, optlen);

    if( 
      v == 0 || 
      v > CI_IP_MAX_TTL ) {
      rc = -EINVAL;
      goto fail_fault;
    }

    s->cp.ip_ttl = (ci_uint8) v;
    s->s_flags |= CI_SOCK_FLAG_SET_IP_TTL;
    if( ! CI_IP_IS_MULTICAST(s->pkt.ipx.ip4.ip_daddr_be32) )
      s->pkt.ipx.ip4.ip_ttl = s->cp.ip_ttl;
    if( s->b.state == CI_TCP_STATE_UDP) {
      ci_udp_state* us = SOCK_TO_UDP(s);
      if (! CI_IP_IS_MULTICAST(us->ephemeral_pkt.ipx.ip4.ip_daddr_be32) )
        us->ephemeral_pkt.ipx.ip4.ip_ttl = s->cp.ip_ttl;
    }
    LOG_TV(log("%s: "NS_FMT" IP_TTL = %u", __FUNCTION__,
               NS_PRI_ARGS(netif, s), s->cp.ip_ttl));
    break;
  }

    /* Bug 5644 */
  case IP_PKTINFO:
    if( (rc = opt_not_ok(optval, optlen, char)) ) {
      if( optlen == 0 )
        return 0;
      goto fail_fault;
    }

    if (ci_get_optval(optval, optlen))
      s->cmsg_flags |= CI_IP_CMSG_PKTINFO;
    else
      s->cmsg_flags &= ~CI_IP_CMSG_PKTINFO;
    break;

  case IP_MTU_DISCOVER:
  {
    unsigned val;
    /* sets the Path MTU discovery on this socket */
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_fault;
    val = ci_get_optval(optval, optlen);

    if( val < IP_PMTUDISC_DONT ||
#ifdef IP_PMTUDISC_PROBE
        val > IP_PMTUDISC_PROBE
#else
        val > IP_PMTUDISC_DO
#endif
      ) {
      rc = -EINVAL;
      goto fail_fault;
    }

    switch( val ) {
      case IP_PMTUDISC_DONT:
        s->s_flags &= ~(CI_SOCK_FLAG_ALWAYS_DF | CI_SOCK_FLAG_PMTU_DO);
        break;
      case IP_PMTUDISC_DO:
        s->s_flags |= CI_SOCK_FLAG_PMTU_DO | CI_SOCK_FLAG_ALWAYS_DF;
        break;
      case IP_PMTUDISC_WANT:
        s->s_flags |= CI_SOCK_FLAG_PMTU_DO;
        s->s_flags &= ~CI_SOCK_FLAG_ALWAYS_DF;
        break;
#ifdef IP_PMTUDISC_PROBE
      case IP_PMTUDISC_PROBE:
        s->s_flags |= CI_SOCK_FLAG_ALWAYS_DF;
        s->s_flags &= ~CI_SOCK_FLAG_PMTU_DO;
        break;
#endif
    }
    break;
  }

   case IP_RECVTOS:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_fault;

    if (ci_get_optval(optval, optlen))
      s->cmsg_flags |= CI_IP_CMSG_TOS;
    else
      s->cmsg_flags &= ~CI_IP_CMSG_TOS;
    break;

  case IP_RECVERR:
    if( (rc = ci_set_recverr(netif, s, CI_SOCKOPT_FLAG_IP_RECVERR, optval,
                             optlen)) )
      goto fail_fault;
    break;

#ifdef IP_TRANSPARENT
  case IP_TRANSPARENT:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_fault;
    if (ci_get_optval(optval, optlen)) {
      if( (NI_OPTS(netif).scalable_filter_enable !=
           CITP_SCALABLE_FILTERS_ENABLE) ||
          (NI_OPTS(netif).scalable_filter_mode &
           CITP_SCALABLE_MODE_TPROXY_ACTIVE) == 0 ||
          ! (s->b.state & CI_TCP_STATE_TCP) )
        goto handover;
      if( s->b.sb_aflags & CI_SB_AFLAG_OS_BACKED ) {
        /* The transparent proxy socket option must be set before we acquire
         * an os socket.
         */
        rc = -EINVAL;
        goto fail_fault;
      }
      s->s_flags |= CI_SOCK_FLAG_TPROXY;
      s->cp.sock_cp_flags |= OO_SCP_TPROXY;
    }
    else {
      /* If we've bound the socket we've inserted only sw filters and we
       * have no backing socket.  Fixing up the state would be hard, and we
       * don't see an obvious reason for wanting to do this, so for now
       * we're not supporting it.
       *
       * Sockets with IP_TRANSPARENT must be explicitly bound so we can
       * simply check the bound flag to detect this case.
       */
      if( (s->s_flags & CI_SOCK_FLAG_TPROXY) &&
          (s->s_flags & CI_SOCK_FLAG_BOUND) ) {
        rc = -EINVAL;
        goto fail_fault;
      }
      else {
        s->s_flags &= ~CI_SOCK_FLAG_TPROXY;
        s->cp.sock_cp_flags &= ~OO_SCP_TPROXY;
      }
    }
    break;
#endif



  case IP_RECVTTL:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_fault;

    if (ci_get_optval(optval, optlen))
      s->cmsg_flags |= CI_IP_CMSG_TTL;
    else
      s->cmsg_flags &= ~CI_IP_CMSG_TTL;
    break;

   case IP_RECVOPTS:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_fault;

    if (ci_get_optval(optval, optlen))
      s->cmsg_flags |= CI_IP_CMSG_RECVOPTS;
    else
      s->cmsg_flags &= ~CI_IP_CMSG_RECVOPTS;
    break;

  case IP_RETOPTS:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_fault;

    if (ci_get_optval(optval, optlen))
      s->cmsg_flags |= CI_IP_CMSG_RETOPTS;
    else
      s->cmsg_flags &= ~CI_IP_CMSG_RETOPTS;
    break;


  REPORT_CASE(IP_ADD_MEMBERSHIP)
  REPORT_CASE(IP_DROP_MEMBERSHIP)
#ifdef MCAST_JOIN_GROUP
  REPORT_CASE(MCAST_JOIN_GROUP)
  REPORT_CASE(MCAST_LEAVE_GROUP)
#endif
  REPORT_CASE(IP_MULTICAST_IF)
  REPORT_CASE(IP_MULTICAST_LOOP)
  REPORT_CASE(IP_MULTICAST_TTL)
    /* When real work is necessary, it is already done in UDP-specific
     * functions or by OS . */
    break;



  default:
    goto fail_noopt;
  }

  return 0;

 fail_fault:
  LOG_SC(log("%s: "NS_FMT" option %i ptr/len error (EFAULT or EINVAL)", __FUNCTION__,
             NS_PRI_ARGS(netif, s), optname));
  RET_WITH_ERRNO( -rc );

 fail_noopt:
  LOG_SC(log("%s: "NS_FMT" unimplemented/bad option %i (ENOPROTOOPT)",
             __FUNCTION__, NS_PRI_ARGS(netif, s), optname));
 fail_unhan:
  RET_WITH_ERRNO( ENOPROTOOPT );

#ifdef IP_TRANSPARENT
 handover:
  LOG_SC(log("%s: "NS_FMT" can't handle option %i; handing over",
             __FUNCTION__, NS_PRI_ARGS(netif, s), optname));
  /* This is not actually a failure, so don't set errno. */
  return CI_SOCKET_HANDOVER;
#endif
}

#if CI_CFG_IPV6
ci_inline void
ci_set_autoflowlabel_flags(ci_netif* ni, ci_sock_cmn* s, const void *optval,
                           socklen_t optlen)
{
  if( ci_get_optval(optval, optlen) )
    s->s_flags |= CI_SOCK_FLAG_AUTOFLOWLABEL_OPT;
  else
    s->s_flags &= ~CI_SOCK_FLAG_AUTOFLOWLABEL_OPT;

  switch(NI_OPTS(ni).auto_flowlabels) {
    case CITP_IP6_AUTO_FLOW_LABEL_FORCED:
      s->s_flags |= CI_SOCK_FLAG_AUTOFLOWLABEL_REQ;
      break;
    case CITP_IP6_AUTO_FLOW_LABEL_OPTOUT:
    case CITP_IP6_AUTO_FLOW_LABEL_OPTIN:
      if( s->s_flags & CI_SOCK_FLAG_AUTOFLOWLABEL_OPT )
        s->s_flags |= CI_SOCK_FLAG_AUTOFLOWLABEL_REQ;
      else
        s->s_flags &= ~CI_SOCK_FLAG_AUTOFLOWLABEL_REQ;
      break;
    case CITP_IP6_AUTO_FLOW_LABEL_OFF:
    default:
      s->s_flags &= ~CI_SOCK_FLAG_AUTOFLOWLABEL_REQ;
      break;
  }
}
#endif

#if CI_CFG_FAKE_IPV6
/* Handler for common getsockopt:SOL_IPV6 options. */
int ci_set_sol_ip6( ci_netif* netif, ci_sock_cmn* s,
                    int optname, const void *optval, socklen_t optlen )
{
  int rc = 0; /* Shut up compiler warning */

  switch( optname ) {
  case IPV6_V6ONLY:
  {
    int val = ci_get_optval(optval, optlen);
#if CI_CFG_IPV6
    if( val && ! CI_IS_ADDR_IP6(s->laddr) )
      RET_WITH_ERRNO(EINVAL);
    if( val )
      s->s_flags |= CI_SOCK_FLAG_V6ONLY;
    else
      s->s_flags &=~ CI_SOCK_FLAG_V6ONLY;
#else
    if( val )
      return CI_SOCKET_HANDOVER;
#endif
    break;
  }

#if CI_CFG_IPV6
  case IPV6_RECVPKTINFO:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_inval;

    if ( ci_get_optval(optval, optlen) )
      s->cmsg_flags |= CI_IPV6_CMSG_PKTINFO;
    else
      s->cmsg_flags &= ~CI_IPV6_CMSG_PKTINFO;
    break;

  case IPV6_TCLASS:
  {
    int val;

    if( (rc = opt_not_ok( optval, optlen, char)) )
      goto fail_inval;

    val = ci_get_optval(optval, optlen);
    /* Value checks according to RFC 3542 par. 6.5 and are similar to Linux */
    if( val < -1 || val > 0xff )
      goto fail_inval;
    if( val == -1 )
      val = CI_IPV6_DFLT_TCLASS;

    s->tclass = (ci_uint8)val;
    if( ipcache_is_ipv6(&s->pkt) ) {
      ci_ip6_set_tclass(&s->pkt.ipx.ip6, (ci_uint8)val);
      if( s->b.state == CI_TCP_STATE_UDP )
        ci_ip6_set_tclass(&SOCK_TO_UDP(s)->ephemeral_pkt.ipx.ip6,
                          (ci_uint8)val);
    }
    break;
  }

  case IPV6_RECVERR:
    if( (rc = ci_set_recverr(netif, s, CI_SOCKOPT_FLAG_IPV6_RECVERR, optval,
                             optlen)) )
      goto fail_inval;
    break;

  case IPV6_AUTOFLOWLABEL:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_inval;
    ci_set_autoflowlabel_flags(netif, s, optval, optlen);
    break;

  default:
    /* Don't fail with error but print the message about unimplemented IPv6
     * options. Options are set for system socket.*/
    LOG_U(log("%s: "NS_FMT" unimplemented/bad SOL_IPV6 option %i",
              __FUNCTION__, NS_PRI_ARGS(netif, s), optname));
#endif
  }
  /* All socket options are already set for system socket, and we do not
   * handle IPv6 option natively. */
  return rc;
#if CI_CFG_IPV6
 fail_inval:
  LOG_SC(log("%s: "NS_FMT" option %i ptr/len error (EINVAL or EFAULT)",
             __FUNCTION__, NS_PRI_ARGS(netif, s), optname));
  RET_WITH_ERRNO( -rc );
#endif
}
#endif

/* Handler for common setsockopt:SOL_SOCKET handlers */
int ci_set_sol_socket(ci_netif* netif, ci_sock_cmn* s,
                      int optname, const void* optval, socklen_t optlen)
{
  int v;
  int rc;

  ci_assert(netif);

  switch(optname) {
#if CI_CFG_TCP_SOCK_STATS
    /* Our proprietary socket options for collecting stats */
  case CI_SO_L5_CONFIG_SOCK_STATS:
    {
      ci_tcp_state* ts = (ci_tcp_state*) s;
      ci_ip_stats_config *tcp_config;
      if( (rc = opt_not_ok(optval, optlen, ci_ip_stats_config)) )
        goto fail_inval;

      tcp_config = (ci_ip_stats_config *) optval;

      NI_CONF(netif).tconst_stats =
        ci_tcp_time_ms2ticks(netif, tcp_config->timeout);

      ts->stats_fmt = tcp_config->output_fmt;
      /* (Re)start the collection - will dump right now */
      ci_tcp_stats_action( netif, ts,
                           tcp_config->action_type,
                           tcp_config->output_fmt,
                           NULL, NULL);
      break;
    }
#endif

#if CI_CFG_SUPPORT_STATS_COLLECTION
  case CI_SO_L5_CONFIG_NETIF_STATS:
    {
      ci_ip_stats_config *netif_config;

      if( (rc =opt_not_ok(optval, optlen, ci_ip_stats_config)) )
        goto fail_inval;

      netif_config = (ci_ip_stats_config *) optval;

      NI_CONF(netif).tconst_stats =
        ci_tcp_time_ms2ticks(netif, netif_config->timeout);

      netif->state->stats_fmt = netif_config->output_fmt;
      /* (Re)start the collection - will dump right now */
      ci_netif_stats_action( netif,
                             netif_config->action_type,
                             netif_config->output_fmt,
                             NULL, NULL);
      break;
    }
#endif

  case SO_KEEPALIVE:
    /* Default Keepalive handler - use ONLY for protocols that do not
     * do keepalives */
    if( (rc = opt_not_ok(optval, optlen, unsigned)) )
      goto fail_inval;

    if(*(unsigned*)optval)
      s->s_flags |= CI_SOCK_FLAG_KALIVE;
    else
      s->s_flags &= ~CI_SOCK_FLAG_KALIVE;
    break;

  case SO_OOBINLINE:
    /* If enabled, out-of-band data is directly placed in receive stream.
     * While this has no effect in UDP, setsockopt() still stores the flag. */
    if( (rc = opt_not_ok(optval, optlen, unsigned)) )
      goto fail_inval;


    s->s_flags = ( *((unsigned*)optval) )
      ? s->s_flags | CI_SOCK_FLAG_OOBINLINE
      : s->s_flags & (~CI_SOCK_FLAG_OOBINLINE);
    break;

  case SO_RCVLOWAT: {
    int val;
    if( (rc = opt_not_ok(optval, optlen, int)) )
      goto fail_inval;
    val = *(int*) optval;

    if( val < 0)
      val = INT_MAX;
    /* In Linux (2.4, 2.6) 0 means 1. */
    s->so.rcvlowat = val ? val : 1;
    break;
  }

  case SO_DONTROUTE:
    /* don't send via gateway, only directly connected machine */
    /*! ?? \TODO */
      LOG_U(ci_log("%s: "NS_FMT" SO_DONTROUTE seen (not supported)",
                   __FUNCTION__, NS_PRI_ARGS(netif, s)));
      goto fail_noopt;

  case SO_BROADCAST:
    /* Allow broadcasts (no effect on TCP) */
    if( (rc = opt_not_ok(optval, optlen, unsigned)) )
      goto fail_inval;

    if(*(unsigned*)optval)
      s->s_flags |= CI_SOCK_FLAG_BROADCAST;
    else
      s->s_flags &= ~CI_SOCK_FLAG_BROADCAST;
    break;

  case SO_REUSEADDR:
    if( (rc = opt_not_ok(optval, optlen, unsigned)) )
      goto fail_inval;
    if( *(unsigned*) optval )
      s->s_flags |= CI_SOCK_FLAG_REUSEADDR;
    else
      s->s_flags &= ~CI_SOCK_FLAG_REUSEADDR;
    break;

  case SO_SNDBUF:
  case SO_SNDBUFFORCE:
    /* Sets the maximum socket send buffer in bytes. */
    if( (rc = opt_not_ok(optval, optlen, int)) )
      goto fail_inval;
    v = *(int*) optval;

    /* UDP case is handled in ci_udp_setsockopt_lk() */
    ci_assert_flags(s->b.state, CI_TCP_STATE_TCP);

    if( optname == SO_SNDBUF ) {
      v = CI_MIN(v, (int) NI_OPTS(netif).tcp_sndbuf_max);
    }
    else {
      int lim = CI_MAX((int)NI_OPTS(netif).tcp_sndbuf_max,
                       netif->packets->sets_max * CI_CFG_PKT_BUF_SIZE / 2);
      if( v > lim ) {
        NI_LOG_ONCE(netif, RESOURCE_WARNINGS,
                    "SO_SNDBUFFORCE: limiting user-provided value %d to %d.  "
                    "Consider increasing of EF_MAX_PACKETS.", v, lim);
        v = lim;
      }
    }
    s->so.sndbuf = CI_MAX(oo_adjust_SO_XBUF(v), (int) NI_OPTS(netif).tcp_sndbuf_min);
    /* only recalculate sndbuf, if the socket is already connected, if not,
     * then eff_mss is probably rubbish and we also know that the sndbuf
     * will have to be set when the socket is promoted to established
     */
    if( ! (s->b.state & CI_TCP_STATE_NOT_CONNECTED) )
      ci_tcp_set_sndbuf(netif, SOCK_TO_TCP(s));
    s->s_flags |= CI_SOCK_FLAG_SET_SNDBUF;
    break;

  case SO_RCVBUF:
  case SO_RCVBUFFORCE:
    /* Sets the maximum socket receive buffer in bytes. */
    if( (rc = opt_not_ok(optval, optlen, int)) )
      goto fail_inval;
    v = *(int*) optval;

    /* UDP case is handled in ci_udp_setsockopt_lk() */
    ci_assert_flags(s->b.state, CI_TCP_STATE_TCP);

    if( optname == SO_RCVBUF ) {
      v = CI_MIN(v, (int) NI_OPTS(netif).tcp_rcvbuf_max);
    }
    else {
      int lim = CI_MAX((int)NI_OPTS(netif).tcp_rcvbuf_max,
                       netif->packets->sets_max * CI_CFG_PKT_BUF_SIZE / 2);
      if( v > lim ) {
        NI_LOG_ONCE(netif, RESOURCE_WARNINGS,
                    "SO_RCVBUFFORCE: limiting user-provided value %d to %d.  "
                    "Consider increasing of EF_MAX_PACKETS.", v, lim);
        v = lim;
      }
    }
    s->so.rcvbuf = CI_MAX(oo_adjust_SO_XBUF(v), (int) NI_OPTS(netif).tcp_rcvbuf_min);
    if( ~s->b.state & CI_TCP_STATE_NOT_CONNECTED )
      ci_tcp_set_rcvbuf(netif, SOCK_TO_TCP(s));
    s->s_flags |= CI_SOCK_FLAG_SET_RCVBUF;
    break;

  case SO_LINGER:
    {
      struct linger *l = (struct linger*)optval;

      /* sets linger status */
      if( (rc = opt_not_ok(optval, optlen, struct linger)) )
        goto fail_inval;

      if( l->l_onoff ) {
        s->s_flags |= CI_SOCK_FLAG_LINGER;
        s->so.linger = l->l_linger;
      } else {
        s->s_flags &= ~CI_SOCK_FLAG_LINGER;
      }
      VERB(ci_log("%s: onoff:%d fl:%x", __FUNCTION__,
                  l->l_onoff, s->s_flags));
      break;
    }

  case SO_PRIORITY:
      if( (rc = opt_not_ok(optval, optlen, ci_pkt_priority_t)) )
        goto fail_inval;

      /* Linux stores/returns the precise priority value set */
      s->so_priority = *(ci_pkt_priority_t *)optval;
      break;

  case SO_BINDTODEVICE:
      rc = ci_sock_bindtodevice(netif, s, optval, optlen);
      if( rc == 0 || rc == CI_SOCKET_HANDOVER )
        return rc;
      else
        goto fail_other;
      break;

  case SO_DEBUG:
    if( (rc = opt_not_ok(optval, optlen, unsigned)) )
      goto fail_inval;

    if (*(unsigned*)optval)
      s->so.so_debug |= CI_SOCKOPT_FLAG_SO_DEBUG;
    else
      s->so.so_debug &= ~CI_SOCKOPT_FLAG_SO_DEBUG;
    break;


  case SO_TIMESTAMP:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_inval;
    if( ci_get_optval(optval, optlen) )
      s->cmsg_flags |= CI_IP_CMSG_TIMESTAMP;
    else
      s->cmsg_flags &= ~CI_IP_CMSG_TIMESTAMP;
    break;

  case SO_TIMESTAMPNS:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_inval;
    if( ci_get_optval(optval, optlen) )
      s->cmsg_flags |= CI_IP_CMSG_TIMESTAMPNS;
    else
      s->cmsg_flags &= ~CI_IP_CMSG_TIMESTAMPNS;
    break;

  case SO_REUSEPORT:
    if( (rc = opt_not_ok(optval, optlen, unsigned)) )
      goto fail_inval;
    if( *(unsigned*) optval )
      s->s_flags |= CI_SOCK_FLAG_REUSEPORT;
    else
      s->s_flags &= ~CI_SOCK_FLAG_REUSEPORT;
    break;

  case ONLOAD_SO_BUSY_POLL:
  {
    int val;
    if( (rc = opt_not_ok(optval, optlen, unsigned)) )
      goto fail_inval;
    val = *(int*) optval;

    if( val < 0 )
      goto fail_inval;

    s->b.spin_cycles = oo_usec_to_cycles64(netif,
                                           val == INT_MAX ? -1 : val);
    break;
  }

#if CI_CFG_TIMESTAMPING
  case ONLOAD_SO_TIMESTAMPING:
    if( (rc = opt_not_ok(optval, optlen, unsigned)) )
      goto fail_inval;
    v = ci_get_optval(optval, optlen);
    rc = -EINVAL;
    if( v & ~(ONLOAD_SOF_TIMESTAMPING_MASK |
              ONLOAD_SOF_TIMESTAMPING_STREAM) )
      goto fail_inval;
    if( (v & ONLOAD_SOF_TIMESTAMPING_STREAM) &&
        ( ! (s->b.state & CI_TCP_STATE_TCP) ||
          ! (v & ONLOAD_SOF_TIMESTAMPING_TX_HARDWARE) ) )
      goto fail_inval;

    if( (v & ONLOAD_SOF_TIMESTAMPING_TX_HARDWARE) ) {
      int intf_i;
      int some_good = 0;

      OO_STACK_FOR_EACH_INTF_I(netif, intf_i) {
        if( netif->state->nic[intf_i].oo_vi_flags &
            OO_VI_FLAGS_TX_HW_TS_EN )
          some_good = 1;
        else
          LOG_U(log("WARNING: Request for SOF_TIMESTAMPING_TX_HARDWARE when "
                    "TX timestamps are off on the network interface "
                    "with ifindex=%d.  "
                    "Try setting EF_TX_TIMESTAMPING.",
                    ci_intf_i_to_ifindex(netif, intf_i)));
      }
      if( ! some_good )
        log("WARNING: Request for SOF_TIMESTAMPING_TX_HARDWARE when "
            "TX timestamps are off for ALL Onload network interfaces.  "
            "Try setting EF_TX_TIMESTAMPING.");
    }
    if( (v & ONLOAD_SOF_TIMESTAMPING_RX_HARDWARE) ) {
      int intf_i;
      int some_good = 0;

      OO_STACK_FOR_EACH_INTF_I(netif, intf_i) {
        if( netif->state->nic[intf_i].oo_vi_flags &
            OO_VI_FLAGS_RX_HW_TS_EN )
          some_good = 1;
        else
          LOG_U(log("WARNING: Request for SOF_TIMESTAMPING_RX_HARDWARE when "
                    "RX timestamps are off on the network interface "
                    "with ifindex=%d.  "
                    "Try setting EF_RX_TIMESTAMPING.",
                    ci_intf_i_to_ifindex(netif, intf_i)));
      }
      if( ! some_good )
        log("WARNING: Request for SOF_TIMESTAMPING_RX_HARDWARE when "
            "RX timestamps are off for ALL Onload network interfaces.  "
            "Try setting EF_RX_TIMESTAMPING.");
    }


    /* SOF_TIMESTAMPING_OPT_ID support for TX */
    if( v & ONLOAD_SOF_TIMESTAMPING_OPT_ID &&
        ~s->timestamping_flags & ONLOAD_SOF_TIMESTAMPING_OPT_ID ) {
      s->ts_key = 0;
      if( s->b.state & CI_TCP_STATE_TCP_CONN )
        s->ts_key = SOCK_TO_TCP(s)->snd_una;
    }

    rc = 0;
    s->timestamping_flags = v;
    if ( (s->b.state & CI_TCP_STATE_TCP) &&
         (~s->b.state & CI_TCP_STATE_NOT_CONNECTED) )
      ci_tcp_set_sndbuf(netif, SOCK_TO_TCP(s));

    /* cmsg_flags is used for RX path only.  Set a flags in it: */
    if( v & (ONLOAD_SOF_TIMESTAMPING_RX_HARDWARE |
             ONLOAD_SOF_TIMESTAMPING_RX_SOFTWARE) )
      s->cmsg_flags |= CI_IP_CMSG_TIMESTAMPING;
    else
      s->cmsg_flags &= ~CI_IP_CMSG_TIMESTAMPING;
    break;
#endif

#ifdef SO_SELECT_ERR_QUEUE
  case SO_SELECT_ERR_QUEUE:
    if( (rc = opt_not_ok(optval, optlen, char)) )
      goto fail_inval;
    if( ci_get_optval(optval, optlen) )
      ci_bit_set(&s->s_aflags, CI_SOCK_AFLAG_SELECT_ERR_QUEUE_BIT);
    else
      ci_bit_clear(&s->s_aflags, CI_SOCK_AFLAG_SELECT_ERR_QUEUE_BIT);
    break;
#endif

  default:
    /* SOL_SOCKET options that are defined to fail with ENOPROTOOPT:
     *  SO_TYPE,  CI_SOSNDLOWAT,
     *  SO_ERROR, SO_ACCEPTCONN
     */
    goto fail_noopt;
  }

  /* Success */
  return 0;

 fail_inval:
  LOG_SC(log("%s: "NS_FMT" option %i ptr/len error (EINVAL or EFAULT)",
             __FUNCTION__, NS_PRI_ARGS(netif, s), optname));
  RET_WITH_ERRNO( -rc );

 fail_noopt:
  LOG_SC(log("%s: "NS_FMT" unimplemented/bad option %i (ENOPROTOOPT)",
             __FUNCTION__, NS_PRI_ARGS(netif, s), optname));
  RET_WITH_ERRNO( ENOPROTOOPT );

 fail_other:
  RET_WITH_ERRNO(-rc);
}


int ci_opt_is_setting_reuseport(int level, int optname, const void* optval,
                                socklen_t optlen)
{
  if( level == SOL_SOCKET && optname == SO_REUSEPORT &&
      opt_ok(optval, optlen, unsigned) && *(unsigned*)optval == 1 )
    return 1;
  return 0;
}

int ci_setsockopt_os_fail_ignore(ci_netif* ni, ci_sock_cmn* s, int err,
                                 int level, int optname,
                                 const void* optval, socklen_t optlen)
{
  if( level == SOL_SOCKET && optname == ONLOAD_SO_BUSY_POLL &&
           optlen >= sizeof(int) ) 
    return 1;
#if CI_CFG_TIMESTAMPING
  else if( (s->b.state & CI_TCP_STATE_TCP) && level == SOL_SOCKET &&
           ( optname == SO_TIMESTAMP || optname == SO_TIMESTAMPNS ||
             optname == ONLOAD_SO_TIMESTAMPING ) &&
           optlen >= sizeof(int) )
    return 1;
#endif
  return 0;
}


/* This function is the common handler for SOL_SOCKET level options that do
 * not require the stack lock to be held.  It is safe to call this function
 * with the lock held though, and this is done in both the TCP and UDP case.
 * In the TCP case this is because all options on a TCP socket must be set
 * with the stack lock held.  In the UDP case we do so because of lock
 * ordering requirements.
 */
int ci_set_sol_socket_nolock(ci_netif* ni, ci_sock_cmn* s, int optname,
			     const void* optval, socklen_t optlen)
{
  int rc = 1;  /* This means "not handled". */

  switch( optname ) {
  case SO_RCVTIMEO: {
    struct timeval *tv = (struct timeval *)optval;
    ci_uint64 timeo_usec;
    if( (rc = opt_not_ok(optval, optlen, struct timeval)) )
      goto fail_inval;
    timeo_usec = tv->tv_sec * 1000000ULL + tv->tv_usec;
    if( timeo_usec == 0 )
      s->so.rcvtimeo_msec = 0;
    else if( timeo_usec > 0xffffffffULL * 1000 )
      s->so.rcvtimeo_msec = -1; /* some weeks = MAX_UINT */
    else if( timeo_usec < 1000 )
      s->so.rcvtimeo_msec = 1; /* small timeout = 1 */
    else
      s->so.rcvtimeo_msec = tv->tv_sec * 1000 + tv->tv_usec / 1000;
    rc = 0;
    break;
  }

  case SO_SNDTIMEO: {
    struct timeval *tv = (struct timeval *)optval;
    ci_uint64 timeo_usec;
    if( (rc = opt_not_ok(optval, optlen, struct timeval)) )
      goto fail_inval;
    timeo_usec = tv->tv_sec * 1000000ULL + tv->tv_usec;
    if( timeo_usec == 0 )
      s->so.sndtimeo_msec = 0;
    else if( timeo_usec > 0xffffffffULL * 1000 )
      s->so.sndtimeo_msec = -1; /* some weeks = MAX_UINT */
    else if( timeo_usec < 1000 )
      s->so.sndtimeo_msec = 1; /* small timeout = 1 */
    else
      s->so.sndtimeo_msec = tv->tv_sec * 1000 + tv->tv_usec / 1000;
    rc = 0;
    break;
  }

  }
  return rc;

 fail_inval:
  RET_WITH_ERRNO(-rc);
}
#endif /* ifndef __KERNEL__ */

/*! \cidoxg_end */
