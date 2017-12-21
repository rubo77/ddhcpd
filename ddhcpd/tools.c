#include "tools.h"
#include "logger.h"

#include <string.h>
#include <getopt.h>
#include <stdio.h>
#include <netinet/in.h>
#include <stdlib.h>


void addr_add(struct in_addr* subnet, struct in_addr* result, int add) {
  struct in_addr addr;
  memcpy(&addr, subnet, sizeof(struct in_addr));
  addr.s_addr = ntohl(addr.s_addr);
  addr.s_addr += add;
  addr.s_addr = htonl(addr.s_addr);
  memcpy(result, &addr, sizeof(struct in_addr));
}

dhcp_option* parse_option() {

  //size_t optlen = strlen(optarg);

  char* len_s = strchr(optarg, ';');

  if (len_s == NULL) {
    ERROR("Malformed dhcp option '%s'\n", optarg);
    exit(1);
  }

  len_s++[0] = '\0';

  char* payload_s = strchr(len_s, ';');

  if (payload_s == NULL) {
    ERROR("Malformed dhcp option '%s'\n", optarg);
    exit(1);
  }

  payload_s++[0] = '\0';

  uint8_t len = atoi(len_s);
  uint8_t code = atoi(optarg);

  dhcp_option* option = (dhcp_option*) malloc(sizeof(dhcp_option));
  option->code = code;
  option->len = len;
  option->payload = (uint8_t*)  malloc(sizeof(uint8_t) * len);

  for (int i = 0 ; i < len; i++) {
    char* next_payload_s = strchr(payload_s, ',');

    if (next_payload_s == NULL && i < len - 1) {
      ERROR("Malformed dhcp option '%s' to few payload\n", optarg);
      exit(1);
    }

    if (i < len - 1) {
      next_payload_s++[0] = '\0';
    }

    uint8_t payload = atoi(payload_s);
    option->payload[i] = payload;
    payload_s = next_payload_s;
  }

  return option;
}

char* hwaddr2c(uint8_t* hwaddr) {
  char* str = calloc(33,sizeof(char));
  char* istr = str;
  for(int i = 0; i < 16; i++){
    sprintf(istr,"%02X",(char) hwaddr[i]);
    istr = istr + 2;
  }
  str[32] = '\0';
  return str;
}
