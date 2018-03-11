#ifndef RDT_H
#define RDT_H

#include <stdint.h>

const int MAX_PACKET_SIZE = 1024;        // in bytes
const uint64_t SEQ_GEN_MAX = 10000;
const int MAX_SEQ_NUM = 30720;
const int BASE_RCV_WINDOW = 5120;        // in bytes
const int TIMEOUT = 500;                 // in milliseconds

struct packet_hdr {                      //rdt packet
  uint64_t seq_n;
  uint64_t ack_n;
  uint16_t rcv_window;
  char ack_flag;
  char syn_flag;
  char fin_flag;
};

#endif
