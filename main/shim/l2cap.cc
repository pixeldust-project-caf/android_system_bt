/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "bt_shim_l2cap"

#include <cstdint>

#include "main/shim/entry.h"
#include "main/shim/l2cap.h"
#include "main/shim/shim.h"
#include "osi/include/allocator.h"
#include "osi/include/log.h"

constexpr bool kDisconnectResponseRequired = false;
constexpr size_t kBtHdrSize = sizeof(BT_HDR);
constexpr uint16_t kConnectionFail = 1;
constexpr uint16_t kConnectionSuccess = 0;
constexpr uint16_t kInvalidConnectionInterfaceDescriptor = 0;
constexpr uint8_t kUnusedId = 0;
constexpr uint16_t kUnusedResult = 0;

bool bluetooth::legacy::shim::PsmManager::IsPsmRegistered(uint16_t psm) const {
  return psm_to_callback_map_.find(psm) != psm_to_callback_map_.end();
}

bool bluetooth::legacy::shim::PsmManager::HasClient(uint16_t psm) const {
  return IsPsmRegistered(psm) && psm_to_callback_map_.at(psm) != nullptr;
}

void bluetooth::legacy::shim::PsmManager::RegisterPsm(
    uint16_t psm, const tL2CAP_APPL_INFO* callbacks) {
  CHECK(!HasClient(psm));
  psm_to_callback_map_[psm] = callbacks;
}

void bluetooth::legacy::shim::PsmManager::RegisterPsm(uint16_t psm) {
  RegisterPsm(psm, nullptr);
}

void bluetooth::legacy::shim::PsmManager::UnregisterPsm(uint16_t psm) {
  CHECK(IsPsmRegistered(psm));
  psm_to_callback_map_.erase(psm);
}

const tL2CAP_APPL_INFO* bluetooth::legacy::shim::PsmManager::Callbacks(
    uint16_t psm) {
  CHECK(HasClient(psm));
  return psm_to_callback_map_[psm];
}

bluetooth::legacy::shim::L2cap::L2cap()
    : classic_dynamic_psm_(kInitialClassicDynamicPsm),
      le_dynamic_psm_(kInitialLeDynamicPsm),
      classic_virtual_psm_(kInitialClassicVirtualPsm) {}

bluetooth::legacy::shim::PsmManager& bluetooth::legacy::shim::L2cap::Le() {
  return le_;
}

bluetooth::legacy::shim::PsmManager& bluetooth::legacy::shim::L2cap::Classic() {
  return classic_;
}

bool bluetooth::legacy::shim::L2cap::ConnectionExists(uint16_t cid) const {
  return cid_to_psm_map_.find(cid) != cid_to_psm_map_.end();
}

uint16_t bluetooth::legacy::shim::L2cap::CidToPsm(uint16_t cid) const {
  CHECK(ConnectionExists(cid));
  return cid_to_psm_map_.at(cid);
}

uint16_t bluetooth::legacy::shim::L2cap::ConvertClientToRealPsm(
    uint16_t client_psm, bool is_outgoing_only_connection) {
  if (!is_outgoing_only_connection) {
    return client_psm;
  }
  return GetNextVirtualPsm(client_psm);
}

uint16_t bluetooth::legacy::shim::L2cap::ConvertClientToRealPsm(
    uint16_t client_psm) {
  if (client_psm_to_real_psm_map_.find(client_psm) ==
      client_psm_to_real_psm_map_.end()) {
    return client_psm;
  }
  return client_psm_to_real_psm_map_.at(client_psm);
}

void bluetooth::legacy::shim::L2cap::RemoveClientPsm(uint16_t client_psm) {
  if (client_psm_to_real_psm_map_.find(client_psm) !=
      client_psm_to_real_psm_map_.end()) {
    client_psm_to_real_psm_map_.erase(client_psm);
  }
}

uint16_t bluetooth::legacy::shim::L2cap::GetNextVirtualPsm(uint16_t real_psm) {
  if (real_psm < kInitialClassicDynamicPsm) {
    return real_psm;
  }

  while (Classic().IsPsmRegistered(classic_virtual_psm_)) {
    classic_virtual_psm_ += 2;
    if (classic_virtual_psm_ >= kFinalClassicVirtualPsm) {
      classic_virtual_psm_ = kInitialClassicVirtualPsm;
    }
  }
  return classic_virtual_psm_;
}

uint16_t bluetooth::legacy::shim::L2cap::GetNextDynamicLePsm() {
  while (Le().IsPsmRegistered(le_dynamic_psm_)) {
    le_dynamic_psm_++;
    if (le_dynamic_psm_ > kFinalLeDynamicPsm) {
      le_dynamic_psm_ = kInitialLeDynamicPsm;
    }
  }
  return le_dynamic_psm_;
}

uint16_t bluetooth::legacy::shim::L2cap::GetNextDynamicClassicPsm() {
  while (Classic().IsPsmRegistered(classic_dynamic_psm_)) {
    classic_dynamic_psm_ += 2;
    if (classic_dynamic_psm_ > kFinalClassicDynamicPsm) {
      classic_dynamic_psm_ = kInitialClassicDynamicPsm;
    } else if (classic_dynamic_psm_ & 0x0100) {
      /* the upper byte must be even */
      classic_dynamic_psm_ += 0x0100;
    }

    /* if psm is in range of reserved BRCM Aware features */
    if ((BRCM_RESERVED_PSM_START <= classic_dynamic_psm_) &&
        (classic_dynamic_psm_ <= BRCM_RESERVED_PSM_END)) {
      classic_dynamic_psm_ = BRCM_RESERVED_PSM_END + 2;
    }
  }
  return classic_dynamic_psm_;
}

void bluetooth::legacy::shim::L2cap::RegisterService(
    uint16_t psm, const tL2CAP_APPL_INFO* callbacks, bool enable_snoop,
    tL2CAP_ERTM_INFO* p_ertm_info) {
  LOG_DEBUG(LOG_TAG, "Registering service on psm:%hd", psm);

  if (!enable_snoop) {
    LOG_WARN(LOG_TAG, "UNIMPLEMENTED Cannot disable snooping on psm:%d", psm);
  }
  Classic().RegisterPsm(psm, callbacks);

  std::promise<void> register_completed;
  auto completed = register_completed.get_future();
  bool use_ertm = false;
  if (p_ertm_info != nullptr &&
      p_ertm_info->preferred_mode == L2CAP_FCR_ERTM_MODE) {
    use_ertm = true;
  }
  constexpr auto mtu = 1000;  // TODO: Let client decide
  bluetooth::shim::GetL2cap()->RegisterService(
      psm, use_ertm, mtu,
      std::bind(
          &bluetooth::legacy::shim::L2cap::OnRemoteInitiatedConnectionCreated,
          this, std::placeholders::_1, std::placeholders::_2,
          std::placeholders::_3),
      std::move(register_completed));
  completed.wait();
  LOG_DEBUG(LOG_TAG, "Successfully registered service on psm:%hd", psm);
}

void bluetooth::legacy::shim::L2cap::OnRemoteInitiatedConnectionCreated(
    std::string string_address, uint16_t psm, uint16_t cid) {
  RawAddress raw_address;
  RawAddress::FromString(string_address, raw_address);

  LOG_DEBUG(LOG_TAG,
            "Sending connection indicator to upper stack from device:%s "
            "psm:%hd cid:%hd",
            string_address.c_str(), psm, cid);

  CHECK(!ConnectionExists(cid));
  cid_to_psm_map_[cid] = psm;
  SetDownstreamCallbacks(cid);
  Classic().Callbacks(psm)->pL2CA_ConnectInd_Cb(raw_address, cid, psm,
                                                kUnusedId);
}

void bluetooth::legacy::shim::L2cap::UnregisterService(uint16_t psm) {
  if (!Classic().IsPsmRegistered(psm)) {
    LOG_WARN(LOG_TAG,
             "Service must be registered in order to unregister psm:%hd", psm);
    return;
  }
  for (auto& entry : cid_to_psm_map_) {
    if (entry.second == psm) {
      LOG_WARN(LOG_TAG,
               "  Unregistering service with active channels psm:%hd cid:%hd",
               psm, entry.first);
    }
  }

  LOG_DEBUG(LOG_TAG, "Unregistering service on psm:%hd", psm);
  bluetooth::shim::GetL2cap()->UnregisterService(psm);
  Classic().UnregisterPsm(psm);
}

uint16_t bluetooth::legacy::shim::L2cap::CreateConnection(
    uint16_t psm, const RawAddress& raw_address) {
  if (!Classic().IsPsmRegistered(psm)) {
    LOG_WARN(LOG_TAG, "Service must be registered in order to connect psm:%hd",
             psm);
    return kInvalidConnectionInterfaceDescriptor;
  }

  std::promise<uint16_t> connect_completed;
  auto completed = connect_completed.get_future();
  LOG_DEBUG(LOG_TAG,
            "Starting local initiated connection to psm:%hd address:%s", psm,
            raw_address.ToString().c_str());

  bluetooth::shim::GetL2cap()->CreateConnection(
      psm, raw_address.ToString(),
      std::bind(
          &bluetooth::legacy::shim::L2cap::OnLocalInitiatedConnectionCreated,
          this, std::placeholders::_1, std::placeholders::_2,
          std::placeholders::_3),
      std::move(connect_completed));

  uint16_t cid = completed.get();
  if (cid == kInvalidConnectionInterfaceDescriptor) {
    LOG_WARN(LOG_TAG,
             "Failed to allocate resources to connect to psm:%hd address:%s",
             psm, raw_address.ToString().c_str());
  } else {
    LOG_DEBUG(LOG_TAG,
              "Successfully started connection to psm:%hd address:%s"
              " connection_interface_descriptor:%hd",
              psm, raw_address.ToString().c_str(), cid);
    CHECK(!ConnectionExists(cid));
    cid_to_psm_map_[cid] = psm;
  }
  return cid;
}

void bluetooth::legacy::shim::L2cap::OnLocalInitiatedConnectionCreated(
    std::string string_address, uint16_t psm, uint16_t cid) {
  LOG_DEBUG(LOG_TAG,
            "Sending connection confirm to the upper stack but really "
            "a connection to %s has already been done cid:%hd",
            string_address.c_str(), cid);
  uint16_t status = kConnectionFail;
  if (cid != kInvalidConnectionInterfaceDescriptor) {
    SetDownstreamCallbacks(cid);
    status = kConnectionSuccess;
  }
  Classic().Callbacks(psm)->pL2CA_ConnectCfm_Cb(cid, status);
}

bool bluetooth::legacy::shim::L2cap::Write(uint16_t cid, BT_HDR* bt_hdr) {
  CHECK(bt_hdr != nullptr);
  const uint8_t* data = bt_hdr->data + bt_hdr->offset;
  size_t len = bt_hdr->len;
  if (!ConnectionExists(cid) || len == 0) {
    return false;
  }
  LOG_DEBUG(LOG_TAG, "Writing data cid:%hd len:%zd", cid, len);
  bluetooth::shim::GetL2cap()->Write(cid, data, len);
  return true;
}

void bluetooth::legacy::shim::L2cap::SetDownstreamCallbacks(uint16_t cid) {
  bluetooth::shim::GetL2cap()->SetReadDataReadyCallback(
      cid, [this](uint16_t cid, std::vector<const uint8_t> data) {
        LOG_DEBUG(LOG_TAG, "OnDataReady cid:%hd len:%zd", cid, data.size());
        BT_HDR* bt_hdr =
            static_cast<BT_HDR*>(osi_calloc(data.size() + kBtHdrSize));
        std::copy(data.begin(), data.end(), bt_hdr->data);
        bt_hdr->len = data.size();
        classic_.Callbacks(CidToPsm(cid))->pL2CA_DataInd_Cb(cid, bt_hdr);
      });

  bluetooth::shim::GetL2cap()->SetConnectionClosedCallback(
      cid, [this](uint16_t cid, int error_code) {
        LOG_DEBUG(LOG_TAG, "OnChannel closed callback cid:%hd", cid);
        if (!ConnectionExists(cid)) {
          LOG_WARN(LOG_TAG, "%s Unexpected channel closure cid:%hd", __func__,
                   cid);
          return;
        }
        if (cid_closing_set_.count(cid) == 1) {
          cid_closing_set_.erase(cid);
          classic_.Callbacks(CidToPsm(cid))
              ->pL2CA_DisconnectCfm_Cb(cid, kUnusedResult);
        } else {
          classic_.Callbacks(CidToPsm(cid))
              ->pL2CA_DisconnectInd_Cb(cid, kDisconnectResponseRequired);
        }
        cid_to_psm_map_.erase(cid);
      });
}

bool bluetooth::legacy::shim::L2cap::ConnectResponse(
    const RawAddress& raw_address, uint8_t signal_id, uint16_t cid,
    uint16_t result, uint16_t status, tL2CAP_ERTM_INFO* ertm_info) {
  CHECK(ConnectionExists(cid));
  LOG_DEBUG(LOG_TAG,
            "%s Silently dropping client connect response as channel is "
            "already connected",
            __func__);
  return true;
}

bool bluetooth::legacy::shim::L2cap::ConfigRequest(
    uint16_t cid, const tL2CAP_CFG_INFO* config_info) {
  LOG_INFO(LOG_TAG, "Received config request from upper layer cid:%hd", cid);
  CHECK(ConnectionExists(cid));

  bluetooth::shim::GetL2cap()->SendLoopbackResponse([this, cid]() {
    CHECK(ConnectionExists(cid));
    tL2CAP_CFG_INFO cfg_info{
        .result = L2CAP_CFG_OK,
        .mtu_present = false,
        .qos_present = false,
        .flush_to_present = false,
        .fcr_present = false,
        .fcs_present = false,
        .ext_flow_spec_present = false,
        .flags = 0,
    };
    classic_.Callbacks(CidToPsm(cid))->pL2CA_ConfigCfm_Cb(cid, &cfg_info);
    classic_.Callbacks(CidToPsm(cid))->pL2CA_ConfigInd_Cb(cid, &cfg_info);
  });
  return true;
}

bool bluetooth::legacy::shim::L2cap::ConfigResponse(
    uint16_t cid, const tL2CAP_CFG_INFO* config_info) {
  CHECK(ConnectionExists(cid));
  LOG_DEBUG(
      LOG_TAG,
      "%s Silently dropping client config response as channel is already open",
      __func__);
  return true;
}

bool bluetooth::legacy::shim::L2cap::DisconnectRequest(uint16_t cid) {
  CHECK(ConnectionExists(cid));
  if (cid_closing_set_.find(cid) != cid_closing_set_.end()) {
    LOG_WARN(LOG_TAG, "%s Channel already in closing state cid:%hu", __func__,
             cid);
    return false;
  }
  LOG_DEBUG(LOG_TAG, "%s cid:%hu", __func__, cid);
  cid_closing_set_.insert(cid);
  bluetooth::shim::GetL2cap()->CloseConnection(cid);
  return true;
}

bool bluetooth::legacy::shim::L2cap::DisconnectResponse(uint16_t cid) {
  LOG_DEBUG(LOG_TAG,
            "%s Silently dropping client disconnect response as channel is "
            "already disconnected",
            __func__);
  return true;
}

void bluetooth::legacy::shim::L2cap::Dump(int fd) {
  if (cid_to_psm_map_.empty()) {
    dprintf(fd, "%s No active l2cap channels\n", "gd::shim::legacy::l2cap");
  } else {
    for (auto& connection : cid_to_psm_map_) {
      dprintf(fd, "%s active l2cap channel cid:%hd psm:%hd\n",
              "gd::shim::legacy::l2cap", connection.first, connection.second);
    }
  }
}