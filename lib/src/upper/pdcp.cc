/*
 * Copyright 2013-2019 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srslte/upper/pdcp.h"

#include <string>

#if(NUK && NUK_UE)
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "srslte/common/buffer_pool.h"
#endif

namespace srslte {

#if(NUK && NUK_UE)
pdcp::pdcp(srslte::timer_handler* timers_, srslte::log* log_) : timers(timers_), pdcp_log(log_), thread("UE PDCP")
{
  pthread_rwlock_init(&rwlock, NULL);
}
#else
pdcp::pdcp(srslte::timer_handler* timers_, srslte::log* log_) : timers(timers_), pdcp_log(log_)
{
  pthread_rwlock_init(&rwlock, NULL);
}
#endif

pdcp::~pdcp()
{
  // destroy all remaining entities
  pthread_rwlock_wrlock(&rwlock);
  for (pdcp_map_t::iterator it = pdcp_array.begin(); it != pdcp_array.end(); ++it) {
    delete (it->second);
  }
  pdcp_array.clear();

  for (pdcp_map_t::iterator it = pdcp_array_mrb.begin(); it != pdcp_array_mrb.end(); ++it) {
    delete (it->second);
  }
  pdcp_array_mrb.clear();

  pthread_rwlock_unlock(&rwlock);
  pthread_rwlock_destroy(&rwlock);
}

void pdcp::init(srsue::rlc_interface_pdcp* rlc_, srsue::rrc_interface_pdcp* rrc_, srsue::gw_interface_pdcp* gw_)
{
  rlc = rlc_;
  rrc = rrc_;
  gw  = gw_;
}
#if(NUK && NUK_UE)
void pdcp::init(srsue::rlc_interface_pdcp* rlc_, srsue::rrc_interface_pdcp* rrc_, srsue::gw_interface_pdcp* gw_, std::string node_type_)
{
  rlc = rlc_;
  rrc = rrc_;
  gw  = gw_;

  std::string str1 = "aggregation";
  std::string str2 = "transmission";
  if(str1.compare(node_type_) == 0)
  {
    node_status = 0;
    pdcp_log->info("\n[NUK] UE is aggregation node \n");
    pdcp_log->console("\n[NUK] UE is aggregation node \n");
  }else if (str2.compare(node_type_) == 0)
  {
    node_status = 1;
    pdcp_log->info("\n[NUK] UE is transmission node \n");
    pdcp_log->console("\n[NUK] UE is transmission node \n");
  }else
  {
    node_status = -1;
    pdcp_log->error("\n[NUK] UE node type error!\n");
    pdcp_log->console("\n[NUK] UE node type error!\n");
  }

  pool =  srslte::byte_buffer_pool::get_instance();

  create_inter_socket(node_status);
  if(node_status == 0)
  {
    #if(NUK_JIN_DEBUG)
    pdcp_log->console("[NUK] UE start thread\n");
    #endif
    start(THREAD_PRIO);
  }
}
#endif

void pdcp::stop()
{
#if(NUK && NUK_UE)
{
  if(thread_run_enable)
  {
    thread_run_enable =false;
    int cnt = 0;
    while(thread_running && cnt <100)
    {
      usleep(5000);
      cnt++;
    }
    if(thread_running)
    {
      thread_cancel();
    }
    wait_thread_finish();
  }
  if(socket_fd)
    close(socket_fd);
}
#endif
}

void pdcp::reestablish()
{
  pthread_rwlock_rdlock(&rwlock);
  for (pdcp_map_t::iterator it = pdcp_array.begin(); it != pdcp_array.end(); ++it) {
    it->second->reestablish();
  }
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::reestablish(uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    pdcp_array.at(lcid)->reestablish();
  }
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::reset()
{
  // destroy all bearers
  pthread_rwlock_wrlock(&rwlock);
  for (pdcp_map_t::iterator it = pdcp_array.begin(); it != pdcp_array.end(); /* post increment in erase */) {
    it->second->reset();
    delete (it->second);
    pdcp_array.erase(it++);
  }
  pthread_rwlock_unlock(&rwlock);
}

/*******************************************************************************
  RRC/GW interface
*******************************************************************************/
bool pdcp::is_lcid_enabled(uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  bool ret = false;
  if (valid_lcid(lcid)) {
    ret = pdcp_array.at(lcid)->is_active();
  }
  pthread_rwlock_unlock(&rwlock);
  return ret;
}

void pdcp::write_sdu(uint32_t lcid, unique_byte_buffer_t sdu, bool blocking)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    pdcp_array.at(lcid)->write_sdu(std::move(sdu), blocking);
  } else {
    pdcp_log->warning("Writing sdu: lcid=%d. Deallocating sdu\n", lcid);
  }
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::write_sdu_mch(uint32_t lcid, unique_byte_buffer_t sdu)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_mch_lcid(lcid)) {
    pdcp_array_mrb.at(lcid)->write_sdu(std::move(sdu), true);
  }
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::add_bearer(uint32_t lcid, pdcp_config_t cfg)
{
  pthread_rwlock_wrlock(&rwlock);
  if (not valid_lcid(lcid)) {
    if (not pdcp_array.insert(pdcp_map_pair_t(lcid, new pdcp_entity_lte(rlc, rrc, gw, timers, pdcp_log))).second) {
      pdcp_log->error("Error inserting PDCP entity in to array\n.");
      goto unlock_and_exit;
    }
    pdcp_array.at(lcid)->init(lcid, cfg);
    pdcp_log->info("Add %s (lcid=%d, bearer_id=%d, sn_len=%dbits)\n",
                   rrc->get_rb_name(lcid).c_str(),
                   lcid,
                   cfg.bearer_id,
                   cfg.sn_len);
#if(NUK && NUK_UE)
    cp_lcid = lcid;
#endif
  } else {
    pdcp_log->warning("Bearer %s already configured. Reconfiguration not supported\n", rrc->get_rb_name(lcid).c_str());
  }
unlock_and_exit:
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::add_bearer_mrb(uint32_t lcid, pdcp_config_t cfg)
{
  pthread_rwlock_wrlock(&rwlock);
  if (not valid_mch_lcid(lcid)) {
    if (not pdcp_array_mrb.insert(pdcp_map_pair_t(lcid, new pdcp_entity_lte(rlc, rrc, gw, timers, pdcp_log))).second) {
      pdcp_log->error("Error inserting PDCP entity in to array\n.");
      goto unlock_and_exit;
    }
    pdcp_array_mrb.at(lcid)->init(lcid, cfg);
    pdcp_log->info("Add %s (lcid=%d, bearer_id=%d, sn_len=%dbits)\n",
                   rrc->get_rb_name(lcid).c_str(),
                   lcid,
                   cfg.bearer_id,
                   cfg.sn_len);
  } else {
    pdcp_log->warning("Bearer %s already configured. Reconfiguration not supported\n", rrc->get_rb_name(lcid).c_str());
  }
unlock_and_exit:
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::del_bearer(uint32_t lcid)
{
  pthread_rwlock_wrlock(&rwlock);
  if (valid_lcid(lcid)) {
    pdcp_map_t::iterator it = pdcp_array.find(lcid);
    delete (it->second);
    pdcp_array.erase(it);
    pdcp_log->warning("Deleted PDCP bearer %s\n", rrc->get_rb_name(lcid).c_str());
  } else {
    pdcp_log->warning("Can't delete bearer %s. Bearer doesn't exist.\n", rrc->get_rb_name(lcid).c_str());
  }
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::change_lcid(uint32_t old_lcid, uint32_t new_lcid)
{
  pthread_rwlock_wrlock(&rwlock);

  // make sure old LCID exists and new LCID is still free
  if (valid_lcid(old_lcid) && not valid_lcid(new_lcid)) {
    // insert old PDCP entity into new LCID
    pdcp_map_t::iterator it          = pdcp_array.find(old_lcid);
    pdcp_entity_lte*     pdcp_entity = it->second;
    if (not pdcp_array.insert(pdcp_map_pair_t(new_lcid, pdcp_entity)).second) {
      pdcp_log->error("Error inserting PDCP entity into array\n.");
      goto exit;
    }
    // erase from old position
    pdcp_array.erase(it);
    pdcp_log->warning("Changed LCID of PDCP bearer from %d to %d\n", old_lcid, new_lcid);
  } else {
    pdcp_log->error(
        "Can't change PDCP of bearer %s from %d to %d. Bearer doesn't exist or new LCID already occupied.\n",
        rrc->get_rb_name(old_lcid).c_str(),
        old_lcid,
        new_lcid);
  }
exit:
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::config_security(uint32_t                    lcid,
                           uint8_t*                    k_rrc_enc,
                           uint8_t*                    k_rrc_int,
                           uint8_t*                    k_up_enc,
                           CIPHERING_ALGORITHM_ID_ENUM cipher_algo,
                           INTEGRITY_ALGORITHM_ID_ENUM integ_algo)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    pdcp_array.at(lcid)->config_security(k_rrc_enc, k_rrc_int, k_up_enc, nullptr, cipher_algo, integ_algo);
  }
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::config_security_all(uint8_t*                    k_rrc_enc,
                               uint8_t*                    k_rrc_int,
                               uint8_t*                    k_up_enc,
                               CIPHERING_ALGORITHM_ID_ENUM cipher_algo,
                               INTEGRITY_ALGORITHM_ID_ENUM integ_algo)
{
  pthread_rwlock_rdlock(&rwlock);
  for (pdcp_map_t::iterator it = pdcp_array.begin(); it != pdcp_array.end(); ++it) {
    it->second->config_security(k_rrc_enc, k_rrc_int, k_up_enc, nullptr, cipher_algo, integ_algo);
  }
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::enable_integrity(uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    pdcp_array.at(lcid)->enable_integrity();
  }
  pthread_rwlock_unlock(&rwlock);
}

void pdcp::enable_encryption(uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    pdcp_array.at(lcid)->enable_encryption();
  }
  pthread_rwlock_unlock(&rwlock);
}

bool pdcp::get_bearer_status(uint32_t lcid, uint16_t* dlsn, uint16_t* dlhfn, uint16_t* ulsn, uint16_t* ulhfn)
{
  if (not valid_lcid(lcid)) {
    return false;
  }
  pdcp_array[lcid]->get_bearer_status(dlsn, dlhfn, ulsn, ulhfn);
  return true;
}

/*******************************************************************************
  RLC interface
*******************************************************************************/
void pdcp::write_pdu(uint32_t lcid, unique_byte_buffer_t pdu)
{
#if(NUK && NUK_UE)

  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    if(node_status == 1 && lcid == 3)
    {
      // transmission node(UE2) send data pdu to aggregation node(UE1)
      local_communication(std::move(pdu));
    }else
    {
      pdcp_array.at(lcid)->write_pdu(std::move(pdu));
    }
  } else {
    pdcp_log->warning("Writing pdu: lcid=%d. Deallocating pdu\n", lcid);
  }
  pthread_rwlock_unlock(&rwlock);

#else

  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    pdcp_array.at(lcid)->write_pdu(std::move(pdu));
  } else {
    pdcp_log->warning("Writing pdu: lcid=%d. Deallocating pdu\n", lcid);
  }
  pthread_rwlock_unlock(&rwlock);

#endif
}

void pdcp::write_pdu_bcch_bch(unique_byte_buffer_t sdu)
{
  rrc->write_pdu_bcch_bch(std::move(sdu));
}

void pdcp::write_pdu_bcch_dlsch(unique_byte_buffer_t sdu)
{
  rrc->write_pdu_bcch_dlsch(std::move(sdu));
}

void pdcp::write_pdu_pcch(unique_byte_buffer_t sdu)
{
  rrc->write_pdu_pcch(std::move(sdu));
}

void pdcp::write_pdu_mch(uint32_t lcid, unique_byte_buffer_t sdu)
{
  if (0 == lcid) {
    rrc->write_pdu_mch(lcid, std::move(sdu));
  } else {
    gw->write_pdu_mch(lcid, std::move(sdu));
  }
}

/*******************************************************************************
  Helpers (Lock must be hold when calling those)
*******************************************************************************/
bool pdcp::valid_lcid(uint32_t lcid)
{
  if (lcid >= SRSLTE_N_RADIO_BEARERS) {
    pdcp_log->error("Radio bearer id must be in [0:%d] - %d", SRSLTE_N_RADIO_BEARERS, lcid);
    return false;
  }

  if (pdcp_array.find(lcid) == pdcp_array.end()) {
    return false;
  }

  return true;
}

bool pdcp::valid_mch_lcid(uint32_t lcid)
{
  if (lcid >= SRSLTE_N_MCH_LCIDS) {
    pdcp_log->error("Radio bearer id must be in [0:%d] - %d", SRSLTE_N_RADIO_BEARERS, lcid);
    return false;
  }

  if (pdcp_array_mrb.find(lcid) == pdcp_array_mrb.end()) {
    return false;
  }

  return true;
}


#if(NUK && NUK_UE)
/*******************************************************************************
  NUK private funciton
*******************************************************************************/

void pdcp::create_inter_socket(int node_status_)
{
  memset(&ser, 0, sizeof(struct sockaddr_un));
  ser.sun_family = AF_UNIX;
  memcpy(ser.sun_path+1, ser_name, strlen(ser_name));

  memset(&cli, 0, sizeof(struct sockaddr_un));
  cli.sun_family = AF_UNIX;
  memcpy(cli.sun_path+1, cli_name, strlen(cli_name));
  
  if((socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
  {
    perror("UE PDCP Unix socket establish failed\n");
  }
  
  socklen_t len = sizeof(struct sockaddr_un);
  switch(node_status_)
  {
    case 0:
      // Aggregation node as Server 
      if(bind(socket_fd, (const struct sockaddr *)&ser, len) < 0)
      {
        perror("UE PDCP Unix socket bind failed\n");
        pdcp_log->error("[NUK] Server bind socket failed\n");
      }
      break;
    case 1:
      // Transmission node as Client
      if(bind(socket_fd, (const struct sockaddr *)&cli, len) < 0)
      {
        perror("UE PDCP Unix socket bind failed\n");
        pdcp_log->error("[NUK] Client bind socket failed\n");
      }
      break;
    default:
      // Other number as Error
      pdcp_log->error("[NUK] Invalid number to bind socket\n");
      break;
  }
}

void pdcp::run_thread()
{
  fd_set readfds;
  struct timeval tv;
  tv.tv_sec =0;
  tv.tv_usec = 50000;
  thread_run_enable = true;
  thread_running = true;

  int SRSENB_BUFFER_HEADER_OFFSET = 1024;
  int SRSENB_MAX_BUFFER_SIZE_BYTES = 12756;


  while(thread_running)
  {
    FD_ZERO(&readfds);
    FD_SET(socket_fd, &readfds);
    srslte::unique_byte_buffer_t thread_pdu = allocate_unique_buffer(*pool);
    int n =0;
    n = select(socket_fd + 1, &readfds, NULL, NULL, &tv);

    switch(n)
    {
      case -1:
        pdcp_log->error("[NUK] Failed to read from socket\n");
        pdcp_log->console("[NUK] Failed to read from socket\n");
        break;

      case 0:
        //pdcp_log->debug("Socket time out\n");
        break;

      case 1:
        if(FD_ISSET(socket_fd, &readfds))
        {
          thread_pdu->N_bytes = recvfrom(socket_fd, thread_pdu->msg, SRSENB_MAX_BUFFER_SIZE_BYTES - SRSENB_BUFFER_HEADER_OFFSET, 0 , NULL, NULL);
        }
        #if(NUK_JIN_DEBUG)
        pdcp_log->info("[NUK] Received packets\n");
        pdcp_log->console("[NUK] Received packets\n");
        pdcp_log->info_hex(thread_pdu->msg, thread_pdu->N_bytes, "[NUK] UE1 Unix socket packet content\n");
        #endif

        if(cp_lcid == 3)
        {
          #if(NUK_JIN_DEBUG)
          pdcp_log->info("[NUK] UE1 aggregate pdu : %d\n",ue1_count);
          pdcp_log->console("[NUK] UE1 aggregate pdu : %d\n",ue1_count);
          ue1_count++;
          #endif
          // TODO: unpack_pdcp_sn will lead to gw drop packet.
          write_pdu(cp_lcid,std::move(thread_pdu));  
        }else
        {
          pdcp_log->debug("[NUK] thread pdcp cp_lcid !=3 \n");
        }
        break;

      default:
        pdcp_log->debug("[NUK] select() unknow return. n is : %d , errno is : %d \n", n, errno);
        pdcp_log->console("[NUK] select() unknow return. n is : %d , errno is : %d \n", n, errno);
        break;
    }
  }
}

void pdcp::local_communication(unique_byte_buffer_t pdu)
{
  if(socket_fd){
    socklen_t len = sizeof(struct sockaddr_un);
    if(sendto(socket_fd, pdu->msg, pdu->N_bytes,0, (struct sockaddr *)&ser,len) < 0){
    pdcp_log->error("[NUK] UE Unix socket sendto failed\n");
    }
  }else{
    pdcp_log->error("[NUK] local_communication socket not ready!\n");
    pdcp_log->console("[NUK] local_communication socket not ready!\n");
  }
}
#endif
} // namespace srslte
