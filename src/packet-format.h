#include <stdint.h>

#define MAX_WINDOW_SIZE 32
#define TIMEOUT 1000 // ms
#define MAX_RETRIES 3
#define FAST_RETRANSMIT_DUPACKS 3
#define MAX_CHUNK_SIZE 1000

typedef struct __attribute__((__packed__)) data_pkt_t {
  uint32_t seq_num;
  char data[MAX_CHUNK_SIZE];
} data_pkt_t;

typedef struct __attribute__((__packed__)) ack_pkt_t {
  uint32_t seq_num;
  uint32_t selective_acks;
} ack_pkt_t;

