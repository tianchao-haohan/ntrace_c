#ifndef __ICMP_H__
#define __ICMP_H__

#include <stdint.h>

/* ICMP type */
#define ICMP_ECHOREPLY      0  /* Echo Reply */
#define ICMP_DEST_UNREACH   3  /* Destination Unreachable */
#define ICMP_SOURCE_QUENCH  4  /* Source Quench */
#define ICMP_REDIRECT       5  /* Redirect (change route) */
#define ICMP_ECHO           8  /* Echo Request */
#define ICMP_TIME_EXCEEDED  11 /* Time Exceeded */
#define ICMP_PARAMETERPROB  12 /* Parameter Problem */
#define ICMP_TIMESTAMP      13 /* Timestamp Request */
#define ICMP_TIMESTAMPREPLY 14 /* Timestamp Reply */
#define ICMP_INFO_REQUEST   15 /* Information Request */
#define ICMP_INFO_REPLY     16 /* Information Reply */
#define ICMP_ADDRESS        17 /* Address Mask Request */
#define ICMP_ADDRESSREPLY   18 /* Address Mask Reply */
#define NR_ICMP_TYPES       18

static char *icmpTypeName [] = {
    "ICMP_ECHOREPLY",
    "ICMP_DEST_UNREACH",
    "ICMP_SOURCE_QUENCH",
    "ICMP_REDIRECT",
    "ICMP_ECHO",
    "ICMP_TIME_EXCEEDED",
    "ICMP_PARAMETERPROB",
    "ICMP_TIMESTAMP",
    "ICMP_TIMESTAMPREPLY",
    "ICMP_INFO_REQUEST",
    "ICMP_INFO_REPLY",
    "ICMP_ADDRESS",
    "ICMP_ADDRESSREPLY",
    "NR_ICMP_TYPES"
};

/* Codes for UNREACH. */
#define ICMP_NET_UNREACH    0  /* Network Unreachable */
#define ICMP_HOST_UNREACH   1  /* Host Unreachable */
#define ICMP_PROT_UNREACH   2  /* Protocol Unreachable */
#define ICMP_PORT_UNREACH   3  /* Port Unreachable */
#define ICMP_FRAG_NEEDED    4  /* Fragmentation Needed/DF set */
#define ICMP_SR_FAILED      5  /* Source Route failed */
#define ICMP_NET_UNKNOWN    6
#define ICMP_HOST_UNKNOWN   7
#define ICMP_HOST_ISOLATED  8
#define ICMP_NET_ANO        9
#define ICMP_HOST_ANO       10
#define ICMP_NET_UNR_TOS    11
#define ICMP_HOST_UNR_TOS   12
#define ICMP_PKT_FILTERED   13  /* Packet filtered */
#define ICMP_PREC_VIOLATION 14  /* Precedence violation */
#define ICMP_PREC_CUTOFF    15  /* Precedence cut off */
#define NR_ICMP_UNREACH     15  /*instead of hardcoding immediate value */

static char *icmpDestUnreachCodeName [] = {
    "ICMP_NET_UNREACH",
    "ICMP_HOST_UNREACH",
    "ICMP_PROT_UNREACH",
    "ICMP_PORT_UNREACH",
    "ICMP_FRAG_NEEDED",
    "ICMP_SR_FAILED",
    "ICMP_NET_UNKNOWN",
    "ICMP_HOST_UNKNOWN",
    "ICMP_HOST_ISOLATED",
    "ICMP_NET_ANO",
    "ICMP_HOST_ANO",
    "ICMP_NET_UNR_TOS",
    "ICMP_HOST_UNR_TOS",
    "ICMP_PKT_FILTERED",
    "ICMP_PREC_VIOLATION",
    "ICMP_PREC_CUTOFF",
    "NR_ICMP_UNREACH"
};

typedef struct _icmphdr icmphdr;
typedef icmphdr *icmphdrPtr;

struct _icmphdr {
    uint8_t type;                       /**< ICMP message type */
    uint8_t code;                       /**< ICMP type sub-code */
    uint16_t chkSum;                    /**< ICMP checksum */
    uint32_t data;                      /**< ICMP data */
};

#endif /* __ICMP_H__ */
