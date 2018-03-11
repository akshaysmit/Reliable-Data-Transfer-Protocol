#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <pthread.h>
#include <chrono>
#include "rdt.h"

int not_found_error = 0;
const int ACKED = 2;
const int NOT_ACKED = 3;
int portno, sockfd;
socklen_t clilen;
struct sockaddr_in serv_addr, cli_addr;
unsigned min_index = 0;
unsigned max_index = 4;
uint64_t send_base;
uint64_t original_seq_num;
size_t filesize;
uint16_t rwnd;
int num_packets;
int *ack_status;
int *timer_active;
struct packet_hdr *headers;
std::chrono::time_point<std::chrono::steady_clock> *timers;
int file_fd;
int handshake_done = 0;
int finack_received = 0;
char *filename;

pthread_mutex_t master_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t generate_seq() {
  srand(time(NULL));
  return rand() % SEQ_GEN_MAX;
}

uint64_t expected_ack(int ind) {  //return the expected ack for given packet
  if (ind < num_packets - 1)
    return (1 + original_seq_num + (ind+1) * MAX_PACKET_SIZE) % MAX_SEQ_NUM;
  else
    return (1 + original_seq_num + filesize + num_packets * sizeof(struct packet_hdr))
      % MAX_SEQ_NUM;
}

void print_send(unsigned seq_num, unsigned wnd, int retrans, int syn, int fin) {
  printf("Sending packet ");
  printf("%u %u", seq_num, wnd);
  if (retrans == 1)
    printf(" Retransmission");
  if (syn == 1)
    printf(" SYN");
  if (fin == 1)
    printf(" FIN");
  
  printf("\n");
  fflush(stdout);
}

void print_rcv(unsigned ack) {
  printf("Receiving packet %u\n", ack);
  fflush(stdout);
}

int send_data(int index) { //send corresponding header + payload
  char holder[2 * MAX_PACKET_SIZE];
  char *ptr = holder;
  bzero(holder, 2 * MAX_PACKET_SIZE);
  memcpy(holder, &(headers[index].seq_n), 8);
  memcpy(holder+8, &(headers[index].ack_n), 8);
  memcpy(holder+16, &(headers[index].rcv_window), 2);
  memcpy(holder+18, &(headers[index].ack_flag), 1);
  memcpy(holder+19, &(headers[index].syn_flag), 1);
  memcpy(holder+20, &(headers[index].fin_flag), 1);

  if (not_found_error) {
    strcpy(holder+21, "HTTP/1.1 404 Not Found \r\n\r\n");
    int i = sendto(sockfd, ptr, 21+27, 0, (struct sockaddr *) &cli_addr, clilen);
    if (i < 0) {
      fprintf(stderr, "ERROR in send_data\n");
      return -1;
    }
    else
      return 0;
  }
  
  size_t rdsize;
  off_t offset;
  if (index == num_packets - 1) {
    rdsize = filesize - (num_packets - 1) * (MAX_PACKET_SIZE - sizeof(struct packet_hdr));
    offset = index * (MAX_PACKET_SIZE - sizeof(struct packet_hdr));
  }
  else {
    rdsize = MAX_PACKET_SIZE - sizeof(struct packet_hdr);
    offset = index * rdsize;
  }

  pread(file_fd, holder+21, rdsize, offset);
  size_t length = rdsize + 21;
  
  int i = sendto(sockfd, ptr, length, 0, (struct sockaddr *) &cli_addr, clilen);
  if (i < 0) {
    fprintf(stderr, "ERROR in send_data\n");
    return -1;
  }

  return 0;
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

  int i = sendto(sockfd, ptr, length, 0, (struct sockaddr *) &cli_addr, clilen);
  if (i < 0) {
    fprintf(stderr, "ERROR in send_hdr\n");
    return -1;
  }

  return 0;
}

void rcv_hdr(struct packet_hdr *pkt) {
  char holder[32];
  bzero(holder, 32);
  int n = recvfrom(sockfd, holder, 32, 0, (struct sockaddr *) &cli_addr, &clilen);
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

void *ACKreader(void *arg) {  //this thread handles reception of ACK's
  while (1) {
    struct packet_hdr pkt;
    memset(&pkt, 0, sizeof(struct packet_hdr));
    rcv_hdr(&pkt);
    if (pkt.ack_flag != 1)  //ACK flag not set
      continue;
    pthread_mutex_lock(&master_lock);
    int j;
    for (j = min_index; j < max_index; j++) {
      if (expected_ack(j) == pkt.ack_n)
	break;
    }

    if (j >= max_index) {  //No corresponding packet
      pthread_mutex_unlock(&master_lock);
      continue;
    }

    if (j >= min_index)
      print_rcv(pkt.ack_n);
    
    ack_status[j] = ACKED;   //set to ACK'ed
    if (j == min_index) {    //move window
      while (j < num_packets && ack_status[j] == ACKED) {
	min_index++;
	max_index++;
	j++;
      }
    }

    if (min_index == num_packets) {
      pthread_mutex_unlock(&master_lock);
      return NULL;
    }
    pthread_mutex_unlock(&master_lock);
  }
}

void *handshake_ACK(void *arg) { //this thread reads client ACK during handshake
  while(1) {
    struct packet_hdr pkt;
    memset(&pkt, 0, sizeof(struct packet_hdr));
    char holder[MAX_PACKET_SIZE];
    bzero(holder, MAX_PACKET_SIZE);

    int n = recvfrom(sockfd, holder, MAX_PACKET_SIZE, 0, (struct sockaddr *) &cli_addr, &clilen);
    if (n < 0)
      return NULL;
    memcpy(&(pkt.seq_n), holder, 8);
    memcpy(&(pkt.ack_n), holder+8, 8);
    memcpy(&(pkt.rcv_window), holder+16, 2);
    memcpy(&(pkt.ack_flag), holder+18, 1);
    memcpy(&(pkt.syn_flag), holder+19, 1);
    memcpy(&(pkt.fin_flag), holder+20, 1);

    if (pkt.ack_flag == 0 || pkt.ack_n != send_base)
      continue;

    pthread_mutex_lock(&master_lock);
    print_rcv(pkt.ack_n);
    filename = (char *) malloc(sizeof(char) * n);
    bzero(filename, n);
    memcpy(filename, holder+21, n-21);
    handshake_done = 1;
    pthread_mutex_unlock(&master_lock);
    return NULL;
  }
}

void handshake(int fd, struct sockaddr_in *addr, socklen_t *addr_len) {
  struct packet_hdr pkt;
  memset(&pkt, 0, sizeof(struct packet_hdr));
  
  /* Read SYN */
  rcv_hdr(&pkt);
  if (pkt.syn_flag == 0)
    exit(0);
  int ret_ack = pkt.seq_n + 1;
  rwnd = pkt.rcv_window;
  
  /* Send SYNACK amd read ACK */
  send_base++;
  pthread_t t;
  pthread_create(&t, NULL, handshake_ACK, NULL);
  pthread_detach(t);

  std::chrono::time_point<std::chrono::steady_clock> timer;
  timer = std::chrono::steady_clock::now();
  int retrans = 0;
  
  while(1) {
    
    pthread_mutex_lock(&master_lock);
    if (handshake_done == 1) {
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
    pkt.syn_flag = 1;
    pkt.seq_n = original_seq_num;
    pkt.ack_flag = 1;
    pkt.ack_n = ret_ack;

    if(send_hdr(&pkt) == -1)
      exit(1);
    print_send(pkt.seq_n, rwnd, retrans, 1, 0);
    pthread_mutex_unlock(&master_lock);

    retrans = 1;
  }
}

void create_headers() {
  num_packets = ceil(filesize / (double) (MAX_PACKET_SIZE - sizeof(struct packet_hdr)));
  
  ack_status = (int *) malloc(sizeof(int) * num_packets);
  timer_active = (int *) malloc(sizeof(int) * num_packets);
  headers = (struct packet_hdr *) malloc(sizeof(struct packet_hdr) * num_packets);
  timers = new std::chrono::time_point<std::chrono::steady_clock>[num_packets];
  for (int i = 0; i < num_packets; i++) {
    timer_active[i] = 0;
    ack_status[i] = NOT_ACKED;
  }
  
  for (int i = 0; i < num_packets; i++) { //Fill in each header
    headers[i].seq_n = (send_base + i * MAX_PACKET_SIZE) % MAX_SEQ_NUM;
    headers[i].ack_flag = 0;
    headers[i].syn_flag = 0;
    headers[i].fin_flag = 0;
  }
}

void fin(); //just to satisfy the compiler

void send_file() {
  file_fd = open(filename, O_RDONLY);
  struct packet_hdr pkt;
  memset(&pkt, 0, sizeof(struct packet_hdr));
  
  if (file_fd == -1) {
    not_found_error = 1;
    filesize = 27;
  }
  else {
    struct stat sb;
    stat(filename, &sb);
    filesize = sb.st_size;
  }
  create_headers();
  
  pthread_t ack_thread;
  pthread_create(&ack_thread, NULL, ACKreader, NULL); //start ACK reader thread
  pthread_detach(ack_thread);
  
  int index = 0;
  while (1) {
    pthread_mutex_lock(&master_lock);
    int min = min_index;
    int max = max_index;

    if (min == num_packets) {
      pthread_mutex_unlock(&master_lock);
      break;
    }
    
    for (int i = min; i <= max && i < num_packets; i++) {
      if (ack_status[i] == ACKED)
	continue;

      if (timer_active[i] == 0) {     //new transmission
	timers[i] = std::chrono::steady_clock::now();
	if (send_data(i) == -1)
	  exit(1);
	print_send(headers[i].seq_n, rwnd, 0, 0, 0);
	timer_active[i] = 1;
      }
      
      else {
	std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
	double t = std::chrono::duration_cast<std::chrono::milliseconds>(end - timers[i]).count();
	if (t < TIMEOUT) 
	  continue;
	timers[i] = std::chrono::steady_clock::now(); //reset clock
	if (send_data(i) == -1)
	  exit(1);
	print_send(headers[i].seq_n, rwnd, 1, 0, 0);
      }
    }
    
    pthread_mutex_unlock(&master_lock);
    //optional usleep
  }

  delete [] timers;
  free(timer_active);
  free(headers);
  free(ack_status);
}

void *finack_reader(void *arg) { // this thread reads the finack
  while(1) {
    struct packet_hdr pkt;
    memset(&pkt, 0, sizeof(struct packet_hdr));
    char holder[MAX_PACKET_SIZE];
    bzero(holder, MAX_PACKET_SIZE);
    
    int n = recvfrom(sockfd, holder, MAX_PACKET_SIZE, 0, (struct sockaddr *) &cli_addr, &clilen);
    if (n < 0) 
      return NULL;
    
    memcpy(&(pkt.seq_n), holder, 8);
    memcpy(&(pkt.ack_n), holder+8, 8);
    memcpy(&(pkt.rcv_window), holder+16, 2);
    memcpy(&(pkt.ack_flag), holder+18, 1);
    memcpy(&(pkt.syn_flag), holder+19, 1);
    memcpy(&(pkt.fin_flag), holder+20, 1);

    if (pkt.fin_flag != 1 || pkt.ack_flag != 1)
      continue;

    pthread_mutex_lock(&master_lock);
    print_rcv(pkt.ack_n);
    finack_received = 1;
    pthread_mutex_unlock(&master_lock);
    return NULL;
  }
}

void fin() {
  struct packet_hdr pkt;
  pthread_t t;
  pthread_create(&t, NULL, finack_reader, NULL);
  pthread_detach(t);

  std::chrono::time_point<std::chrono::steady_clock> timer;
  timer = std::chrono::steady_clock::now();
  int retrans = 0;
  int count = 0;
  char holder[1024];
  bzero(holder, 1024);
  
  // send FIN and wait for FINACK, max 9 retransmissions
  while(1) {
    pthread_mutex_lock(&master_lock);
    if (finack_received == 1) {
      pthread_mutex_unlock(&master_lock);
      break;
    }

    if (count == 10)
      exit(0);
    
    std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
    double d = std::chrono::duration_cast<std::chrono::milliseconds>(end - timer).count();
    if (retrans && d < TIMEOUT) {
      pthread_mutex_unlock(&master_lock);
      continue;
    }

    timer = std::chrono::steady_clock::now(); //timer reset
    memset(&pkt, 0, sizeof(struct packet_hdr));
    pkt.fin_flag = 1;
    pkt.seq_n = expected_ack(num_packets - 1);

    if(send_hdr(&pkt) == -1)
      exit(1);
    count++;
    print_send(pkt.seq_n, rwnd, retrans, 0, 1);
    pthread_mutex_unlock(&master_lock);

    retrans = 1;
  }
  
  // wait 2*TIMEOUT ms to ensure ACK reaches client
  bzero(holder, 1024);
  
  timer = std::chrono::steady_clock::now();
  memset(&pkt, 0, sizeof(struct packet_hdr));
  pkt.ack_flag = 1;
  if(send_hdr(&pkt) == -1)
    exit(1);

  while(1) {
    std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
    double d = std::chrono::duration_cast<std::chrono::milliseconds>(end - timer).count();
    if (d >= 2 * TIMEOUT)
      exit(0);
    
    int n = recvfrom(sockfd, holder, 32, MSG_DONTWAIT, (struct sockaddr *) &cli_addr, &clilen);
    if (n < 0)
      continue;

    // Got something from client
    memcpy(&(pkt.seq_n), holder, 8);
    memcpy(&(pkt.ack_n), holder+8, 8);
    memcpy(&(pkt.rcv_window), holder+16, 2);
    memcpy(&(pkt.ack_flag), holder+18, 1);
    memcpy(&(pkt.syn_flag), holder+19, 1);
    memcpy(&(pkt.fin_flag), holder+20, 1);

    if (pkt.fin_flag != 1 || pkt.ack_flag != 1) //not a FINACK
      continue;

    memset(&pkt, 0, sizeof(struct packet_hdr));
    pkt.ack_flag = 1;
    pkt.seq_n = expected_ack(num_packets - 1);
    if(send_hdr(&pkt) == -1)
      exit(1);
  }
}

int main(int argc, char **argv) {
  clilen = sizeof(cli_addr);

  if (argc != 2) {
    fprintf(stderr,"Please provide port number\n");
    exit(1);
  }
  else
    portno = atoi(argv[1]);

  if (portno == 0) {
    fprintf(stderr, "Please enter valid port number\n");
    exit(1);
  }

  sockfd = socket(AF_INET, SOCK_DGRAM, 0); //UDP connection
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;          //Code of address family
  serv_addr.sin_addr.s_addr = INADDR_ANY;  //IP address of this machine

  serv_addr.sin_port = htons(portno);      //Port number in network byte order

  if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    perror("Error on binding: ");
    exit(1);
  }

  send_base = generate_seq();
  original_seq_num = send_base;
  
  handshake(sockfd, &cli_addr, &clilen);
  send_file();
  free(filename);
  fin();
}
