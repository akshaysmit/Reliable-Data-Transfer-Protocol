#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <chrono>
#include <pthread.h>
#include "rdt.h"

int file_not_found = 0;
int portno;
char *hostname;
char *filename;
int sockfd;
int filefd;
socklen_t servlen;
struct sockaddr_in serv_addr;
uint64_t my_seq_num;
uint64_t rcv_base;
uint64_t init_server_seqnum;
size_t buffered_sizes[4];
int spot_taken[4];
struct packet_hdr buffered_hdr[4];
char BUFFER[4][1024];
int synack_received = 0;
int first_packet_received = 0;
unsigned write_index = 0; //index of the packet that must be written to file next

pthread_mutex_t master_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t generate_seq() {
  srand(time(NULL));
  return rand() % SEQ_GEN_MAX;
}

int packet_in_window(uint64_t seq_num) { //check if packet is in receiver window
  if (rcv_base < 26624 && seq_num >= rcv_base && seq_num <= rcv_base + 4*MAX_PACKET_SIZE)
    return 1;
  else if (rcv_base >= 26624) {
    if (rcv_base >= seq_num || seq_num <= ((rcv_base + 4*MAX_PACKET_SIZE) % MAX_SEQ_NUM))
      return 1;
  }
  return 0;
}

int packet_below_window(uint64_t seq_num) { //check if ACK should be sent for this packet
  if (rcv_base >= 5120 && seq_num >= (rcv_base - 5*MAX_PACKET_SIZE) && seq_num < rcv_base)
    return 1;
  else if (rcv_base < 5120) {
    if (seq_num < rcv_base || seq_num >= (rcv_base - 5*MAX_PACKET_SIZE) + MAX_SEQ_NUM)
      return 1;
  }
  return 0;
}

void print_send(unsigned ack_num, int retrans, int syn, int fin) { //if ack_num == 0, it is not printed
  printf("Sending packet");
  if (ack_num != 0)
    printf(" %u", ack_num);
  if (retrans == 1)
    printf(" Retransmission");
  if (syn == 1)
    printf(" SYN");
  if (fin == 1)
    printf(" FIN");

  printf("\n");
}

void print_rcv(unsigned seq) {
  printf("Receiving packet %u\n", seq);
}

int send_hdr(struct packet_hdr *pkt) {
  char holder[32];
  bzero(holder, 32);
  memcpy(holder, &(pkt->seq_n), 8);
  memcpy(holder+8, &(pkt->ack_n), 8);
  memcpy(holder+16, &(pkt->rcv_window), 2);
  memcpy(holder+18, &(pkt->ack_flag), 1);
  memcpy(holder+19, &(pkt->syn_flag), 1);
  memcpy(holder+20, &(pkt->fin_flag), 1);
  int length = 21;
  char *ptr = holder;

  int i = sendto(sockfd, ptr, length, 0, (struct sockaddr *) &serv_addr, servlen);
  if (i < 0) {
    fprintf(stderr, "ERROR in send_hdr\n");
    return -1;
  }
  
  return 0;
}

void rcv_hdr(struct packet_hdr *pkt) {
  char holder[32];
  bzero(holder, 32);
  int n = recvfrom(sockfd, holder, 32, 0, (struct sockaddr *) &serv_addr, &servlen);
  if (n < 0) {
    fprintf(stderr, "Error receiving data\n");
    exit(1);
  }
  memcpy(&(pkt->seq_n), holder, 8);
  memcpy(&(pkt->ack_n), holder+8, 8);
  memcpy(&(pkt->rcv_window), holder+16, 2);
  memcpy(&(pkt->ack_flag), holder+18, 1);
  memcpy(&(pkt->syn_flag), holder+19, 1);
  memcpy(&(pkt->fin_flag), holder+20, 1);
}

int rcv_data(struct packet_hdr *pkt, char *holder) {;
  //holder should have MAX_PACKET_SIZE bytes
  char buf[MAX_PACKET_SIZE];
  bzero(holder, MAX_PACKET_SIZE);
  bzero(buf, MAX_PACKET_SIZE);
  int n = recvfrom(sockfd, buf, MAX_PACKET_SIZE, 0, (struct sockaddr *) &serv_addr, &servlen);
  if (n < 0)
    return -1;
  memcpy(&(pkt->seq_n), buf, 8);
  memcpy(&(pkt->ack_n), buf+8, 8);
  memcpy(&(pkt->rcv_window), buf+16, 2);
  memcpy(&(pkt->ack_flag), buf+18, 1);
  memcpy(&(pkt->syn_flag), buf+19, 1);
  memcpy(&(pkt->fin_flag), buf+20, 1);
  
  memcpy(holder, buf+21, n-21);
  return (n-21);
}

int get_open_spot() {
  for (int i = 0; i < 4; i++) {
    if (spot_taken[i] == 0)
      return i;
  }
  return -1;
}

int is_buffered(struct packet_hdr *hp) {
  for (int i = 0; i < 4; i++) {
    if (spot_taken[i] == 1 && hp->seq_n == buffered_hdr[i].seq_n)
      return 1;
  }
  return 0;
}

void move_window() {
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      if (spot_taken[j] == 1 && buffered_hdr[j].seq_n == rcv_base) {
	spot_taken[j] = 0;
	rcv_base = buffered_hdr[j].ack_n;
	pwrite(filefd, BUFFER[j], buffered_sizes[j], write_index * (MAX_PACKET_SIZE - sizeof(struct packet_hdr)));
	write_index++;
      }	
    }
  }
}

void fin(uint64_t final_seqnum) {
  
  //Already got FIN. Now send FINACK, wait till 2*TIMEOUT or until ACK received
  char holder[32];
  bzero(holder, 32);
  
  struct packet_hdr pkt;
  memset(&pkt, 0, sizeof(struct packet_hdr));
  pkt.ack_n = (final_seqnum + 1) % MAX_SEQ_NUM;
  pkt.fin_flag = 1;
  pkt.ack_flag = 1;
  std::chrono::time_point<std::chrono::steady_clock> timer;
  timer = std::chrono::steady_clock::now();
  send_hdr(&pkt);
  print_send(pkt.ack_n, 0, 0, 1);

  while(1) {
    std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
    double d = std::chrono::duration_cast<std::chrono::milliseconds>(end - timer).count();
    if (d >= 2 * TIMEOUT)
      exit(0);

    int n = recvfrom(sockfd, holder, 32, MSG_DONTWAIT, (struct sockaddr *) &serv_addr, &servlen);
    if (n < 0)
      continue;

    // Got something from server
    memcpy(&(pkt.seq_n), holder, 8);
    memcpy(&(pkt.ack_n), holder+8, 8);
    memcpy(&(pkt.rcv_window), holder+16, 2);
    memcpy(&(pkt.ack_flag), holder+18, 1);
    memcpy(&(pkt.syn_flag), holder+19, 1);
    memcpy(&(pkt.fin_flag), holder+20, 1);

    if (pkt.ack_flag == 1 && pkt.fin_flag == 0) //Got the last ACK
      exit(0);

    if (pkt.fin_flag == 1 && pkt.ack_flag == 0) { //Server didn't get FINACK
      memset(&pkt, 0, sizeof(struct packet_hdr));
      pkt.fin_flag = 1;
      pkt.ack_flag = 1;
      pkt.ack_n = (final_seqnum + 1) % MAX_SEQ_NUM;
      send_hdr(&pkt);
      print_send(pkt.ack_n, 1, 0, 1);
    }
  }
  
  exit(0);
}


void get_file() {
  while(1) {
    char data[MAX_PACKET_SIZE];
    struct packet_hdr pkt;
    
    int bytes;
    if ((bytes = rcv_data(&pkt, data)) == -1)
      exit(1);

    if (pkt.syn_flag == 1) //ignore handshake packets
      continue;
    
    if (pkt.fin_flag == 1) {     //Got a FIN
      print_rcv(pkt.seq_n);
      fin(pkt.seq_n);
    }

    if (bytes >= 27 && data[0] == 'H' && data[1] == 'T' && data[2] == 'T' && data[3] == 'P' &&
	data[4] == '/' && data[5] == '1' && data[6] == '.' && data[7] == '1') {
      //not found error
      print_rcv(pkt.seq_n);
      if (!file_not_found)
	printf("%s\n", data);
      pkt.ack_flag = 1;
      pkt.ack_n = pkt.seq_n + bytes + sizeof(struct packet_hdr);
      send_hdr(&pkt);
      print_send(pkt.ack_n, file_not_found, 0, 0);
      file_not_found = 1;
    }
    
    else if (pkt.seq_n == rcv_base) {
      print_rcv(pkt.seq_n);
      pkt.ack_flag = 1;
      pkt.fin_flag = 0;
      pkt.ack_n = (pkt.seq_n + bytes + sizeof(struct packet_hdr)) % MAX_SEQ_NUM;
      send_hdr(&pkt);
      print_send(pkt.ack_n, 0, 0, 0);
      rcv_base = pkt.ack_n;

      pwrite(filefd, data, bytes, write_index * (MAX_PACKET_SIZE - sizeof(struct packet_hdr)));
      write_index++;
      move_window();
    }

    else if (packet_below_window(pkt.seq_n)) { //must send ACK to move sender window
      print_rcv(pkt.seq_n);
      pkt.ack_flag = 1;
      pkt.fin_flag = 0;
      pkt.ack_n = (pkt.seq_n + bytes + sizeof(struct packet_hdr)) % MAX_SEQ_NUM;
      send_hdr(&pkt);
      print_send(pkt.ack_n, 1, 0, 0);
    }

    else {  //send ACK and buffer data
      print_rcv(pkt.seq_n);
      pkt.ack_flag = 1;
      pkt.fin_flag = 0;
      pkt.ack_n = (pkt.seq_n + bytes + sizeof(struct packet_hdr)) % MAX_SEQ_NUM;
      send_hdr(&pkt);

      if (is_buffered(&pkt))
	print_send(pkt.ack_n, 1, 0, 0);
      else {
	print_send(pkt.ack_n, 0, 0, 0);
	int ind = get_open_spot();
	memcpy(BUFFER[ind], data, bytes);
	spot_taken[ind] = 1;
	buffered_sizes[ind] = bytes;
	buffered_hdr[ind] = pkt;
      }
    }
  }
}

void *SYNACK_read(void *arg) { //this thread handles reading of SYNACK
  while(1) {
    struct packet_hdr pkt;
    memset(&pkt, 0, sizeof(struct packet_hdr));
    rcv_hdr(&pkt);
    
    if (pkt.syn_flag != 1 || pkt.ack_flag != 1)
      continue;
    if (pkt.ack_n != my_seq_num + 1)
      continue;

    pthread_mutex_lock(&master_lock);
    rcv_base = pkt.seq_n;
    print_rcv(pkt.seq_n);
    rcv_base++;
    synack_received = 1;
    init_server_seqnum = rcv_base;
    pthread_mutex_unlock(&master_lock);
    return NULL;
  }
}

void *first_packet(void *arg) { //this thread handles the first data packet received
  filefd = open("received.data", O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
  
  while(1) {
    int bytes;
    char data[MAX_PACKET_SIZE];
    struct packet_hdr pkt;
    
    if ((bytes = rcv_data(&pkt, data)) == -1)
      return NULL;
    
    if (pkt.syn_flag == 1 || pkt.ack_flag == 1)
      continue;
    if (pkt.seq_n >= rcv_base + BASE_RCV_WINDOW)
      continue;
    if (pkt.seq_n < rcv_base)
      continue;

    pthread_mutex_lock(&master_lock);
    print_rcv(pkt.seq_n);

    if (bytes >= 27 && data[0] == 'H' && data[1] == 'T' && data[2] == 'T' && data[3] == 'P' && 
	data[4] == '/' && data[5] == '1' && data[6] == '.' && data[7] == '1') {
      //not found error
      printf("%s\n", data);
      pkt.ack_flag = 1;
      pkt.ack_n = pkt.seq_n + bytes + sizeof(struct packet_hdr);
      send_hdr(&pkt);
      print_send(pkt.ack_n, 0, 0, 0);
      file_not_found = 1;
    }
    
    else if (pkt.seq_n > rcv_base) { //send ACK and buffer data
      pkt.ack_flag = 1;
      pkt.ack_n = pkt.seq_n + bytes + sizeof(struct packet_hdr);
      send_hdr(&pkt);
      print_send(pkt.ack_n, 0, 0, 0);
      
      memcpy(BUFFER[0], data, bytes);
      spot_taken[0] = 1;
      buffered_sizes[0] = bytes;
      buffered_hdr[0] = pkt;
    }

    else {  //write to file
      pkt.ack_flag = 1;
      pkt.ack_n = pkt.seq_n + bytes + sizeof(struct packet_hdr);
      send_hdr(&pkt);
      print_send(pkt.ack_n, 0, 0, 0);
      rcv_base = pkt.ack_n;
      pwrite(filefd, data, bytes, 0);
      write_index++;
    }

    first_packet_received = 1;
    pthread_mutex_unlock(&master_lock);
    return NULL;
  }
}

void handshake() {       //Requires sockfd, servlen, and serv_addr to be initialized

  /* Send SYN and read SYNACK */
  struct packet_hdr pkt;
  pthread_t t;
  pthread_create(&t, NULL, SYNACK_read, NULL);
  pthread_detach(t);

  std::chrono::time_point<std::chrono::steady_clock> timer;
  timer = std::chrono::steady_clock::now();
  
  int retrans = 0;
  while(1) {

    pthread_mutex_lock(&master_lock);
    if (synack_received == 1) {
      pthread_mutex_unlock(&master_lock);
      break;
    }

    std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
    double d = std::chrono::duration_cast<std::chrono::milliseconds>(end - timer).count();
    if (retrans && d < TIMEOUT) {
      pthread_mutex_unlock(&master_lock);
      continue;
    }
    
    timer = std::chrono::steady_clock::now(); //timer reset
    
    memset(&pkt, 0, sizeof(struct packet_hdr));
    pkt.rcv_window = BASE_RCV_WINDOW;
    pkt.syn_flag = 1;
    pkt.seq_n = my_seq_num;

    if(send_hdr(&pkt) == -1)
      exit(1);
    print_send(0, retrans, 1, 0);
    pthread_mutex_unlock(&master_lock);

    retrans = 1;
  }

  /* Send ACK + filename */
  pthread_create(&t, NULL, first_packet, NULL);
  pthread_detach(t);
  timer = std::chrono::steady_clock::now();
  retrans = 0;

  while(1) {
    pthread_mutex_lock(&master_lock);
    if (first_packet_received == 1) {
      pthread_mutex_unlock(&master_lock);
      break;
    }

    std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
    double d = std::chrono::duration_cast<std::chrono::milliseconds>(end - timer).count();
    if (retrans && d < TIMEOUT) {
      pthread_mutex_unlock(&master_lock);
      continue;
    }

    timer = std::chrono::steady_clock::now(); //timer reset

    memset(&pkt, 0, sizeof(struct packet_hdr));
    pkt.ack_flag = 1;
    pkt.ack_n = rcv_base;
    pkt.rcv_window = BASE_RCV_WINDOW;

    char holder[MAX_PACKET_SIZE];
    bzero(holder, MAX_PACKET_SIZE);
    memcpy(holder, &(pkt.seq_n), 8);
    memcpy(holder+8, &(pkt.ack_n), 8);
    memcpy(holder+16, &(pkt.rcv_window), 2);
    memcpy(holder+18, &(pkt.ack_flag), 1);
    memcpy(holder+19, &(pkt.syn_flag), 1);
    memcpy(holder+20, &(pkt.fin_flag), 1);

    memcpy(holder+21, filename, strlen(filename));
    sendto(sockfd, holder, strlen(filename) + 21, 0, (struct sockaddr *) &serv_addr, servlen);
    print_send(rcv_base, retrans, 0, 0);

    pthread_mutex_unlock(&master_lock);
    retrans = 1;
  }
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "Usage: $./client <server_hostname> <server_portnumber> <filename>\n");
    exit(1);
  }

  hostname = argv[1];
  portno = atoi(argv[2]);
  filename = argv[3];

  if (portno == 0) {
    fprintf(stderr, "Please enter valid port number\n");
    exit(1);
  }

  servlen = sizeof(struct sockaddr_in);
  struct hostent *server;
  
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);     //UDP connection
  if (sockfd < 0) {
    perror("ERROR opening socket");
    exit(1);
  }
  
  server = gethostbyname(hostname);
  if (server == NULL) {
    fprintf(stderr, "ERROR, no such host\n");
    exit(1);
  }

  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy((char *)server->h_addr,
	(char *)&serv_addr.sin_addr.s_addr,
	server->h_length);
  serv_addr.sin_port = htons(portno);

  my_seq_num = generate_seq();
  
  handshake();         //Perform 3-way handshake
  get_file();
}
