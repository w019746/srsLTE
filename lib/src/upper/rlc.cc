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

#include "srslte/upper/rlc.h"
#include "srslte/upper/rlc_tm.h"
#include "srslte/upper/rlc_um.h"
#include "srslte/upper/rlc_am.h"

using namespace asn1::rrc;

namespace srslte {

rlc::rlc()
{
  pool = byte_buffer_pool::get_instance();
  rlc_log = NULL;
  pdcp = NULL;
  rrc = NULL;
  mac_timers = NULL;
  ue = NULL;
  bzero(metrics_time, sizeof(metrics_time));
  pthread_rwlock_init(&rwlock, NULL);
}

rlc::~rlc()
{
  // destroy all remaining entities
  pthread_rwlock_wrlock(&rwlock);
  for (rlc_map_t::iterator it = rlc_array.begin(); it != rlc_array.end(); ++it) {
    delete(it->second);
  }
  rlc_array.clear();

  for (rlc_map_t::iterator it = rlc_array_mrb.begin(); it != rlc_array_mrb.end(); ++it) {
    delete(it->second);
  }
  rlc_array_mrb.clear();

  pthread_rwlock_unlock(&rwlock);
  pthread_rwlock_destroy(&rwlock);
}

void rlc::init(srsue::pdcp_interface_rlc* pdcp_,
               srsue::rrc_interface_rlc*  rrc_,
               srsue::ue_interface*       ue_,
               log*                       rlc_log_,
               mac_interface_timers*      mac_timers_,
               uint32_t                   lcid_)
{
  pdcp    = pdcp_;
  rrc     = rrc_;
  ue      = ue_;
  rlc_log = rlc_log_;
  mac_timers = mac_timers_;
  default_lcid = lcid_;

  gettimeofday(&metrics_time[1], NULL);
  reset_metrics();

  // create default RLC_TM bearer for SRB0
  add_bearer(default_lcid, srslte_rlc_config_t());
}

void rlc::reset_metrics() 
{
  for (rlc_map_t::iterator it = rlc_array.begin(); it != rlc_array.end(); ++it) {
    it->second->reset_metrics();
  }

  for (rlc_map_t::iterator it = rlc_array_mrb.begin(); it != rlc_array_mrb.end(); ++it) {
    it->second->reset_metrics();
  }
}

void rlc::stop()
{
  pthread_rwlock_rdlock(&rwlock);
  for (rlc_map_t::iterator it = rlc_array.begin(); it != rlc_array.end(); ++it) {
    it->second->stop();
  }
  for (rlc_map_t::iterator it = rlc_array_mrb.begin(); it != rlc_array_mrb.end(); ++it) {
    it->second->stop();
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::get_metrics(rlc_metrics_t &m)
{
  pthread_rwlock_rdlock(&rwlock);

  gettimeofday(&metrics_time[2], NULL);
  get_time_interval(metrics_time);
  double secs = (double)metrics_time[0].tv_sec + metrics_time[0].tv_usec*1e-6;

  for (rlc_map_t::iterator it = rlc_array.begin(); it != rlc_array.end(); ++it) {
    m.dl_tput_mbps[it->first] = (it->second->get_num_rx_bytes()*8/static_cast<double>(1e6))/secs;
    m.ul_tput_mbps[it->first] = (it->second->get_num_tx_bytes()*8/static_cast<double>(1e6))/secs;
    rlc_log->info("LCID=%d, RX throughput: %4.6f Mbps. TX throughput: %4.6f Mbps.\n",
                    it->first,
                    (it->second->get_num_rx_bytes()*8/static_cast<double>(1e6))/secs,
                    (it->second->get_num_tx_bytes()*8/static_cast<double>(1e6))/secs);
  }

  // Add multicast metrics
  for (rlc_map_t::iterator it = rlc_array_mrb.begin(); it != rlc_array_mrb.end(); ++it) {
    m.dl_tput_mbps[it->first] = (it->second->get_num_rx_bytes()*8/static_cast<double>(1e6))/secs;
    rlc_log->info("MCH_LCID=%d, RX throughput: %4.6f Mbps\n",
                  it->first,
                  (it->second->get_num_rx_bytes()*8/static_cast<double>(1e6))/secs);
  }

  memcpy(&metrics_time[1], &metrics_time[2], sizeof(struct timeval));
  reset_metrics();

  pthread_rwlock_unlock(&rwlock);
}

// Reestablish all RLC bearer
void rlc::reestablish()
{
  pthread_rwlock_rdlock(&rwlock);

  for (rlc_map_t::iterator it = rlc_array.begin(); it != rlc_array.end(); ++it) {
    it->second->reestablish();
  }

  for (rlc_map_t::iterator it = rlc_array_mrb.begin(); it != rlc_array_mrb.end(); ++it) {
    it->second->reestablish();
  }

  pthread_rwlock_unlock(&rwlock);
}

// Reestablish a specific RLC bearer
void rlc::reestablish(uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    rlc_array.at(lcid)->reestablish();
  } else {
    rlc_log->warning("RLC LCID %d doesn't exist. Deallocating SDU\n", lcid);
  }
  pthread_rwlock_unlock(&rwlock);
}

// Resetting the RLC layer returns the object to the state after the call to init():
// All LCIDs are removed, except SRB0
void rlc::reset()
{
  pthread_rwlock_wrlock(&rwlock);

  for (rlc_map_t::iterator it = rlc_array.begin(); it != rlc_array.end(); ++it) {
    it->second->stop();
    delete(it->second);
  }
  rlc_array.clear();

  for (rlc_map_t::iterator it = rlc_array_mrb.begin(); it != rlc_array_mrb.end(); ++it) {
    it->second->stop();
    delete(it->second);
  }
  rlc_array_mrb.clear();

  pthread_rwlock_unlock(&rwlock);

  // Add SRB0 again
  add_bearer(default_lcid, srslte_rlc_config_t());
}

void rlc::empty_queue()
{
  // Empty Tx queue, not needed for MCH bearers
  pthread_rwlock_rdlock(&rwlock);
  for (rlc_map_t::iterator it = rlc_array.begin(); it != rlc_array.end(); ++it) {
    it->second->empty_queue();
  }
  pthread_rwlock_unlock(&rwlock);
}

/*******************************************************************************
  PDCP interface
*******************************************************************************/

void rlc::write_sdu(uint32_t lcid, byte_buffer_t *sdu, bool blocking)
{
  // FIXME: rework build PDU logic to allow large SDUs (without concatenation)
  if (sdu->N_bytes > RLC_MAX_SDU_SIZE) {
    rlc_log->warning("Dropping too long SDU of size %d B (Max. size %d B).\n", sdu->N_bytes, RLC_MAX_SDU_SIZE);
    pool->deallocate(sdu);
    return;
  }

  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    rlc_array.at(lcid)->write_sdu(sdu, blocking);
  } else {
    rlc_log->warning("RLC LCID %d doesn't exist. Deallocating SDU\n", lcid);
    byte_buffer_pool::get_instance()->deallocate(sdu);
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::write_sdu_mch(uint32_t lcid, byte_buffer_t *sdu)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid_mrb(lcid)) {
    rlc_array_mrb.at(lcid)->write_sdu(sdu, false); // write in non-blocking mode by default
  } else {
    rlc_log->warning("RLC LCID %d doesn't exist. Deallocating SDU\n", lcid);
    byte_buffer_pool::get_instance()->deallocate(sdu);
  }
  pthread_rwlock_unlock(&rwlock);
}


bool rlc::rb_is_um(uint32_t lcid)
{
  bool ret = false;

  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    ret = rlc_array.at(lcid)->get_mode() == RLC_MODE_UM;
  } else {
    rlc_log->warning("LCID %d doesn't exist.\n", lcid);
  }
  pthread_rwlock_unlock(&rwlock);

  return ret;
}

/*******************************************************************************
  MAC interface
*******************************************************************************/
bool rlc::has_data(uint32_t lcid)
{
  bool has_data = false;

  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    has_data = rlc_array.at(lcid)->has_data();
  }
  pthread_rwlock_unlock(&rwlock);

  return has_data;
}

uint32_t rlc::get_buffer_state(uint32_t lcid)
{
  uint32_t ret = 0;

  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    ret = rlc_array.at(lcid)->get_buffer_state();
  }
  pthread_rwlock_unlock(&rwlock);

  return ret;
}

uint32_t rlc::get_total_mch_buffer_state(uint32_t lcid)
{
  uint32_t ret = 0;

  pthread_rwlock_rdlock(&rwlock);

  if (valid_lcid_mrb(lcid)) {
    ret = rlc_array_mrb.at(lcid)->get_buffer_state();
  }
  pthread_rwlock_unlock(&rwlock);

  return ret;
}

int rlc::read_pdu(uint32_t lcid, uint8_t *payload, uint32_t nof_bytes)
{
  uint32_t ret = 0;

  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    ret = rlc_array.at(lcid)->read_pdu(payload, nof_bytes);
  } else {
    rlc_log->warning("LCID %d doesn't exist.\n", lcid);
  }
  pthread_rwlock_unlock(&rwlock);

  return ret;
}

int rlc::read_pdu_mch(uint32_t lcid, uint8_t *payload, uint32_t nof_bytes)
{
  uint32_t ret = 0;

  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid_mrb(lcid)) {
    ret = rlc_array_mrb.at(lcid)->read_pdu(payload, nof_bytes);
  } else {
    rlc_log->warning("LCID %d doesn't exist.\n", lcid);
  }
  pthread_rwlock_unlock(&rwlock);

  return ret;
}

void rlc::write_pdu(uint32_t lcid, uint8_t *payload, uint32_t nof_bytes)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid(lcid)) {
    rlc_array.at(lcid)->write_pdu(payload, nof_bytes);
  } else {
    rlc_log->warning("LCID %d doesn't exist. Dropping PDU.\n", lcid);
  }
  pthread_rwlock_unlock(&rwlock);
}

// Pass directly to PDCP, no DL througput counting done
void rlc::write_pdu_bcch_bch(uint8_t *payload, uint32_t nof_bytes)
{
  rlc_log->info_hex(payload, nof_bytes, "BCCH BCH message received.");
  byte_buffer_t *buf = pool_allocate;
  if (buf != NULL) {
    memcpy(buf->msg, payload, nof_bytes);
    buf->N_bytes = nof_bytes;
    buf->set_timestamp();
    pdcp->write_pdu_bcch_bch(buf);
  } else {
    rlc_log->error("Fatal error: Out of buffers from the pool in write_pdu_bcch_bch()\n");
  }
}

// Pass directly to PDCP, no DL througput counting done
void rlc::write_pdu_bcch_dlsch(uint8_t *payload, uint32_t nof_bytes)
{
  rlc_log->info_hex(payload, nof_bytes, "BCCH TXSCH message received.");
  byte_buffer_t *buf = pool_allocate;
  if (buf != NULL) {
    memcpy(buf->msg, payload, nof_bytes);
    buf->N_bytes = nof_bytes;
    buf->set_timestamp();
    pdcp->write_pdu_bcch_dlsch(buf);
  } else {
    rlc_log->error("Fatal error: Out of buffers from the pool in write_pdu_bcch_dlsch()\n");
  }
}

// Pass directly to PDCP, no DL througput counting done
void rlc::write_pdu_pcch(uint8_t *payload, uint32_t nof_bytes)
{
  rlc_log->info_hex(payload, nof_bytes, "PCCH message received.");
  byte_buffer_t *buf = pool_allocate;
  if (buf != NULL) {
    memcpy(buf->msg, payload, nof_bytes);
    buf->N_bytes = nof_bytes;
    buf->set_timestamp();
    pdcp->write_pdu_pcch(buf);
  } else {
    rlc_log->error("Fatal error: Out of buffers from the pool in write_pdu_pcch()\n");
  }
}

void rlc::write_pdu_mch(uint32_t lcid, uint8_t *payload, uint32_t nof_bytes)
{
  pthread_rwlock_rdlock(&rwlock);
  if (valid_lcid_mrb(lcid)) {
    rlc_array_mrb.at(lcid)->write_pdu(payload, nof_bytes);
  }
  pthread_rwlock_unlock(&rwlock);
}

/*******************************************************************************
  RRC interface
*******************************************************************************/

// FIXME: Remove function to forbid implicit configuration
void rlc::add_bearer(uint32_t lcid)
{
  if (lcid > 2) {
    rlc_log->error("Radio bearer %s does not support default RLC configuration.\n", rrc->get_rb_name(lcid).c_str());
    return;
  }

  // No config provided - use defaults for SRB0, SRB1, and SRB2
  if (lcid == 0) {
    // SRB0 is TM
    add_bearer(lcid, srslte_rlc_config_t());
  } else {
    // SRB1 and SRB2 are AM
    rlc_cfg_c cnfg;
    cnfg.set(rlc_cfg_c::types::am);
    rlc_cfg_c::am_s_* amcfg            = &cnfg.am();
    amcfg->ul_am_rlc.t_poll_retx       = t_poll_retx_e::ms45;
    amcfg->ul_am_rlc.poll_pdu          = poll_pdu_e::p_infinity;
    amcfg->ul_am_rlc.poll_byte         = poll_byte_e::kbinfinity;
    amcfg->ul_am_rlc.max_retx_thres    = ul_am_rlc_s::max_retx_thres_e_::t4;
    amcfg->dl_am_rlc.t_reordering      = t_reordering_e::ms35;
    amcfg->dl_am_rlc.t_status_prohibit = t_status_prohibit_e::ms0;
    add_bearer(lcid, srslte_rlc_config_t(&cnfg));
  }
}


void rlc::add_bearer(uint32_t lcid, srslte_rlc_config_t cnfg)
{
  pthread_rwlock_wrlock(&rwlock);

  rlc_common *rlc_entity = NULL;

  if (not valid_lcid(lcid)) {
    switch(cnfg.rlc_mode)
    {
      case RLC_MODE_TM:
        rlc_entity = new rlc_tm();
        break;
      case RLC_MODE_AM:
        rlc_entity = new rlc_am();
        break;
      case RLC_MODE_UM:
        rlc_entity = new rlc_um();
        break;
      default:
        rlc_log->error("Cannot add RLC entity - invalid mode\n");
        goto unlock_and_exit;
    }

    // configure and add to array
    rlc_entity->init(rlc_log, lcid, pdcp, rrc, mac_timers);

    if (cnfg.rlc_mode != RLC_MODE_TM) {
      if (rlc_entity->configure(cnfg) == false) {
        rlc_log->error("Error configuring RLC entity\n.");
        goto delete_and_exit;
      }
    }

    if (not rlc_array.insert(rlc_map_pair_t(lcid, rlc_entity)).second) {
      rlc_log->error("Error inserting RLC entity in to array\n.");
      goto delete_and_exit;
    }
    rlc_log->warning("Added radio bearer %s in %s\n", rrc->get_rb_name(lcid).c_str(), rlc_mode_text[cnfg.rlc_mode]);
    goto unlock_and_exit;
  } else {
    rlc_log->warning("Bearer %s already created.\n", rrc->get_rb_name(lcid).c_str());
  }

delete_and_exit:
  if (rlc_entity) {
    delete(rlc_entity);
  }

unlock_and_exit:
  pthread_rwlock_unlock(&rwlock);
}


void rlc::add_bearer_mrb(uint32_t lcid)
{
  pthread_rwlock_wrlock(&rwlock);
  rlc_common *rlc_entity = NULL;

  if (not valid_lcid_mrb(lcid)) {
    rlc_entity = new rlc_um();
    // configure and add to array
    rlc_entity->init(rlc_log, lcid, pdcp, rrc, mac_timers);
    if (not rlc_entity->configure(srslte_rlc_config_t::mch_config())) {
      rlc_log->error("Error configuring RLC entity\n.");
      goto delete_and_exit;
    }
    if (not rlc_array_mrb.insert(rlc_map_pair_t(lcid, rlc_entity)).second) {
      rlc_log->error("Error inserting RLC entity in to array\n.");
      goto delete_and_exit;
    }
    rlc_log->warning("Added radio bearer %s with mode RLC_UM\n", rrc->get_rb_name(lcid).c_str());
    goto unlock_and_exit;
  } else {
    rlc_log->warning("Bearer %s already created.\n", rrc->get_rb_name(lcid).c_str());
  }

delete_and_exit:
  if (rlc_entity != NULL) {
    delete(rlc_entity);
  }

unlock_and_exit:
  pthread_rwlock_unlock(&rwlock);
}


void rlc::del_bearer(uint32_t lcid)
{
  pthread_rwlock_wrlock(&rwlock);

  if (valid_lcid(lcid)) {
    rlc_map_t::iterator it = rlc_array.find(lcid);
    it->second->stop();
    delete(it->second);
    rlc_array.erase(it);
    rlc_log->warning("Deleted RLC bearer %s\n", rrc->get_rb_name(lcid).c_str());
  } else {
    rlc_log->error("Can't delete bearer %s. Bearer doesn't exist.\n", rrc->get_rb_name(lcid).c_str());
  }

  pthread_rwlock_unlock(&rwlock);
}


void rlc::del_bearer_mrb(uint32_t lcid)
{
  pthread_rwlock_wrlock(&rwlock);

  if (valid_lcid_mrb(lcid)) {
    rlc_map_t::iterator it = rlc_array_mrb.find(lcid);
    it->second->stop();
    delete(it->second);
    rlc_array_mrb.erase(it);
    rlc_log->warning("Deleted RLC MRB bearer %s\n", rrc->get_rb_name(lcid).c_str());
  } else {
    rlc_log->error("Can't delete bearer %s. Bearer doesn't exist.\n", rrc->get_rb_name(lcid).c_str());
  }

  pthread_rwlock_unlock(&rwlock);
}


void rlc::change_lcid(uint32_t old_lcid, uint32_t new_lcid)
{
  pthread_rwlock_wrlock(&rwlock);

  // make sure old LCID exists and new LCID is still free
  if (valid_lcid(old_lcid) && not valid_lcid(new_lcid)) {
    // insert old rlc entity into new LCID
    rlc_map_t::iterator it = rlc_array.find(old_lcid);
    rlc_common *rlc_entity = it->second;
    if (not rlc_array.insert(rlc_map_pair_t(new_lcid, rlc_entity)).second) {
      rlc_log->error("Error inserting RLC entity into array\n.");
      goto exit;
    }
    // erase from old position
    rlc_array.erase(it);

    if (valid_lcid(new_lcid) && not valid_lcid(old_lcid)) {
      rlc_log->info("Successfully changed LCID of RLC bearer from %d to %d\n", old_lcid, new_lcid);
    } else {
      rlc_log->error("Error during LCID change of RLC bearer from %d to %d\n", old_lcid, new_lcid);
    }
  } else {
    rlc_log->error("Can't change LCID of bearer %s from %d to %d. Bearer doesn't exist or new LCID already occupied.\n", rrc->get_rb_name(old_lcid).c_str(), old_lcid, new_lcid);
  }
exit:
  pthread_rwlock_unlock(&rwlock);
}


bool rlc::has_bearer(uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  bool ret = valid_lcid(lcid);
  pthread_rwlock_unlock(&rwlock);
  return ret;
}


/*******************************************************************************
  Helpers (Lock must be hold when calling those)
*******************************************************************************/

bool rlc::valid_lcid(uint32_t lcid)
{
  if (lcid >= SRSLTE_N_RADIO_BEARERS) {
    rlc_log->error("Radio bearer id must be in [0:%d] - %d\n", SRSLTE_N_RADIO_BEARERS, lcid);
    return false;
  }

  if (rlc_array.find(lcid) == rlc_array.end()) {
    return false;
  }

  return true;
}

bool rlc::valid_lcid_mrb(uint32_t lcid)
{
  if (lcid >= SRSLTE_N_MCH_LCIDS) {
    rlc_log->error("Radio bearer id must be in [0:%d] - %d\n", SRSLTE_N_RADIO_BEARERS, lcid);
    return false;
  }

  if (rlc_array_mrb.find(lcid) == rlc_array_mrb.end()) {
    return false;
  }

  return true;
}

} // namespace srslte
