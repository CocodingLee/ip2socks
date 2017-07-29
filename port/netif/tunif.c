/**
 * based on lwip-contrib
 */
#include "netif/tunif.h"
#include "netif/socket_util.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "lwip/ip.h"


#if LWIP_IPV4 /* @todo: IPv6 */

#define IFNAME0 't'
#define IFNAME1 'n'

#ifndef TUNIF_DEBUG
#define TUNIF_DEBUG LWIP_DBG_OFF
#endif

#if defined(LWIP_UNIX_LINUX)

#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <fcntl.h>


#ifndef DEVTUN_DEFAULT_IF
#define DEVTUN_DEFAULT_IF "tun0"
#endif
#ifndef DEVTUN
#define DEVTUN "/dev/net/tun"
#endif
#define NETMASK_ARGS "netmask %d.%d.%d.%d"
#define IFCONFIG_ARGS "tun0 inet %d.%d.%d.%d " NETMASK_ARGS
#elif defined(LWIP_UNIX_OPENBSD)
#define DEVTUN "/dev/tun0"
#define NETMASK_ARGS "netmask %d.%d.%d.%d"
#define IFCONFIG_ARGS "tun0 inet %d.%d.%d.%d " NETMASK_ARGS " link0"
#else /* others */
#define DEVTUN "/dev/tun0"
#define NETMASK_ARGS "netmask %d.%d.%d.%d"
#define IFCONFIG_ARGS "tun0 inet %d.%d.%d.%d " NETMASK_ARGS
#endif

#if defined(LWIP_UNIX_MACH)

#include <sys/kern_control.h>
#include <net/if_utun.h>
#include <sys/sys_domain.h>
#include <netinet/ip.h>
#include <sys/kern_event.h>
#include <sys/ioctl.h>

#endif /* LWIP_UNIX_MACH */

struct tunif {
    /* Add whatever per-interface state that is needed here. */
    int fd;
};

/* Forward declarations. */
void tunif_input(struct netif *netif);

static err_t tunif_output(struct netif *netif, struct pbuf *p,
                          const ip4_addr_t *ipaddr);


#if defined(LWIP_UNIX_MACH)

static inline int utun_modified_len(int len) {
  if (len > 0)
    return (len > sizeof(u_int32_t)) ? len - sizeof(u_int32_t) : 0;
  else
    return len;
}

static int utun_write(int fd, void *buf, size_t len) {
  u_int32_t type;
  struct iovec iv[2];
  struct ip *iph;

  iph = (struct ip *) buf;

  if (iph->ip_v == 6)
    type = htonl(AF_INET6);
  else
    type = htonl(AF_INET);

  iv[0].iov_base = &type;
  iv[0].iov_len = sizeof(type);
  iv[1].iov_base = buf;
  iv[1].iov_len = len;

  return utun_modified_len(writev(fd, iv, 2));
}

static int utun_read(int fd, void *buf, size_t len) {
  u_int32_t type;
  struct iovec iv[2];

  iv[0].iov_base = &type;
  iv[0].iov_len = sizeof(type);
  iv[1].iov_base = buf;
  iv[1].iov_len = len;

  return utun_modified_len(readv(fd, iv, 2));
}

#endif /* LWIP_UNIX_MACH */

#if defined(LWIP_UNIX_MACH)
#define tun_read(...) utun_read(__VA_ARGS__)
#define tun_write(...) utun_write(__VA_ARGS__)
#endif /* LWIP_UNIX_MACH */

/*-----------------------------------------------------------------------------------*/

int tun_create(char *dev) {
  int fd;
#if defined(LWIP_UNIX_LINUX)
  struct ifreq ifr;
  int err;
  char *clonedev = "/dev/net/tun";

  if ((fd = open(clonedev, O_RDWR)) < 0) {
    return fd;
  }

  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

  if ((err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0) {
    close(fd);
    return err;
  }

  printf("Open tun/tap device: %s for reading...\n", ifr.ifr_name);
#endif

#if defined(LWIP_UNIX_MACH)
  struct ctl_info ctlInfo;
  struct sockaddr_ctl sc;
  int utunnum;

  if (dev == NULL) {
    printf("utun device name cannot be null");
    return -1;
  }
  if (sscanf(dev, "utun%d", &utunnum) != 1) {
    printf("invalid utun device name: %s", dev);
    return -1;
  }

  memset(&ctlInfo, 0, sizeof(ctlInfo));
  if (strlcpy(ctlInfo.ctl_name, UTUN_CONTROL_NAME, sizeof(ctlInfo.ctl_name)) >=
      sizeof(ctlInfo.ctl_name)) {
    printf("can not setup utun device: UTUN_CONTROL_NAME too long");
    return -1;
  }

  fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);

  if (fd == -1) {
    printf("socket[SYSPROTO_CONTROL]");
    return -1;
  }

  if (ioctl(fd, CTLIOCGINFO, &ctlInfo) == -1) {
    close(fd);
    printf("ioctl[CTLIOCGINFO]");
    return -1;
  }

  sc.sc_id = ctlInfo.ctl_id;
  sc.sc_len = sizeof(sc);
  sc.sc_family = AF_SYSTEM;
  sc.ss_sysaddr = AF_SYS_CONTROL;
  sc.sc_unit = utunnum + 1;

  if (connect(fd, (struct sockaddr *) &sc, sizeof(sc)) == -1) {
    close(fd);
    printf("connect[AF_SYS_CONTROL]");
    return -1;
  }
#endif /* LWIP_UNIX_MACH */

  return fd;
}

static void
low_level_init(struct netif *netif) {
  struct tunif *tunif;
#if defined(LWIP_UNIX_MACH)
  char tun_name[16];
  memcpy(tun_name, "utun7", 5);
#endif /* LWIP_UNIX_MACH */

#if defined(LWIP_UNIX_LINUX)
  char tun_name[IFNAMSIZ];
  tun_name[0] = '\0';
#endif
  printf("tun name is %s", tun_name);


  tunif = (struct tunif *) netif->state;

  /* Obtain MAC address from network interface. */

  /* Do whatever else is needed to initialize interface. */

  tunif->fd = tun_create(tun_name);
  printf("tunif_init: fd %d %s\n", tunif->fd, tun_name);
  if (tunif->fd < 1) {
    perror("tunif_init failed\n");
    exit(1);
  }

  setnonblocking(tunif->fd);
}
/*-----------------------------------------------------------------------------------*/
/*
 * low_level_output():
 *
 * Should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 */
/*-----------------------------------------------------------------------------------*/

static err_t
low_level_output(struct tunif *tunif, struct pbuf *p) {
  char buf[1500];
  int ret;

  /* initiate transfer(); */

  pbuf_copy_partial(p, buf, p->tot_len, 0);

  /* signal that packet should be sent(); */
#if defined(LWIP_UNIX_MACH)
  ret = tun_write(tunif->fd, buf, p->tot_len);
#endif
#if defined(LWIP_UNIX_LINUX)
  ret = write(tunif->fd, buf, p->tot_len);
#endif
  if (ret == -1) {
    perror("tunif: write failed\n");
  }
  return ERR_OK;
}
/*-----------------------------------------------------------------------------------*/
/*
 * low_level_input():
 *
 * Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 */
/*-----------------------------------------------------------------------------------*/
static struct pbuf *
low_level_input(struct tunif *tunif) {
  struct pbuf *p;
  ssize_t len;
  char buf[1500];

  /* Obtain the size of the packet and put it into the "len"
     variable. */
#if defined(LWIP_UNIX_MACH)
  len = tun_read(tunif->fd, buf, sizeof(buf));
#endif /* LWIP_UNIX_MACH */
#if defined(LWIP_UNIX_LINUX)
  len = read(tunif->fd, buf, sizeof(buf));
#endif
  if ((len <= 0) || (len > 0xffff)) {
    return NULL;
  }

  /* We allocate a pbuf chain of pbufs from the pool. */
  p = pbuf_alloc(PBUF_LINK, (u16_t) len, PBUF_POOL);

  if (p != NULL) {
    pbuf_take(p, buf, (u16_t) len);
    /* acknowledge that packet has been read(); */
  } else {
    /* drop packet(); */
    printf("pbuf_alloc failed\n");
    return NULL;
  }

  return p;
}

/*-----------------------------------------------------------------------------------*/
/*
 * tunif_output():
 *
 * This function is called by the TCP/IP stack when an IP packet
 * should be sent. It calls the function called low_level_output() to
 * do the actuall transmission of the packet.
 *
 */
/*-----------------------------------------------------------------------------------*/
static err_t
tunif_output(struct netif *netif, struct pbuf *p,
             const ip4_addr_t *ipaddr) {
  struct tunif *tunif;
  LWIP_UNUSED_ARG(ipaddr);

  tunif = (struct tunif *) netif->state;

  return low_level_output(tunif, p);

}
/*-----------------------------------------------------------------------------------*/
/*
 * tunif_input():
 *
 * This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface.
 *
 */
/*-----------------------------------------------------------------------------------*/
void
tunif_input(struct netif *netif) {
  struct tunif *tunif;
  struct pbuf *p;

  tunif = (struct tunif *) netif->state;

  p = low_level_input(tunif);

  if (p == NULL) {
    LWIP_DEBUGF(TUNIF_DEBUG, ("tunif_input: low_level_input returned NULL\n"));
    return;
  }

  err_t err = netif->input(p, netif);
  if (err != ERR_OK) {
    printf("============================> tapif_input: netif input error %s\n", lwip_strerr(err));
    pbuf_free(p);
  }
}
/*-----------------------------------------------------------------------------------*/
/*
 * tunif_init():
 *
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 */
/*-----------------------------------------------------------------------------------*/
err_t
tunif_init(struct netif *netif) {
  struct tunif *tunif;

  tunif = (struct tunif *) mem_malloc(sizeof(struct tunif));
  if (!tunif) {
    return ERR_MEM;
  }
  netif->state = tunif;
  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  netif->output = tunif_output;

  low_level_init(netif);
  return ERR_OK;
}
/*-----------------------------------------------------------------------------------*/

#endif /* LWIP_IPV4 */
