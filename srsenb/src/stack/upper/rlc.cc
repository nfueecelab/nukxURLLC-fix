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

#include "srsenb/hdr/stack/upper/rlc.h"
#include "srsenb/hdr/stack/upper/common_enb.h"

#if(NUK)
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#endif

namespace srsenb {
#if(NUK)
rlc::rlc() : thread("NUK"){}
#endif

#if(NUK)
void rlc::init(pdcp_interface_rlc*    pdcp_,
               rrc_interface_rlc*     rrc_,
               mac_interface_rlc*     mac_,
               srslte::timer_handler* timers_,
               srslte::log*           log_h_,
			   std::string x2ap_myaddr_,
			   std::string x2ap_neiaddr_)
{
  pdcp   = pdcp_;
  rrc    = rrc_;
  log_h  = log_h_;
  mac    = mac_;
  timers = timers_;

  pool = srslte::byte_buffer_pool::get_instance();

  pthread_rwlock_init(&rwlock, nullptr);
  
  x2ap_myaddr  = x2ap_myaddr_;
  x2ap_neiaddr = x2ap_neiaddr_;
  socket_fd = -1;
  thread_running = false;
  thread_run_enable = false;
  split_count = 0;
  ratio_MeNB = 1;
  ratio_SeNB = 0;
  split_mode = false;
  duplication_mode = false;
  loss_MeNB = 0;
  loss_SeNB = 0;
  
  create_socket();
  srand((unsigned int) time(NULL));
  
  #if(IS_MENB)
  log_h->console("[NUK] This eNB is MeNB\n");
  #else
  log_h->console("[NUK] This eNB is SeNB\n");
  start(THREAD_PRIO);
  #endif
}
#else
void rlc::init(pdcp_interface_rlc*    pdcp_,
               rrc_interface_rlc*     rrc_,
               mac_interface_rlc*     mac_,
               srslte::timer_handler* timers_,
               srslte::log*           log_h_)
{
	pdcp   = pdcp_;
	rrc    = rrc_;
	log_h  = log_h_;
	mac    = mac_;
	timers = timers_;

	pool = srslte::byte_buffer_pool::get_instance();

	pthread_rwlock_init(&rwlock, nullptr);
}
#endif

void rlc::stop()
{
  #if(NUK)
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
  #endif
  pthread_rwlock_wrlock(&rwlock);
  for (auto& user : users) {
    user.second.rlc->stop();
  }
  users.clear();
  pthread_rwlock_unlock(&rwlock);
  pthread_rwlock_destroy(&rwlock);
}

void rlc::add_user(uint16_t rnti)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti) == 0) {
    std::unique_ptr<srslte::rlc> obj(new srslte::rlc(log_h));
    obj->init(&users[rnti], &users[rnti], timers, RB_ID_SRB0);
    users[rnti].rnti   = rnti;
    users[rnti].pdcp   = pdcp;
    users[rnti].rrc    = rrc;
    users[rnti].rlc    = std::move(obj);
    users[rnti].parent = this;
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::rem_user(uint16_t rnti)
{
  pthread_rwlock_wrlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->stop();
    users.erase(rnti);
  } else {
    log_h->error("Removing rnti=0x%x. Already removed\n", rnti);
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::clear_buffer(uint16_t rnti)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->empty_queue();
    for (int i = 0; i < SRSLTE_N_RADIO_BEARERS; i++) {
      mac->rlc_buffer_state(rnti, i, 0, 0);
    }
    log_h->info("Cleared buffer rnti=0x%x\n", rnti);
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::add_bearer(uint16_t rnti, uint32_t lcid, srslte::rlc_config_t cnfg)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->add_bearer(lcid, cnfg);
  }
  #if(NUK)
  cp_rnti = rnti;
  cp_lcid = lcid;
  log_h->debug("[NUK] bearer info: rnti : 0x%x , lcid : %u\n", cp_rnti, cp_lcid);
  #endif
  pthread_rwlock_unlock(&rwlock);
}

void rlc::add_bearer_mrb(uint16_t rnti, uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->add_bearer_mrb(lcid);
  }
  pthread_rwlock_unlock(&rwlock);
}

bool rlc::has_bearer(uint16_t rnti, uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  bool result = false;
  if (users.count(rnti)) {
    result = users[rnti].rlc->has_bearer(lcid);
  }
  pthread_rwlock_unlock(&rwlock);
  return result;
}

void rlc::read_pdu_pcch(uint8_t* payload, uint32_t buffer_size)
{
  rrc->read_pdu_pcch(payload, buffer_size);
}

int rlc::read_pdu(uint16_t rnti, uint32_t lcid, uint8_t* payload, uint32_t nof_bytes)
{
  int      ret;
  uint32_t tx_queue;

  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    if (rnti != SRSLTE_MRNTI) {
      ret      = users[rnti].rlc->read_pdu(lcid, payload, nof_bytes);
      tx_queue = users[rnti].rlc->get_buffer_state(lcid);
    } else {
      ret      = users[rnti].rlc->read_pdu_mch(lcid, payload, nof_bytes);
      tx_queue = users[rnti].rlc->get_total_mch_buffer_state(lcid);
    }
    // In the eNodeB, there is no polling for buffer state from the scheduler, thus
    // communicate buffer state every time a PDU is read

    uint32_t retx_queue = 0;
    log_h->debug("Buffer state PDCP: rnti=0x%x, lcid=%d, tx_queue=%d\n", rnti, lcid, tx_queue);
    mac->rlc_buffer_state(rnti, lcid, tx_queue, retx_queue);
  } else {
    ret = SRSLTE_ERROR;
  }
  pthread_rwlock_unlock(&rwlock);
  return ret;
}

void rlc::write_pdu(uint16_t rnti, uint32_t lcid, uint8_t* payload, uint32_t nof_bytes)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->write_pdu(lcid, payload, nof_bytes);

    // In the eNodeB, there is no polling for buffer state from the scheduler, thus
    // communicate buffer state every time a new PDU is written
    uint32_t tx_queue   = users[rnti].rlc->get_buffer_state(lcid);
    uint32_t retx_queue = 0;
    log_h->debug("Buffer state PDCP: rnti=0x%x, lcid=%d, tx_queue=%d\n", rnti, lcid, tx_queue);
    mac->rlc_buffer_state(rnti, lcid, tx_queue, retx_queue);
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::read_pdu_bcch_dlsch(uint32_t sib_index, uint8_t* payload)
{
  // RLC is transparent for BCCH
  rrc->read_pdu_bcch_dlsch(sib_index, payload);
}

void rlc::write_sdu(uint16_t rnti, uint32_t lcid, srslte::unique_byte_buffer_t sdu)
{
#if(NUK)
	uint32_t tx_queue;
	
#if(IS_MENB)
	pthread_rwlock_rdlock(&rwlock);
	switch(decide_path(lcid))
	{
		case 3 :
			// Duplication mode
			
			// Packet sendto SeNB
			if(!decide_loss(loss_SeNB))
			{
				if((sendto (socket_fd, sdu->msg, sdu->N_bytes, MSG_EOR, (struct sockaddr*)&nei_bindaddr, sizeof (struct sockaddr_in))) < 0)
				{
					log_h->error("[NUK] Split to SeNB failed, errno is : %d",errno);
					log_h->console("[NUK] Split to SeNB failed\n");
				}
			}else
			{
				#if(NUK_JIN_DEBUG)
				senb_loss_count++;
				log_h->debug("[NUK] SeNB loss : %u\n", senb_loss_count);
				#endif
			}

			// packet through MeNB
			if(!decide_loss(loss_MeNB))
			{
				if (users.count(rnti)) {
					if (rnti != SRSLTE_MRNTI) {
						users[rnti].rlc->write_sdu(lcid, std::move(sdu), false);
						tx_queue = users[rnti].rlc->get_buffer_state(lcid);
					} else {
						users[rnti].rlc->write_sdu_mch(lcid, std::move(sdu));
						tx_queue = users[rnti].rlc->get_total_mch_buffer_state(lcid);
					}
					// In the eNodeB, there is no polling for buffer state from the scheduler, thus
					// communicate buffer state every time a new SDU is written

					uint32_t retx_queue = 0;
					mac->rlc_buffer_state(rnti, lcid, tx_queue, retx_queue);
					log_h->info("Buffer state: rnti=0x%x, lcid=%d, tx_queue=%d\n", rnti, lcid, tx_queue);
				}
			}else
			{
				#if(NUK_JIN_DEBUG)
				menb_loss_count++;
				log_h->debug("[NUK] MeNB loss : %u\n", menb_loss_count);
				#endif
			}
			break;
		case 2 :
			// Packet sendto SeNB

			// #if(NUK_JIN_DEBUG)
			// menb_split_count++;
			// log_h->debug("[NUK] sendto SeNB : %d \n",menb_split_count);
			// log_h->console("[NUK] sendto SeNB : %d \n",menb_split_count);
			// #endif
			
			if(!decide_loss(loss_SeNB))
			{
				if((sendto (socket_fd, sdu->msg, sdu->N_bytes, MSG_EOR, (struct sockaddr*)&nei_bindaddr, sizeof (struct sockaddr_in))) < 0)
				{
					log_h->error("[NUK] Split to SeNB failed, errno is : %d",errno);
					log_h->console("[NUK] Split to SeNB failed\n");
				}
			}else
			{
				#if(NUK_JIN_DEBUG)
				senb_loss_count++;
				log_h->debug("[NUK] SeNB loss : %u\n", senb_loss_count);
				#endif
			}
			break;
		case 1 :
			// packet through MeNB

			// TODO : verify rlc drb lcid 
			// it always false

			// #if(NUK_JIN_DEBUG && lcid ==3)
			// menb_self_count++;
			// log_h->debug("[NUK] MeNB self DRB sdu : %d \n",menb_self_count);
			// log_h->console("[NUK] MeNB self DRB sdu : %d \n",menb_self_count);
			// #endif
			
			if(!decide_loss(loss_MeNB))
		{
			#if(NUK_JIN_DEBUG)
			log_h->debug("[NUK] srs_path\n");
			#endif
			if (users.count(rnti)) {
				if (rnti != SRSLTE_MRNTI) {
					users[rnti].rlc->write_sdu(lcid, std::move(sdu), false);
					tx_queue = users[rnti].rlc->get_buffer_state(lcid);
				} else {
					users[rnti].rlc->write_sdu_mch(lcid, std::move(sdu));
					tx_queue = users[rnti].rlc->get_total_mch_buffer_state(lcid);
				}
				// In the eNodeB, there is no polling for buffer state from the scheduler, thus
				// communicate buffer state every time a new SDU is written

				uint32_t retx_queue = 0;
				mac->rlc_buffer_state(rnti, lcid, tx_queue, retx_queue);
				log_h->info("Buffer state: rnti=0x%x, lcid=%d, tx_queue=%d\n", rnti, lcid, tx_queue);
			}
		}else
		{
			#if(NUK_JIN_DEBUG)
			menb_loss_count++;
			log_h->debug("[NUK] MeNB loss : %u\n", menb_loss_count);
			#endif
		}
			break;
		default:
			// Error number
			log_h->error("[NUK] RLC decide path unknown\n");
			log_h->console("[NUK] RLC decide path unknown\n");
			break;
	}
	pthread_rwlock_unlock(&rwlock);	
#else
	pthread_rwlock_rdlock(&rwlock);
	if (users.count(rnti)) {
		if (rnti != SRSLTE_MRNTI) {
			users[rnti].rlc->write_sdu(lcid, std::move(sdu), false);
			tx_queue = users[rnti].rlc->get_buffer_state(lcid);
		} else {
			users[rnti].rlc->write_sdu_mch(lcid, std::move(sdu));
			tx_queue = users[rnti].rlc->get_total_mch_buffer_state(lcid);
		}
		// In the eNodeB, there is no polling for buffer state from the scheduler, thus
		// communicate buffer state every time a new SDU is written

		uint32_t retx_queue = 0;
		mac->rlc_buffer_state(rnti, lcid, tx_queue, retx_queue);
		log_h->info("Buffer state: rnti=0x%x, lcid=%d, tx_queue=%d\n", rnti, lcid, tx_queue);
	}
	pthread_rwlock_unlock(&rwlock);
#endif
	
#else
  uint32_t tx_queue;

  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    if (rnti != SRSLTE_MRNTI) {
      users[rnti].rlc->write_sdu(lcid, std::move(sdu), false);
      tx_queue = users[rnti].rlc->get_buffer_state(lcid);
    } else {
      users[rnti].rlc->write_sdu_mch(lcid, std::move(sdu));
      tx_queue = users[rnti].rlc->get_total_mch_buffer_state(lcid);
    }
    // In the eNodeB, there is no polling for buffer state from the scheduler, thus
    // communicate buffer state every time a new SDU is written

    uint32_t retx_queue = 0;
    mac->rlc_buffer_state(rnti, lcid, tx_queue, retx_queue);
    log_h->info("Buffer state: rnti=0x%x, lcid=%d, tx_queue=%d\n", rnti, lcid, tx_queue);
  }
  pthread_rwlock_unlock(&rwlock);
#endif
}

void rlc::discard_sdu(uint16_t rnti, uint32_t lcid, uint32_t discard_sn)
{

  uint32_t tx_queue;

  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->discard_sdu(lcid, discard_sn);
    tx_queue = users[rnti].rlc->get_buffer_state(lcid);

    // In the eNodeB, there is no polling for buffer state from the scheduler, thus
    // communicate buffer state every time a new SDU is discarded
    uint32_t retx_queue = 0;
    mac->rlc_buffer_state(rnti, lcid, tx_queue, retx_queue);
    log_h->info("Buffer state: rnti=0x%x, lcid=%d, tx_queue=%d\n", rnti, lcid, tx_queue);
  }
  pthread_rwlock_unlock(&rwlock);
}

bool rlc::rb_is_um(uint16_t rnti, uint32_t lcid)
{
  bool ret = false;
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    ret = users[rnti].rlc->rb_is_um(lcid);
  }
  pthread_rwlock_unlock(&rwlock);
  return ret;
}

void rlc::user_interface::max_retx_attempted()
{
  rrc->max_retx_attempted(rnti);
}

void rlc::user_interface::write_pdu(uint32_t lcid, srslte::unique_byte_buffer_t sdu)
{
  if (lcid == RB_ID_SRB0) {
    rrc->write_pdu(rnti, lcid, std::move(sdu));
  } else {
    pdcp->write_pdu(rnti, lcid, std::move(sdu));
  }
}

void rlc::user_interface::write_pdu_bcch_bch(srslte::unique_byte_buffer_t sdu)
{
  ERROR("Error: Received BCCH from ue=%d\n", rnti);
}

void rlc::user_interface::write_pdu_bcch_dlsch(srslte::unique_byte_buffer_t sdu)
{
  ERROR("Error: Received BCCH from ue=%d\n", rnti);
}

void rlc::user_interface::write_pdu_pcch(srslte::unique_byte_buffer_t sdu)
{
  ERROR("Error: Received PCCH from ue=%d\n", rnti);
}

std::string rlc::user_interface::get_rb_name(uint32_t lcid)
{
  return std::string(rb_id_text[lcid]);
}

#if(NUK)
void rlc::run_thread()
{
#if(!IS_MENB)
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec =0;
    tv.tv_usec = 50000;
    thread_run_enable = true;
    thread_running = true;

    while(thread_running)
    {
		FD_ZERO(&readfds);
		FD_SET(socket_fd, &readfds);
		int n =0;
		n = select(socket_fd + 1, &readfds, NULL, NULL, &tv);
		srslte::unique_byte_buffer_t thread_pdu;

		switch(n)
		{
			case -1:
				log_h->error("[NUK] Failed to read from socket\n");
				log_h->console("[NUK] Failed to read from socket\n");
				break;
			case 0:
				//log_h->debug("Socket time out\n");
				break;
			case 1:
				#if(NUK_JIN_DEBUG)
				log_h->info("[NUK] Received packets\n");
				log_h->console("[NUK] Received packets\n");
				#endif
				socklen_t addr_len;
				thread_pdu = allocate_unique_buffer(*pool);

				if(FD_ISSET(socket_fd, &readfds))
				{
					thread_pdu->N_bytes = recvfrom(socket_fd, thread_pdu->msg, SRSENB_MAX_BUFFER_SIZE_BYTES - SRSENB_BUFFER_HEADER_OFFSET, 0 , (struct sockaddr*)&my_bindaddr, &addr_len);
				}
				if(cp_lcid == 3)
				{
					write_sdu(cp_rnti,cp_lcid,std::move(thread_pdu));  
					#if(NUK_JIN_DEBUG)
					senb_count++;
					log_h->info("[NUK] SeNB DRB sdu : %d \n",senb_count);
					log_h->console("[NUK] SeNB DRB sdu : %d \n",senb_count);
					#endif
				}else
				{
					log_h->debug("[NUK] rlc lcid !=3 \n");
				}
				break;
			default:
				log_h->debug("[NUK] select() unknow return. n is : %d , errno is : %d \n", n, errno);
				log_h->console("[NUK] select() unknow return. n is : %d , errno is : %d \n", n, errno);
				break;
		}
    }
#endif
}

int rlc::decide_path(uint32_t lcid)
{
	// Return 3 >> Duplication
	// Return 2 >> Split to SeNB
	// Return 1 >> Split to MeNB

	if(lcid != 3)
		return 1;

	if(duplication_mode)
		return 3;

	if(!split_mode)
		return 1;

	// TODO: 1.better check ratio and count value method

	// Only to SeNB
	if(ratio_MeNB == 0 && ratio_SeNB !=0)
		return 2;

	// Only to MeNB
	if(ratio_SeNB == 0 && ratio_MeNB !=0)
		return 1;
	
	// Error setting
	if(ratio_MeNB == 0 && ratio_SeNB == 0)
		return -1;
	
	// Check if count value up to one cycle
	if(split_count >= (ratio_MeNB + ratio_SeNB))
	{
		split_count = 0;
		// #if(NUK_JIN_DEBUG)
		// log_h->debug("one cycle \n");
		// log_h->console("one cycle \n");
		// #endif
	}

	// Caltulate per packet, first packet always to MeNB
	if(split_count < ratio_MeNB)
	{
		// Split to MeNB
		split_count++;
		return 1;
	}else
	{
		// Split to SeNB
		split_count++;
		return 2;
	}
}

void rlc::create_socket()
{
	// Get a UDP socket 
	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if( socket_fd < 0)
	{
		log_h->error("[NUK] Failed to create x2 socket \n");
		log_h->console("[NUK] Failed to create x2 socket \n");
	}

	// Set socket to non_block
	fcntl(socket_fd, F_SETFL, O_NONBLOCK);

	int enable = 1;
#if defined(SO_REUSEADDR)
	if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
		log_h->error("setsockopt(SO_REUSEADDR) failed\n");
#endif
#if defined(SO_REUSEPORT)
	if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0)
		log_h->error("setsockopt(SO_REUSEPORT) failed\n");
#endif

	// Set sockaddr of MeNB and SeNB
	bzero(&my_bindaddr, sizeof(struct sockaddr_in));
	my_bindaddr.sin_family      = AF_INET;
	my_bindaddr.sin_addr.s_addr = inet_addr(x2ap_myaddr.c_str());

	bzero(&nei_bindaddr, sizeof(struct sockaddr_in));
	nei_bindaddr.sin_family      = AF_INET;
	nei_bindaddr.sin_addr.s_addr = inet_addr(x2ap_neiaddr.c_str());

#if(IS_MENB)
	my_bindaddr.sin_port = htons(0);
	nei_bindaddr.sin_port = htons(X2_PORT);
#else
	my_bindaddr.sin_port = htons(X2_PORT);
	nei_bindaddr.sin_port = htons(0);
#endif

	if ((bind(socket_fd, (struct sockaddr*)&my_bindaddr, sizeof(struct sockaddr_in))) == -1)
	{
		log_h->error("[NUK] Failed to bind on my address: %s, port: %d \n", x2ap_myaddr.c_str(), my_bindaddr.sin_port);
		log_h->console("[NUK] Failed to bind on my address: %s, port: %d \n", x2ap_myaddr.c_str(), my_bindaddr.sin_port);
	}else
	{
		log_h->info("[NUK] Success bind RLC socket\n");
	}
}

void rlc::set_split_ratio(uint8_t ratio_MeNB_, uint8_t ratio_SeNB_)
{
#if(NUK_JIN_DEBUG)
	//log_h->console("[NUK] enb::rlc.cc input ratio is %u(M) , %u(S)\n", ratio_MeNB_, ratio_SeNB_);
	menb_split_count = 0;
	menb_self_count = 0;
	senb_count = 0;
#endif
#if(IS_MENB)
	// Check ratio value
	if(ratio_MeNB_ == 0 && ratio_SeNB_ ==0)
	{
		log_h->error("[NUK] The ratio of eNBs all are 0 ! \n");
		log_h->console("[NUK] The ratio of eNBs all are 0 ! \n");
		ratio_MeNB = 1;
		ratio_SeNB = 0;
		log_h->console("[NUK] Force set ratio %u(M) : %u(S) \n", ratio_MeNB, ratio_SeNB);
		return;
	}
	
	ratio_MeNB = ratio_MeNB_;
	ratio_SeNB = ratio_SeNB_;
	log_h->info("[NUK] set ratio %u(M) : %u(S) \n", ratio_MeNB, ratio_SeNB);
	log_h->console("[NUK] set ratio %u(M) : %u(S) \n", ratio_MeNB, ratio_SeNB);
	split_count = 0;
#endif
}

void rlc::set_split_mode()
{
#if(IS_MENB)
	split_mode = !split_mode;
	if(split_mode)
	{
		log_h->info("[NUK] Split_mode is ON \n");
		log_h->console("[NUK] Split_mode is ON \n");
		duplication_mode = false;
		split_count =0;
		set_split_ratio(1,0);
	}else
	{
		log_h->info("[NUK] Split_mode is OFF \n");
		log_h->console("[NUK] Split_mode is OFF \n");
	}
#endif
}
void rlc::set_lossrate(uint8_t loss_MeNB_, uint8_t loss_SeNB_)
{
#if(IS_MENB)
	// Check ratio value
	if(loss_MeNB_ > 100 || loss_SeNB_ > 100)
	{
		#if(NUK_JIN_DEBUG)
		log_h->error("[NUK] The loss rate can't bigger than 100! \n");
		log_h->console("[NUK] The loss rate can't bigger than 100! \n");
		#endif
		loss_MeNB = 0;
		loss_SeNB = 0;
		log_h->console("[NUK] Force set loss rate %u%%(M) : %u%%(S) \n", loss_MeNB, loss_SeNB);
		return;
	}
	
	loss_MeNB = loss_MeNB_;
	loss_SeNB = loss_SeNB_;
	log_h->info("[NUK] set loss rate %u%%(M) : %u%%(S) \n", loss_MeNB, loss_SeNB);
	log_h->console("[NUK] set loss rate %u%%(M) : %u%%(S) \n", loss_MeNB, loss_SeNB);
#endif
}
void rlc::set_duplication_mode()
{
#if(IS_MENB)
	duplication_mode = !duplication_mode;
#if(NUK_JIN_DEBUG)
	if(duplication_mode)
	{
		split_mode = false;
		log_h->info("[NUK] Duplication_mode is ON \n");
		log_h->console("[NUK] Duplication_mode is ON \n");
	}else
	{
		log_h->info("[NUK] Duplication_mode is OFF \n");
		log_h->console("[NUK] Duplication_mode is OFF \n");
	}
#endif
#endif
}
bool rlc::decide_loss(uint8_t rate)
{
	uint64_t r = rand() % 100;
	if(r < rate)
	{
		// the packet should be lost
		return true;
	}else
	{
		// the packet should be transmited
		return false;
	}
}
#endif
} // namespace srsenb
