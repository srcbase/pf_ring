/*
 *
 * (C) 2005-07 - Luca Deri <deri@ntop.org>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * VLAN support courtesy of Vincent Magnin <vincent.magnin@ci.unil.ch>
 *
 */

#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/poll.h>
#include <time.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/ethernet.h>     /* the L2 protocols */

#include "pfring.h"

#define ALARM_SLEEP       1
#define DEFAULT_SNAPLEN 128
pfring  *pd;
int verbose = 0;
pfring_stat pfringStats;


static u_int64_t totPkts, totLost;
static struct timeval startTime;
unsigned long long numPkts = 0, numBytes = 0;

#define DEFAULT_DEVICE "eth0"

/* *************************************** */
/*
 * The time difference in millisecond
 */
double delta_time (struct timeval * now,
		   struct timeval * before) {
  time_t delta_seconds;
  time_t delta_microseconds;

  /*
   * compute delta in second, 1/10's and 1/1000's second units
   */
  delta_seconds      = now -> tv_sec  - before -> tv_sec;
  delta_microseconds = now -> tv_usec - before -> tv_usec;

  if(delta_microseconds < 0) {
    /* manually carry a one from the seconds field */
    delta_microseconds += 1000000;  /* 1e6 */
    -- delta_seconds;
  }
  return((double)(delta_seconds * 1000) + (double)delta_microseconds/1000);
}

/* ******************************** */

void print_stats() {
  pfring_stat pfringStat;
  struct timeval endTime;
  double deltaMillisec;
  static u_int64_t lastPkts = 0;
  u_int64_t diff;
  static struct timeval lastTime;

  if(startTime.tv_sec == 0) return;

  gettimeofday(&endTime, NULL);
  deltaMillisec = delta_time(&endTime, &startTime);

  if(pfring_stats(pd, &pfringStat) >= 0)
    fprintf(stderr, "=========================\n"
	    "Absolute Stats: [%u pkts rcvd][%u pkts dropped]\n"
	    "Total Pkts=%d/Dropped=%.1f %%\n",
	    pfringStat.recv, pfringStat.drop, pfringStat.recv-pfringStat.drop,
	    pfringStat.recv == 0 ? 0 : (double)(pfringStat.drop*100)/(double)pfringStat.recv);
  fprintf(stderr, "%llu pkts [%.1f pkt/sec] - %llu bytes [%.2f Mbit/sec]\n",
	  numPkts, (double)(numPkts*1000)/deltaMillisec,
	  numBytes, (double)8*numBytes/(double)(deltaMillisec));

  if(lastTime.tv_sec > 0) {
    deltaMillisec = delta_time(&endTime, &lastTime);
    diff = pfringStat.recv-lastPkts;
    fprintf(stderr, "=========================\n"
	    "Actual Stats: %llu pkts [%.1f ms][%.1f pkt/sec]\n",
	    diff, deltaMillisec, ((double)diff/(double)(deltaMillisec/1000)));
  }

  lastPkts = pfringStat.recv;
  
  lastTime.tv_sec = endTime.tv_sec, lastTime.tv_usec = endTime.tv_usec;

  fprintf(stderr, "=========================\n");
}

/* ******************************** */

void sigproc(int sig) {
  static int called = 0;

  if(called) return; else called = 1;

  print_stats();
  pfring_close(pd);
  exit(0);
}

/* ******************************** */

void my_sigalarm(int sig) {
  print_stats();
  printf("\n");
  alarm(ALARM_SLEEP);
  signal(SIGALRM, my_sigalarm);
}

/* ****************************************************** */

static char hex[] = "0123456789ABCDEF";

char* etheraddr_string(const u_char *ep, char *buf) {
  u_int i, j;
  char *cp;

  cp = buf;
  if ((j = *ep >> 4) != 0)
    *cp++ = hex[j];
  else
    *cp++ = '0';

  *cp++ = hex[*ep++ & 0xf];

  for(i = 5; (int)--i >= 0;) {
    *cp++ = ':';
    if ((j = *ep >> 4) != 0)
      *cp++ = hex[j];
    else
      *cp++ = '0';

    *cp++ = hex[*ep++ & 0xf];
  }

  *cp = '\0';
  return (buf);
}

/* ****************************************************** */

/*
 * A faster replacement for inet_ntoa().
 */
char* _intoa(unsigned int addr, char* buf, u_short bufLen) {
  char *cp, *retStr;
  u_int byte;
  int n;

  cp = &buf[bufLen];
  *--cp = '\0';

  n = 4;
  do {
    byte = addr & 0xff;
    *--cp = byte % 10 + '0';
    byte /= 10;
    if (byte > 0) {
      *--cp = byte % 10 + '0';
      byte /= 10;
      if (byte > 0)
	*--cp = byte + '0';
    }
    *--cp = '.';
    addr >>= 8;
  } while (--n > 0);

  /* Convert the string to lowercase */
  retStr = (char*)(cp+1);

  return(retStr);
}

/* ************************************ */

char* intoa(unsigned int addr) {
  static char buf[sizeof "ff:ff:ff:ff:ff:ff:255.255.255.255"];

  return(_intoa(addr, buf, sizeof(buf)));
}

/* ****************************************************** */

char* proto2str(u_short proto) {
  static char protoName[8];

  switch(proto) {
  case IPPROTO_TCP:  return("TCP");
  case IPPROTO_UDP:  return("UDP");
  case IPPROTO_ICMP: return("ICMP");
  default:
    snprintf(protoName, sizeof(protoName), "%d", proto);
    return(protoName);
  }
}

/* ****************************************************** */

static int32_t thiszone;

void dummyProcesssPacket(const struct pfring_pkthdr *h, const u_char *p) {
  if(verbose) {
    struct ether_header ehdr;
    u_short eth_type, vlan_id;
    char buf1[32], buf2[32];
    struct ip ip;
    int s = (h->ts.tv_sec + thiszone) % 86400;

    printf("[eth_type=0x%04X]", h->eth_type);
    printf("[l3_proto=%u]", (unsigned int)h->l3_proto);
    printf("[%s:%d -> ", intoa(h->ipv4_src), h->l4_src_port);
    printf("%s:%d] ", intoa(h->ipv4_dst), h->l4_dst_port);
    printf("%02d:%02d:%02d.%06u ",
	   s / 3600, (s % 3600) / 60, s % 60,
	   (unsigned)h->ts.tv_usec);

    memcpy(&ehdr, p, sizeof(struct ether_header));
    eth_type = ntohs(ehdr.ether_type);
    printf("[%s -> %s] ",
	   etheraddr_string(ehdr.ether_shost, buf1),
	   etheraddr_string(ehdr.ether_dhost, buf2));
     
    if(eth_type == 0x8100) {
      vlan_id = (p[14] & 15)*256 + p[15];
      eth_type = (p[16])*256 + p[17];
      printf("[vlan %u] ", vlan_id);
      p+=4;
    }
    if(eth_type == 0x0800) {
      memcpy(&ip, p+sizeof(ehdr), sizeof(struct ip));
      printf("[%s:%d ", intoa(ntohl(ip.ip_src.s_addr)), h->l4_src_port);
      printf("-> %s:%d] ", intoa(ntohl(ip.ip_dst.s_addr)), h->l4_dst_port);
    } else if(eth_type == 0x0806)
      printf("[ARP]");
    else
      printf("[eth_type=0x%04X]", eth_type);
    
    printf("[tos=%d][tcp_flags=%d][caplen=%d][len=%d]\n", 
	   h->ipv4_tos, h->tcp_flags, h->caplen, h->len);
  }

  if(numPkts == 0) gettimeofday(&startTime, NULL);
  numPkts++, numBytes += h->len;
}

/* *************************************** */

int32_t gmt2local(time_t t) {
  int dt, dir;
  struct tm *gmt, *loc;
  struct tm sgmt;

  if (t == 0)
    t = time(NULL);
  gmt = &sgmt;
  *gmt = *gmtime(&t);
  loc = localtime(&t);
  dt = (loc->tm_hour - gmt->tm_hour) * 60 * 60 +
    (loc->tm_min - gmt->tm_min) * 60;

  /*
   * If the year or julian day is different, we span 00:00 GMT
   * and must add or subtract a day. Check the year first to
   * avoid problems when the julian day wraps.
   */
  dir = loc->tm_year - gmt->tm_year;
  if (dir == 0)
    dir = loc->tm_yday - gmt->tm_yday;
  dt += dir * 24 * 60 * 60;

  return (dt);
}

/* *************************************** */

void printHelp(void) {
  printf("pfcount\n(C) 2005-07 Deri Luca <deri@ntop.org>\n");
  printf("-h              [Print help]\n");
  printf("-i <device>     [Device name]\n");
  /* printf("-f <filter>     [pfring filter]\n"); */
  printf("-c <cluster id> [cluster id]\n");
  printf("-s <string>     [String to search on packets]\n");
  printf("-a              [Active packet wait]\n");
  printf("-v              [Verbose]\n");
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char *device = NULL, c, *bpfFilter = NULL, *string = NULL;
  int i, promisc;
  u_int clusterId = 0;
  u_char wait_for_packet = 1;

#if 0  
  struct sched_param schedparam;

  schedparam.sched_priority = 99;
  if(sched_setscheduler(0, SCHED_FIFO, &schedparam) == -1) {
    printf("error while setting the scheduler, errno=%i\n",errno);
    exit(1);
  }      

  mlockall(MCL_CURRENT|MCL_FUTURE);

#define TEST_PROCESSOR_AFFINITY
#ifdef TEST_PROCESSOR_AFFINITY
  {
    unsigned long new_mask = 1;
    unsigned int len = sizeof(new_mask);
    unsigned long cur_mask;
    pid_t p = 0; /* current process */
    int ret;
   
    ret = sched_getaffinity(p, len, NULL);
    printf(" sched_getaffinity = %d, len = %u\n", ret, len);
   
    ret = sched_getaffinity(p, len, &cur_mask);
    printf(" sched_getaffinity = %d, cur_mask = %08lx\n", ret, cur_mask);
   
    ret = sched_setaffinity(p, len, &new_mask);
    printf(" sched_setaffinity = %d, new_mask = %08lx\n", ret, new_mask);
   
    ret = sched_getaffinity(p, len, &cur_mask);
    printf(" sched_getaffinity = %d, cur_mask = %08lx\n", ret, cur_mask);
  }
#endif
#endif

  thiszone = gmt2local(0);

  while((c = getopt(argc,argv,"hi:c:vs:a" /* "f:" */)) != -1) {
    switch(c) {
    case 'h':
      printHelp();
      return(0);
      break;
    case 'a':
      wait_for_packet = 0;
      break;
    case 'c':
      clusterId = atoi(optarg);
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 'v':
      verbose = 1;
      break;
      /*
	case 'f':
	bpfFilter = strdup(optarg);
	break;
      */
    case 's':
      string = strdup(optarg);
      break;
    }
  }

  if(device == NULL) device = DEFAULT_DEVICE;

  printf("Capturing from %s\n", device);

  /* hardcode: promisc=1, to_ms=500 */
  promisc = 1;
  if((pd = pfring_open(device, promisc)) == NULL) {
    printf("pfring_open error\n");
    return(-1);
  } else {
    u_int32_t version;

    pfring_version(pd, &version);

    printf("Using PF_RING v.%d.%d.%d\n", 
	   (version & 0xFFFF0000) >> 16,
	   (version & 0x0000FF00) >> 8,
	   version & 0x000000FF);	   
  }
  
  if(clusterId > 0) {
    int rc = pfring_set_cluster(pd, clusterId);    
    printf("pfring_set_cluster returned %d\n", rc);
  }

  if(1) {
    filtering_rule rule;
    
    struct dummy_filter {      
      u_int32_t src_host;      
    };    

    pfring_toggle_filtering_policy(pd, 0); /* Default to drop */

    memset(&rule, 0, sizeof(rule));

    if(1) {
      struct dummy_filter filter;
      
      filter.src_host = ntohl(inet_addr("192.168.1.12"));

#if 1
      rule.rule_id = 5; 
      rule.rule_action = forward_packet_and_stop_rule_evaluation;
      rule.core_fields.proto = 1;
      rule.core_fields.host_low = 0, rule.core_fields.host_high = 0;
      rule.plugin_action.plugin_id = 1; /* Dummy plugin */

      rule.extended_fields.filter_plugin_id = 1; /* Dummy plugin */
      memcpy(rule.extended_fields.filter_plugin_data, &filter, sizeof(filter));
      /* strcpy(rule.extended_fields.payload_pattern, "hello"); */
#else
      rule.rule_id = 5; 
      rule.rule_action = forward_packet_and_stop_rule_evaluation;
      rule.core_fields.port_low = 80, rule.core_fields.port_high = 80;
      rule.core_fields.host_low = rule.core_fields.host_high = ntohl(inet_addr("192.168.0.160"));
      snprintf(rule.extended_fields.payload_pattern, sizeof(rule.extended_fields.payload_pattern), "GET");
#endif
      if(pfring_add_filtering_rule(pd, &rule) < 0)
	printf("pfring_add_filtering_rule() failed\n");
    } else {
      rule.rule_id = 10; pfring_add_filtering_rule(pd, &rule);
      rule.rule_id = 5;  pfring_add_filtering_rule(pd, &rule);
      rule.rule_id = 15; pfring_add_filtering_rule(pd, &rule);
      rule.rule_id = 5;  pfring_add_filtering_rule(pd, &rule);
      if(pfring_remove_filtering_rule(pd, 15) < 0)
	printf("pfring_remove_filtering_rule() failed\n");
    }
  }

  signal(SIGINT, sigproc);

  if(!verbose) {
    signal(SIGALRM, my_sigalarm);
    alarm(ALARM_SLEEP);
  }

  while(1) {
    struct simple_stats {
      u_int64_t num_pkts, num_bytes;
    };
    
    u_char buffer[2048];
    struct simple_stats stats;
    struct pfring_pkthdr hdr;
    int rc, len;
    
    if(pfring_recv(pd, (char*)buffer, sizeof(buffer), &hdr, wait_for_packet) > 0)
      dummyProcesssPacket(&hdr, buffer);

    len = sizeof(stats);
    rc = pfring_get_filtering_rule_stats(pd, 5, (char*)&stats, &len);
    if(rc < 0)
      printf("pfring_get_filtering_rule_stats() failed [rc=%d]\n", rc);
    else {
      printf("[Pkts=%u][Bytes=%u]\n", 
	     (unsigned long)stats.num_pkts, (unsigned long)stats.num_bytes);
    }
  }

  pfring_close(pd);

  return(0);
}
