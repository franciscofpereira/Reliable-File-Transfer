#include "packet-format.h"
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>

#define RECEIVER_TIMEOUT 4000

int slide_window(int window[], int window_size, uint32_t window_base) {
    
    // Count the number of contiguous received packets at the beginning of the window
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
    printf("Packet %d is the new receiver window base.\n", window_base);
    return window_base;
}


void write_chunk_to_file(FILE *output_file, data_pkt_t *pkt, int seq_num, size_t len) {
    // Calculates starting byte of chunk
    long offset = (seq_num) * MAX_CHUNK_SIZE;  // seq_num starts at 1

    // Set the file pointer to the correct position for this chunk
    if (fseek(output_file, offset, SEEK_SET) != 0) {
        perror("Error seeking to the correct position in the output file");
        exit(EXIT_FAILURE);
    }

    // Write the data to the file
    size_t written_len = fwrite(pkt->data, 1, len - offsetof(data_pkt_t, data), output_file);
    if (written_len < MAX_CHUNK_SIZE && ferror(output_file)) {
        perror("Error writing to output file");
        exit(EXIT_FAILURE);
    }

    printf("Written chunk with sequence number %d at offset %ld bytes.\n", seq_num, offset);
}

void set_timeout(int sockfd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}


void process_selective_ack(int sockfd, struct sockaddr_in *sender_addr, uint32_t window_base, uint32_t seq_num, int window_size, ack_pkt_t *ack_pkt ){
   
  // Set the selective_acks field
  if (seq_num >= window_base && seq_num <= window_base + window_size - 1){
    uint32_t position = seq_num - (window_base + 1); 
    ack_pkt->selective_acks |= (1 << position);        
  }

}


void send_ack_packet(int sockfd, struct sockaddr_in *sender_addr, uint32_t window_base, int window_size, ack_pkt_t ack_pkt) {
    
    ack_pkt.seq_num = htonl(window_base);  // Convert sequence number to network byte order
    ack_pkt.selective_acks = htonl(ack_pkt.selective_acks);  // Convert selective acks to network byte order
    
    // Send the ACK packet to the sender
    ssize_t sent_len = sendto(sockfd, &ack_pkt, sizeof(ack_pkt), 0,
                               (struct sockaddr *)sender_addr, sizeof(*sender_addr));

    if (sent_len == -1) {
        perror("Error sending ACK");
        exit(EXIT_FAILURE);
    } else {
        printf("ACK(%u) sent.\n", window_base);
    }
}

int main(int argc, char *argv[]) {
  
  char *file_name = argv[1];
  int port = atoi(argv[2]);
  int window_size = atoi(argv[3]);

  if (window_size > MAX_WINDOW_SIZE) {
    fprintf(stderr, "Window size cannot exceed %d.\n", MAX_WINDOW_SIZE);
    exit(EXIT_FAILURE);
  }

  FILE *file = fopen(file_name, "w");
  if (!file) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  // Prepare server socket.
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  // Allow address reuse so we can rebind to the same port,
  // after restarting the server.
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) <
      0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in srv_addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
      .sin_port = htons(port),
  };

  set_timeout(sockfd, RECEIVER_TIMEOUT);

  if (bind(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr))) {
    perror("bind");
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "Receiving on port: %d\n", port);

  // initialize window
  int window[window_size];
  for (int i = 0; i < window_size; i++){
    window[i] = 0;
  }

  uint32_t window_base = 0;

  ssize_t len;
  
  int is_sender_known = 0;
  struct sockaddr_in sender_addr;
  int received_last_packet = 0;
  ack_pkt_t ack_pkt;
  
  while(1){

    struct sockaddr_in src_addr;
    data_pkt_t data_pkt;

    // Receive segment.
    len = recvfrom(sockfd, &data_pkt, sizeof(data_pkt), 0,
                 (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
    
    if(!is_sender_known){
      sender_addr = src_addr;
      is_sender_known = 1;
    }

    if(!(src_addr.sin_addr.s_addr == sender_addr.sin_addr.s_addr && src_addr.sin_port == sender_addr.sin_port)){
      printf("Received datagram from unexpected source. Ignoring.\n");
      continue;
    }

    if(len < MAX_CHUNK_SIZE){
      received_last_packet = 1;
    }

    // receiver terminates after 4 seconds of no packets
    if(len < 0){

      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Timeout occurred
        printf("Socket timed out after 4 seconds of waiting for packets.\n");

        // Check if the last packet indicates end-of-file
        if (received_last_packet) {
            printf("File transfer completed successfully!\n");
            fclose(file);
            close(sockfd);
            exit(EXIT_SUCCESS);
        } else {
            printf("File transfer incomplete. Deleting partial file...\n");
            fclose(file);
            remove(file_name);
            close(sockfd);
            exit(EXIT_FAILURE);
        }
      } else {
        perror("recvfrom error");
        fclose(file);
        close(sockfd);
        exit(EXIT_FAILURE);
      }
    }

    uint32_t seq_num = ntohl(data_pkt.seq_num);

  
    // Received the expected packet
    if (seq_num == window_base) {
        
        printf("DataPkt(%d) received in order. Size %ld bytes\n", seq_num,len);

        // Write chunk to file
        write_chunk_to_file(file, &data_pkt, seq_num, len);

        // Mark the packet as received
        window[0] = 1;
        window_base = slide_window(window, window_size, window_base);

        // Send ACK for this packet
        ack_pkt.selective_acks = 0;
        send_ack_packet(sockfd, &src_addr, window_base, window_size, ack_pkt);

    } else if(seq_num > window_base){
        // Received an out-of-order packet
        printf("DataPkt(%d) received out of order. Expected DataPkt(%d). Sending ACK(%d) with selective ack for %d.\n", seq_num, window_base, window_base, seq_num);
        
        if(seq_num >= window_base && seq_num <= window_base + window_size - 1){
          // Mark the packet as received
          window[seq_num - window_base] = 1;
          write_chunk_to_file(file, &data_pkt, seq_num, len);
        }

        if(window_size>1){
          printf("Adding SACK(%d) to ACK(%d).\n", seq_num, window_base);
          process_selective_ack(sockfd, &src_addr, window_base, seq_num, window_size, &ack_pkt);
        }

        // Send ACK for the last in-order packet
        send_ack_packet(sockfd, &src_addr, window_base, window_size, ack_pkt);

    } else {
        // Received a duplicate packet
        printf("DataPkt(%d) received again. Discarding.\n", seq_num);
    }
 
  }

  // Clean up and exit.
  close(sockfd);
  fclose(file);

  printf("Receiver terminating out of the loop...\n");
  return EXIT_SUCCESS;
}
