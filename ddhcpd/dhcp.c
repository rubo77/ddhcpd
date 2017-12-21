#include <string.h>

#include "block.h"
#include "dhcp.h"
#include "dhcp_options.h"
#include "logger.h"
#include "packet.h"
#include "tools.h"

// Free an offered lease after 12 seconds.
uint16_t DHCP_OFFER_TIMEOUT = 12;
uint16_t DHCP_LEASE_TIME    = 3600;
uint16_t DHCP_LEASE_SERVER_DELTA = 100;

#if LOG_LEVEL >= LOG_DEBUG
#define DEBUG_DHCP_LEASE(...) do { \
  DEBUG("DHCP LEASE [ state %i, xid %u, end %i ]\n",lease->state,lease->xid,lease->lease_end);\
} while (0);
#else
#define DEBUG_LEASE(...)
#endif

/**
 * Search for block and lease for given address, returns a status code and found
 * results.
 * A status code of 0 is returned, iff the result is in one of our blocks.
 * Of 1, iff result is non in our blocks.
 * And 2 on failure.
 */
uint8_t find_lease_from_address(struct in_addr* addr, ddhcp_block* blocks, ddhcp_config* config, ddhcp_block** lease_block, uint32_t* lease_index) {
#if LOG_LEVEL >= LOG_DEBUG
  DEBUG("find_lease_from_address( %s, ...)\n", inet_ntoa(*addr));
#endif
  uint32_t address = (uint32_t) addr->s_addr;

  uint32_t block_number = (ntohl(address) - ntohl((uint32_t) config->prefix.s_addr)) / config->block_size;
  uint32_t lease_number = (ntohl(address) - ntohl((uint32_t) config->prefix.s_addr)) % config->block_size;

  if (block_number < config->number_of_blocks) {
    DEBUG("find_lease_from_address(...) -> found block %i and lease %i with state %i \n", block_number, lease_number, blocks[block_number].state);

    if (lease_block) {
      *lease_block = blocks + block_number;
    }

    if (lease_index) {
      *lease_index = lease_number;
    }

    DEBUG("find_lease_from_address( ... ): state: %i\n", DDHCP_OURS);

    if (blocks[block_number].state == DDHCP_OURS) {
      return 0;
    } else {
      // TODO Try to aquire address for client
      return 1;
    }
  }

  DEBUG("find_lease_from_address(...) -> block index %i outside of configured of network structure\n", block_number);

  return 2;
}

void _dhcp_release_lease(ddhcp_block* block , uint32_t lease_index) {
  INFO("Releasing Lease %i in block %i\n", lease_index, block->index);
  dhcp_lease* lease = block->addresses + lease_index;

  // TODO Should we really reset the chaddr or xid, RFC says we
  // ''SHOULD retain a record of the client's initialization parameters for possible reuse''
  memset(lease->chaddr, 0, 16);

  lease->xid   = 0;
  lease->state = FREE;
}

dhcp_packet* build_initial_packet(dhcp_packet* from_client) {
  DEBUG("build_initial_packet( from_client, packet )\n");

  dhcp_packet* packet = (dhcp_packet*) calloc(sizeof(dhcp_packet), 1);

  if (packet == NULL) {
    DEBUG("build_initial_packet(...) -> memory allocation failure\n");
    return NULL;
  }

  packet->op    = 2;
  packet->htype = from_client->htype;
  packet->hlen  = from_client->hlen;
  packet->hops  = from_client->hops;
  packet->xid   = from_client->xid;
  packet->secs  = 0;
  packet->flags = from_client->flags;
  memcpy(&packet->ciaddr, &from_client->ciaddr, 4);
  // yiaddr
  // siaddr
  memcpy(&packet->giaddr, &from_client->giaddr, 4);
  memcpy(&packet->chaddr, &from_client->chaddr, 16);
  // sname
  // file
  // options

  return packet;
}

int dhcp_hdl_discover(int socket, dhcp_packet* discover, ddhcp_block* blocks, ddhcp_config* config) {
  DEBUG("dhcp_discover( %i, packet, blocks, config)\n", socket);

  time_t now = time(NULL);
  ddhcp_block* block = blocks;
  dhcp_lease* lease = NULL;
  ddhcp_block* lease_block = NULL;

  int lease_index = 0;
  int lease_ratio = config->block_size + 1;


  // TODO Select Block according to usage, current behavior leads to fragmentation
  //      of block usage, if more that one block is claimed.
  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_OURS) {
      int free_leases = dhcp_num_free(block);

      if (free_leases > 0) {
        DEBUG("dhcp_discover(...) -> block %i has %i free leases\n", block->index, free_leases);

        if (free_leases < lease_ratio) {
          DEBUG("dhcp_discover(...) -> block %i has best lease ratio until now\n", block->index);

          uint32_t index = dhcp_get_free_lease(block);

          lease_block = block;
          lease_index = index;
          lease_ratio = free_leases;

          lease = block->addresses + index;
        }
      }
    }

    block++;
  }

  if (! lease) {
    DEBUG("dhcp_discover(...) -> no free leases found");
    return 2;
  }

  dhcp_packet* packet = build_initial_packet(discover);

  if (! packet) {
    DEBUG("dhcp_discover(...) -> memory allocation failure");
    return 1;
  }

  // Mark lease as offered and register client
  memcpy(&lease->chaddr, &discover->chaddr, 16);
  lease->xid = discover->xid;
  lease->state = OFFERED;
  lease->lease_end = now + DHCP_OFFER_TIMEOUT;

  addr_add(&lease_block->subnet, &packet->yiaddr, lease_index);

  DEBUG("dhcp_discover(...) offering address %i %s\n", lease_index, inet_ntoa(lease_block->subnet));

  // TODO We need a more extendable way to build up options
  packet->options_len = fill_options(discover->options, discover->options_len, &config->options, 2, &packet->options) ;

  // TODO Error handling
  set_option(packet->options, packet->options_len, DHCP_CODE_MESSAGE_TYPE, 1, (uint8_t[]) {
    DHCPOFFER
  });
  set_option(packet->options, packet->options_len, DHCP_CODE_ADDRESS_LEASE_TIME, 1, (uint8_t[]) {
    DHCP_LEASE_TIME
  });

  dhcp_packet_send(socket, packet);

  free(packet->options);
  free(packet);

  return 0;
}

int dhcp_rhdl_request(uint32_t* address, ddhcp_block* blocks, ddhcp_config* config) {
  DEBUG("dhcp_rhdl_request(address, blocks, config)\n");

  time_t now = time(NULL);
  ddhcp_block* lease_block = NULL;
  uint32_t lease_index = 0;
  struct in_addr requested_address;

  memcpy(&requested_address, address, sizeof(struct in_addr));

  uint8_t found = find_lease_from_address(&requested_address, blocks, config, &lease_block, &lease_index);

  if (found == 0) {
    // Update lease information
    // TODO Check for validity of request (chaddr)
    dhcp_lease* lease = lease_block->addresses + lease_index;
    lease->lease_end = now + DHCP_LEASE_TIME + DHCP_LEASE_SERVER_DELTA;
    // Report ack
    return 0;
  } else if (found == 1) {
    // We got a request for a block we don't own (anymore?)
    // Reply with a nack
    return 1;
  } else {
    return 2;
  }
}

int dhcp_rhdl_ack(int socket, struct dhcp_packet* request, ddhcp_block* blocks, ddhcp_config* config) {

  ddhcp_block* lease_block = NULL;
  uint32_t lease_index = 0;
  struct in_addr requested_address;

  uint8_t* address = find_option_requested_address(request->options, request->options_len);

  if (address) {
    memcpy(&requested_address, address, sizeof(struct in_addr));
  } else if (request->ciaddr.s_addr != INADDR_ANY) {
    memcpy(&requested_address, &request->ciaddr.s_addr, sizeof(struct in_addr));
  }

  if (find_lease_from_address(&requested_address, blocks, config, &lease_block, &lease_index) != 1) {
    DEBUG("dhcp_rhdl_ack( ... ) -> lease not found\n");
    return 1;
  }

  return dhcp_ack(socket, request, lease_block, lease_index, config);
}

int dhcp_hdl_request(int socket, struct dhcp_packet* request, ddhcp_block* blocks, ddhcp_config* config) {
  DEBUG("dhcp_hdl_request( %i, dhcp_packet, blocks, config)\n", socket);

  // search the lease we may have offered

  time_t now = time(NULL);
  dhcp_lease* lease = NULL ;
  ddhcp_block* lease_block = NULL;
  uint32_t lease_index = 0;

  uint8_t* address = find_option_requested_address(request->options, request->options_len);

  struct in_addr requested_address;
  uint8_t found_address = 0;

  if (address) {
    memcpy(&requested_address, address, sizeof(struct in_addr));
    found_address = 1;
  } else if (request->ciaddr.s_addr != INADDR_ANY) {
    memcpy(&requested_address, &request->ciaddr.s_addr, sizeof(struct in_addr));
    found_address = 1;
  }

  if (found_address) {
    // Calculate block and dhcp_lease from address
    uint8_t found = find_lease_from_address(&requested_address, blocks, config, &lease_block, &lease_index);

    if (found != 2) {
      lease = lease_block->addresses + lease_index;
      DEBUG("dhcp_hdl_request(...): Lease found.\n");

      if (lease_block->state == DDHCP_CLAIMED) {
        if (lease_block->addresses == NULL) {
          if (block_alloc(lease_block)) {
            ERROR("dhcp_hdl_request(...): can't allocate requested block");
            dhcp_nack(socket, request);
          }
        }

        lease = lease_block->addresses + lease_index;
        // This lease block is not ours so we have to forward the request
        DEBUG("dhcp_hdl_request(...): Requested lease is owned by another node. Send Request.\n");
        // Register client information in lease
        // TODO This isn't a good idea, because of multi request on the same address from various clients, register it elsewhere and append xid.
        lease->xid = request->xid;
        lease->state = OFFERED;
        lease->lease_end = now + DHCP_LEASE_TIME + DHCP_LEASE_SERVER_DELTA;
        memcpy(&lease->chaddr, &request->chaddr, 16);

        // Build packet and send it
        ddhcp_renew_payload payload;
        memcpy(&payload.chaddr, &request->chaddr, 16);
        memcpy(&payload.address, &requested_address, sizeof(struct in_addr));
        payload.xid = request->xid;
        payload.lease_seconds = 0;
        #if LOG_LEVEL >= LOG_DEBUG
        char* hwaddr = hwaddr2c(payload.chaddr);
        DEBUG("dhcp_hdl_request( ... ): Save request for xid: %u chaddr: %s\n",payload.xid,hwaddr);
        free(hwaddr);
        #endif

        // Send packet
        ddhcp_mcast_packet* packet = new_ddhcp_packet(DDHCP_MSG_RENEWLEASE, config);
        packet->renew_payload = &payload;

        // Store packet for later usage.
        // TODO Error handling
        dhcp_packet_list_add(&config->dhcp_packet_cache,request);

        send_packet_direct(packet, &lease_block->owner_address, config->server_socket, config->mcast_scope_id);
        free(packet);
        return 2;

      } else if (lease_block->state == DDHCP_OURS) {
        if (lease->state != OFFERED || lease->xid != request->xid) {
          if (memcmp(request->chaddr, lease->chaddr, 16) != 0) {
            // Check if lease is free
            if (lease->state != FREE) {
              DEBUG("dhcp_request(...): Requested lease offered to other client\n");
              // Send DHCP_NACK
              dhcp_nack(socket, request);
              return 2;
            }
          }
        }
      } else {
        // Block is neither blocked nor ours, so probably say nak here
        // TODO but first we should check if we are still in warmup.
        return 2;
      }
    }
  } else {
    ddhcp_block* block = blocks;

    // Find lease from xid
    for (uint32_t i = 0; i < config->number_of_blocks; i++) {
      if (block->state == DDHCP_OURS) {
        dhcp_lease* lease_iter = block->addresses;

        for (unsigned int j = 0 ; j < block->subnet_len ; j++) {
          if (lease_iter->state == OFFERED && lease_iter->xid == request->xid) {
            if (memcmp(request->chaddr, lease_iter->chaddr, 16) == 0) {
              lease = lease_iter;
              lease_block = block;
              lease_index = j;
              DEBUG("dhcp_request(...): Found requested lease\n");
              break;
            }
          }

          lease_iter++;
        }

        if (lease) {
          break;
        }
      }

      block++;
    }
  }

  if (!lease) {
    DEBUG("dhcp_request(...): Requested lease not found\n");
    // Send DHCP_NACK
    dhcp_nack(socket, request);
    return 2;
  }

  return dhcp_ack(socket, request, lease_block, lease_index, config);
}

void dhcp_hdl_release(dhcp_packet* packet, ddhcp_block* blocks, ddhcp_config* config) {
  DEBUG("dhcp_hdl_release(dhcp_packet, blocks, config)\n");
  ddhcp_block* lease_block = NULL;
  uint32_t lease_index = 0;
  struct in_addr addr;
  memcpy(&addr, &packet->ciaddr, sizeof(struct in_addr));
  uint8_t found = find_lease_from_address(&addr, blocks, config, &lease_block, &lease_index);


  dhcp_lease* lease;

  switch (found) {
  case 0:
    lease = lease_block->addresses + lease_index;

    // Check Hardware Address of client
    if (memcmp(packet->chaddr, lease->chaddr, 16) == 0) {
      _dhcp_release_lease(lease_block, lease_index);
    } else {
      ERROR("Hardware Adress transmitted by client and our record did not match, do nothing.\n");
    }

  case 1:
    // TODO Handle remote block
    // Send Message to neighbor
    break;

  default:
    // Since there is no reply to this message, we could `silently` drop this case.
    break;
  }
}

int dhcp_nack(int socket, dhcp_packet* from_client) {
  dhcp_packet* packet = build_initial_packet(from_client);

  if (! packet) {
    DEBUG("dhcp_discover(...) -> memory allocation failure\n");
    return 1;
  }

  packet->options_len = 1;
  packet->options = (dhcp_option*) calloc(sizeof(dhcp_option), 1);
  // TODO Error handling

  set_option(packet->options, packet->options_len, DHCP_CODE_MESSAGE_TYPE, 1, (uint8_t[]) {
    DHCPNAK
  });

  dhcp_packet_send(socket, packet);
  free(packet);

  return 0;
}

int dhcp_ack(int socket, dhcp_packet* request, ddhcp_block* lease_block, uint32_t lease_index, ddhcp_config* config) {
  time_t now = time(NULL);
  dhcp_packet* packet = build_initial_packet(request);
  dhcp_lease* lease = lease_block->addresses + lease_index;

  if (! packet) {
    DEBUG("dhcp_request(...) -> memory allocation failure\n");
    return 1;
  }

  // Mark lease as leased and register client
  memcpy(&lease->chaddr, &request->chaddr, 16);
  lease->xid = request->xid;
  lease->state = LEASED;
  lease->lease_end = now + DHCP_LEASE_TIME + DHCP_LEASE_SERVER_DELTA;

  addr_add(&lease_block->subnet, &packet->yiaddr, lease_index);
  DEBUG("dhcp_ack(...) offering address %i %s\n", lease_index, inet_ntoa(packet->yiaddr));

  // TODO We need a more extendable way to build up options
  packet->options_len = fill_options(request->options, request->options_len, &(config->options), 2, &packet->options) ;

  // TODO Error handling
  set_option(packet->options, packet->options_len, DHCP_CODE_MESSAGE_TYPE, 1, (uint8_t[]) {
    DHCPACK
  });
  // TODO correct type conversion, currently solution is simply wrong
  set_option(packet->options, packet->options_len, DHCP_CODE_ADDRESS_LEASE_TIME, 4, (uint8_t[]) {
    0, 0, 0, DHCP_LEASE_TIME
  });

  dhcp_packet_send(socket, packet);
  free(packet->options);
  free(packet);
  return 0;
}

int dhcp_has_free(struct ddhcp_block* block) {
  dhcp_lease* lease = block->addresses;

  for (unsigned int i = 0; i < block->subnet_len; i++) {
    if (lease->state == FREE) {
      return 1;
    }

    lease++;
  }

  return 0;
}

int dhcp_num_free(struct ddhcp_block* block) {
  int num = 0;
  dhcp_lease* lease = block->addresses;

  for (unsigned int i = 0 ; i < block->subnet_len ; i++) {
    if (lease->state == FREE) {
      num++;
    }

    lease++;
  }

  return num;
}

uint32_t dhcp_get_free_lease(ddhcp_block* block) {
  dhcp_lease* lease = block->addresses;

  for (uint32_t i = 0 ; i < block->subnet_len ; i++) {
    if (lease->state == FREE) {
      return i;
    }

    lease++;
  }

  ERROR("dhcp_get_free_lease(...): no free lease found");

  return block->subnet_len;
}

void dhcp_release_lease(uint32_t address, ddhcp_block* blocks, ddhcp_config* config) {

  ddhcp_block* lease_block = NULL;
  uint32_t lease_index = 0;
  struct in_addr addr;
  memcpy(&addr, &address, sizeof(struct in_addr));
  uint8_t found = find_lease_from_address(&addr, blocks, config, &lease_block, &lease_index);

  if (found == 0) {
    _dhcp_release_lease(lease_block, lease_index);
  } else {
    DEBUG("No lease for Address %s found.\n", inet_ntoa(addr));
  }
}

int dhcp_check_timeouts(ddhcp_block* block) {
  DEBUG("dhcp_check_timeouts(block)\n");
  dhcp_lease* lease = block->addresses;
  time_t now = time(NULL);

  int free_leases = 0;

  for (unsigned int i = 0 ; i < block->subnet_len ; i++) {
    if (lease->state != FREE && lease->lease_end < now) {
      _dhcp_release_lease(block, i);
    }

    if (lease->state == FREE) {
      free_leases++;
    }

    lease++;
  }

  return free_leases;
}
