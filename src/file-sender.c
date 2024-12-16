#include "packet-format.h"
#include <limits.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

// Function to get the file offset for a specific sequence number
long get_file_offset_for_chunk(int seq_num) {
    return (seq_num) * MAX_CHUNK_SIZE;  
}

// gets number of chunks in the file
int get_num_chunks(FILE *file) {
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  
  int num_chunks;
  if (file_size % MAX_CHUNK_SIZE == 0) {
    num_chunks = (file_size / MAX_CHUNK_SIZE) + 1;
  } else {
    num_chunks = (file_size + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE;
  }
  
  fseek(file, 0, SEEK_SET);
  return num_chunks;
}

void send_packet(int seq_num, FILE *file, int sockfd, struct sockaddr *srv_addr, socklen_t addr_len) {
    
    data_pkt_t data_pkt;
    // Calculate the file offset for the current chunk based on the seq_num
    long offset = get_file_offset_for_chunk(seq_num);
    
    // Set the file pointer to the correct position for this chunk
    if (fseek(file, offset, SEEK_SET) != 0) {
        perror("Error seeking to the correct chunk in the file");
        exit(EXIT_FAILURE);
    }

    // Read the chunk data from the file
    size_t data_len = fread(data_pkt.data, 1, sizeof(data_pkt.data), file);
    if (data_len < sizeof(data_pkt.data) && ferror(file)) {
        perror("Error reading from file");
        exit(EXIT_FAILURE);
    }

    data_pkt.seq_num = htonl(seq_num);
    
    // Send the data packet to the receiver
    ssize_t sent_len = sendto(sockfd, &data_pkt, offsetof(data_pkt_t, data) + data_len, 0,
                              srv_addr, addr_len);
    

    if (sent_len != offsetof(data_pkt_t, data) + data_len) {
        fprintf(stderr, "Failed to send packet %d\n", seq_num);
        exit(EXIT_FAILURE);
    }
    
    printf("DataPkt(%d) sent. Size %ld bytes.\n", ntohl(data_pkt.seq_num),
           offsetof(data_pkt_t, data) + data_len);
}


// sends all unacknowledged packets in the window
void send_unacknowledged_packets(uint32_t window_base, FILE *file, int sockfd, struct sockaddr *srv_addr, socklen_t addr_len, int window[], int window_size) {
    // Send all unacknowledged packets in the window
    for (int i = 0; i < window_size; i++) {
        if (window[i] == 0) {
          if(window_base + i < get_num_chunks(file)){
          send_packet(window_base + i, file, sockfd, srv_addr, addr_len);
          }
        }
    }
}
int slide_window(int window[], int window_size, uint32_t window_base) {
    
    // Count the number of contiguous acknowledged packets at the beginning of the window
    int shift_count = 0;
    for (int i = 0; i < window_size; i++) {
        if (window[i] == 1) {
            shift_count++;
        } else {
            break;
        }
    }

    printf("shift count: %d\n", shift_count);
    // Shift the window array left by shift_count positions
    for (int i = 0; i < window_size - shift_count; i++) {
        window[i] = window[i + shift_count];
    }

    // Fill the trailing positions with 0, as they are now empty
    for (int i = window_size - shift_count; i < window_size; i++) {
        window[i] = 0;
    }

    // Update the base sequence number to reflect the shift
    window_base += shift_count;
    printf("Packet %d is the new sender window base.\n", window_base);
    return window_base;
}

void set_timeout(int sockfd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}


int main(int argc, char *argv[]) {
  char *file_name = argv[1];
  char *host = argv[2];
  int port = atoi(argv[3]);
  int window_size = atoi(argv[4]);

  if (window_size > MAX_WINDOW_SIZE){ 
    fprintf(stderr, "Window size cannot exceed %d.\n", MAX_WINDOW_SIZE);
    exit(EXIT_FAILURE);
  }
  
  FILE *file = fopen(file_name, "r");
  if (!file) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  // initialize window
  int window[window_size];
  for (int i = 0; i < window_size; i++){
    window[i] = 0;
  }

  // Prepare server host address.
  struct hostent *he;
  if (!(he = gethostbyname(host))) {
    perror("gethostbyname");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in srv_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr = *((struct in_addr *)he->h_addr),
  };

  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  uint32_t seq_num = 0;

  int unacknowledged_chunks = get_num_chunks(file);
  uint32_t window_base = 0;
  
  // Send initial packets
  while(seq_num < window_base + window_size){
    send_packet(seq_num, file, sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    seq_num++;
  }
  
  int num_chunks = get_num_chunks(file);

  ack_pkt_t ack_pkt;

  set_timeout(sockfd, TIMEOUT);
  int timeout_counter = 0;
  int duplicate_count = 0;
  int is_receiver_known = 0;
  struct sockaddr_in receiver_addr;
  

  while(1) {
    
    // Receive ACKs
    ssize_t len = recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0, (struct sockaddr *)&srv_addr, &(socklen_t){sizeof(srv_addr)});
      

    // Stores the receiver's address and port
    if(!is_receiver_known){
      receiver_addr = srv_addr;
      is_receiver_known = 1;
    }

    // Ignores datagrams from other sources
    if (!(srv_addr.sin_addr.s_addr == receiver_addr.sin_addr.s_addr &&
    srv_addr.sin_port == receiver_addr.sin_port)) {
      printf("Received datagram from unexpected source. Ignoring.\n");
      continue; 
    }

    // If we don't receive any ACKs after 1 second (socket time out)
    if (len == -1) { 

      timeout_counter++;
      //printf("Socket timed out: %d\n time", timeout_counter);
      
      // 3 consecutive timeouts without acks result in termination 
      if (timeout_counter == 3) {
        fprintf(stderr, "Socket timed out 3 consecutive times. Sender terminates.\n");
        exit(EXIT_FAILURE);
      } 

      // SEND ALL UNACKNOWLEDGED PACKETS IN THE WINDOW
      printf("Socket timed out, retransmitting all unacknowledged packets in the window...\n");
      send_unacknowledged_packets(window_base, file, sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr), window, window_size);
      
      continue;
    }

    printf("ACK(%d) received.\n", ntohl(ack_pkt.seq_num));
    timeout_counter = 0;
    uint32_t ack_num = ntohl(ack_pkt.seq_num);
    uint32_t selective_acks = ntohl(ack_pkt.selective_acks);

    // handles dupACKs
    if(ack_num == window_base){
      
      duplicate_count++;

      // handles selective acks
      if(selective_acks > 0){

        int i = 0;

        while (selective_acks > 0) {
          
          // This bit is 1, meaning the packet has to be selectively acknowledged
          if (selective_acks & 1) {
              
              // If the selective ack is within the window bounds we acknowledge it
              if( window_base + 1 + i <= window_base + window_size - 1){
                // if not acknoweldged, mark it as acknowledged
                int index = i + 1;
                if(!window[index]){
                  window[index] = 1;
                  unacknowledged_chunks--;
                  printf("Packet %d has been selectively acknowledged.\n", window_base + 1 + i);
                }
              }
          }

          // Shift right to the next bit
          selective_acks >>= 1;
          i++;
        }
          
      }
    
      if(duplicate_count == FAST_RETRANSMIT_DUPACKS){
        send_packet(window_base, file, sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
        printf("3 duplicate ACKs received for packet %u. Fast retransmitting packet %u...\n", ack_num, window_base);
      }

      continue;
    }

    else if(ack_num > window_base){

      // we only process the ack if it is within the bounds of the window
      if( ack_num >= window_base && ack_num <= window_base + window_size){
        
        duplicate_count = 0;
        int difference = ack_num - window_base;
  
        // Mark all packets up to the acknowledged packet as acknowledged
        for(int i = 0; i < difference; i++){
          
          if(!window[i]){
            printf("Packet with sequence number %d acknowledged.\n", window_base + i);
            window[i] = 1;
            unacknowledged_chunks--;
          }
        }
  
        // if window_base is still not the last chunk
        if (window_base < num_chunks) {
        
          int old_window_base = window_base;
          // Slide the window forward
          window_base = slide_window(window, window_size, window_base);

          int shift = window_base - old_window_base;

          // Termination Condition 1: Successfully acknowledged all packets
          if (unacknowledged_chunks == 0 && window_base == num_chunks) {
              printf("All packets successfully acknowledged. Terminating...\n");
              exit(EXIT_SUCCESS);
          }

          printf("WINDOW_BASE:%d\n",window_base);
          printf("unacknowledged_chunks:%d\n",unacknowledged_chunks);
          
          if(window_base + window_size - shift < num_chunks){
            send_packet(window_base + window_size - shift, file, sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));           
          }
        } 
      } else{
        printf("Received ACK(%d) which is out of the sender window bounds. Ignoring.\n", ack_num);
      }
    }
  }

  // Clean up and exit.
  close(sockfd);
  fclose(file);

  printf("Sender terminating successfully...\n");
  return EXIT_SUCCESS;
}

