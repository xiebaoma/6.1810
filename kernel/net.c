#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

#define UDP_QSLOTS 16
#define UDP_NBOUND 64

struct udp_queue {
  int used;
  uint16 port;
  char *pkts[UDP_QSLOTS];
  int lens[UDP_QSLOTS];
  int head;
  int tail;
  int n;
};

static struct udp_queue udpqs[UDP_NBOUND];

static struct udp_queue*
udpq_lookup(uint16 port)
{
  for(int i = 0; i < UDP_NBOUND; i++){
    if(udpqs[i].used && udpqs[i].port == port)
      return &udpqs[i];
  }
  return 0;
}

static struct udp_queue*
udpq_alloc(uint16 port)
{
  for(int i = 0; i < UDP_NBOUND; i++){
    if(!udpqs[i].used){
      udpqs[i].used = 1;
      udpqs[i].port = port;
      udpqs[i].head = 0;
      udpqs[i].tail = 0;
      udpqs[i].n = 0;
      return &udpqs[i];
    }
  }
  return 0;
}

void
netinit(void)
{
  initlock(&netlock, "netlock");
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  int p;
  argint(0, &p);
  if(p < 0 || p > 0xffff)
    return -1;

  acquire(&netlock);
  struct udp_queue *q = udpq_lookup((uint16)p);
  if(q == 0)
    q = udpq_alloc((uint16)p);
  release(&netlock);

  if(q == 0)
    return -1;
  return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  int p;
  argint(0, &p);
  if(p < 0 || p > 0xffff)
    return -1;

  acquire(&netlock);
  struct udp_queue *q = udpq_lookup((uint16)p);
  if(q != 0){
    while(q->n > 0){
      char *pkt = q->pkts[q->head];
      q->head = (q->head + 1) % UDP_QSLOTS;
      q->n--;
      kfree(pkt);
    }
    q->used = 0;
    wakeup(q);
  }
  release(&netlock);

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  struct proc *p = myproc();
  int dporti;
  uint64 srcaddr, sportaddr, bufaddr;
  int maxlen;

  argint(0, &dporti);
  argaddr(1, &srcaddr);
  argaddr(2, &sportaddr);
  argaddr(3, &bufaddr);
  argint(4, &maxlen);

  if(dporti < 0 || dporti > 0xffff || maxlen < 0)
    return -1;

  uint16 dport = (uint16)dporti;
  char *pkt = 0;
  int pktlen = 0;

  acquire(&netlock);
  struct udp_queue *q = udpq_lookup(dport);
  if(q == 0){
    release(&netlock);
    return -1;
  }

  while(q->n == 0){
    sleep(q, &netlock);
    if(!q->used){
      release(&netlock);
      return -1;
    }
  }

  pkt = q->pkts[q->head];
  pktlen = q->lens[q->head];
  q->head = (q->head + 1) % UDP_QSLOTS;
  q->n--;
  release(&netlock);

  int hlen = sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(pktlen < hlen){
    kfree(pkt);
    return -1;
  }

  struct eth *eth = (struct eth *)pkt;
  struct ip *ip = (struct ip *)(eth + 1);
  struct udp *udp = (struct udp *)(ip + 1);
  int ulen = ntohs(udp->ulen);
  if(ulen < (int)sizeof(struct udp)){
    kfree(pkt);
    return -1;
  }

  int paylen = ulen - sizeof(struct udp);
  int avail = pktlen - hlen;
  if(paylen > avail)
    paylen = avail;
  if(paylen < 0){
    kfree(pkt);
    return -1;
  }

  int cc = paylen;
  if(cc > maxlen)
    cc = maxlen;

  uint32 src = ntohl(ip->ip_src);
  uint16 sport = ntohs(udp->sport);
  char *payload = (char *)(udp + 1);

  if(copyout(p->pagetable, srcaddr, (char *)&src, sizeof(src)) < 0 ||
     copyout(p->pagetable, sportaddr, (char *)&sport, sizeof(sport)) < 0 ||
     copyout(p->pagetable, bufaddr, payload, cc) < 0){
    kfree(pkt);
    return -1;
  }

  kfree(pkt);
  return cc;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  int hlen = sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(len < hlen){
    kfree(buf);
    return;
  }

  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);
  if(ip->ip_p != IPPROTO_UDP){
    kfree(buf);
    return;
  }

  struct udp *udp = (struct udp *)(ip + 1);
  uint16 dport = ntohs(udp->dport);

  acquire(&netlock);
  struct udp_queue *q = udpq_lookup(dport);
  if(q == 0 || q->n >= UDP_QSLOTS){
    release(&netlock);
    kfree(buf);
    return;
  }

  q->pkts[q->tail] = buf;
  q->lens[q->tail] = len;
  q->tail = (q->tail + 1) % UDP_QSLOTS;
  q->n++;
  wakeup(q);
  release(&netlock);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
