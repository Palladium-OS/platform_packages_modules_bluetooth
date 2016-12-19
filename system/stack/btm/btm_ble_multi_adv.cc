/******************************************************************************
 *
 *  Copyright (C) 2014  Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#include <base/bind.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <string.h>
#include <queue>
#include <vector>

#include "bt_target.h"
#include "device/include/controller.h"
#include "osi/include/alarm.h"

#include "ble_advertiser.h"
#include "ble_advertiser_hci_interface.h"
#include "btm_int_types.h"

using base::Bind;
using multiadv_cb = base::Callback<void(uint8_t /* status */)>;

struct AdvertisingInstance {
  uint8_t inst_id;
  bool in_use;
  uint8_t adv_evt;
  BD_ADDR rpa;
  alarm_t* adv_raddr_timer;
  int8_t tx_power;
  int timeout_s;
  MultiAdvCb timeout_cb;
  alarm_t* timeout_timer;
  AdvertisingInstance(int inst_id)
      : inst_id(inst_id),
        in_use(false),
        adv_evt(0),
        rpa{0},
        tx_power(0),
        timeout_s(0),
        timeout_cb(),
        timeout_timer(nullptr) {
    adv_raddr_timer = alarm_new_periodic("btm_ble.adv_raddr_timer");
  }

  ~AdvertisingInstance() {
    alarm_free(adv_raddr_timer);
    if (timeout_timer) alarm_free(timeout_timer);
  }
};

/*******************************************************************************
 *  Externs
 ******************************************************************************/
extern fixed_queue_t* btu_general_alarm_queue;

void DoNothing(uint8_t) {}

std::queue<base::Callback<void(tBTM_RAND_ENC* p)>>* rand_gen_inst_id = nullptr;

/* RPA generation completion callback for each adv instance. Will continue write
 * the new RPA into controller. */
void btm_ble_multi_adv_gen_rpa_cmpl(tBTM_RAND_ENC* p) {
  /* Retrieve the index of adv instance from stored Q */
  base::Callback<void(tBTM_RAND_ENC * p)> cb = rand_gen_inst_id->front();
  rand_gen_inst_id->pop();
  cb.Run(p);
}

void btm_ble_adv_raddr_timer_timeout(void* data);

class BleAdvertisingManagerImpl
    : public BleAdvertisingManager,
      public BleAdvertiserHciInterface::AdvertisingEventObserver {
 public:
  BleAdvertisingManagerImpl(BleAdvertiserHciInterface* interface) {
    this->hci_interface = interface;
    hci_interface->ReadInstanceCount(
        base::Bind(&BleAdvertisingManagerImpl::ReadInstanceCountCb,
                   base::Unretained(this)));
  }

  ~BleAdvertisingManagerImpl() { adv_inst.clear(); }

  void ReadInstanceCountCb(uint8_t instance_count) {
    this->inst_count = instance_count;
    adv_inst.reserve(inst_count);
    /* Initialize adv instance indices and IDs. */
    for (uint8_t i = 0; i < inst_count; i++) {
      adv_inst.emplace_back(i);
    }
  }

  void OnRpaGenerationComplete(uint8_t inst_id, tBTM_RAND_ENC* p) {
    LOG(INFO) << "inst_id = " << +inst_id;

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    if (!p) return;

    p->param_buf[2] &= (~BLE_RESOLVE_ADDR_MASK);
    p->param_buf[2] |= BLE_RESOLVE_ADDR_MSB;

    p_inst->rpa[2] = p->param_buf[0];
    p_inst->rpa[1] = p->param_buf[1];
    p_inst->rpa[0] = p->param_buf[2];

    BT_OCTET16 irk;
    BTM_GetDeviceIDRoot(irk);
    tSMP_ENC output;

    if (!SMP_Encrypt(irk, BT_OCTET16_LEN, p->param_buf, 3, &output))
      LOG_ASSERT(false) << "SMP_Encrypt failed";

    /* set hash to be LSB of rpAddress */
    p_inst->rpa[5] = output.param_buf[0];
    p_inst->rpa[4] = output.param_buf[1];
    p_inst->rpa[3] = output.param_buf[2];

    /* set it to controller */
    GetHciInterface()->SetRandomAddress(p_inst->rpa, p_inst->inst_id,
                                        Bind(DoNothing));
  }

  void ConfigureRpa(uint8_t inst_id) {
    if (rand_gen_inst_id == nullptr)
      rand_gen_inst_id =
          new std::queue<base::Callback<void(tBTM_RAND_ENC * p)>>();

    rand_gen_inst_id->push(
        Bind(&BleAdvertisingManagerImpl::OnRpaGenerationComplete,
             base::Unretained(this), inst_id));
    btm_gen_resolvable_private_addr((void*)btm_ble_multi_adv_gen_rpa_cmpl);
  }

  void RegisterAdvertiser(
      base::Callback<void(uint8_t /* inst_id */, uint8_t /* status */)> cb)
      override {
    AdvertisingInstance* p_inst = &adv_inst[0];
    for (uint8_t i = 0; i < inst_count; i++, p_inst++) {
      if (p_inst->in_use) continue;

      p_inst->in_use = true;

#if (BLE_PRIVACY_SPT == TRUE)
      // configure the address, and set up periodic timer to update it.
      ConfigureRpa(p_inst->inst_id);

      if (BTM_BleLocalPrivacyEnabled()) {
        alarm_set_on_queue(p_inst->adv_raddr_timer, BTM_BLE_PRIVATE_ADDR_INT_MS,
                           btm_ble_adv_raddr_timer_timeout, p_inst,
                           btu_general_alarm_queue);
      }
#endif

      cb.Run(p_inst->inst_id, BTM_BLE_MULTI_ADV_SUCCESS);
      return;
    }

    LOG(INFO) << "no free advertiser instance";
    cb.Run(0xFF, ADVERTISE_FAILED_TOO_MANY_ADVERTISERS);
  }

  void StartAdvertising(uint8_t advertiser_id, MultiAdvCb cb,
                        tBTM_BLE_ADV_PARAMS* params,
                        std::vector<uint8_t> advertise_data,
                        std::vector<uint8_t> scan_response_data, int timeout_s,
                        MultiAdvCb timeout_cb) override {
    /* a temporary type for holding all the data needed in callbacks below*/
    struct CreatorParams {
      uint8_t inst_id;
      BleAdvertisingManagerImpl* self;
      MultiAdvCb cb;
      tBTM_BLE_ADV_PARAMS params;
      std::vector<uint8_t> advertise_data;
      std::vector<uint8_t> scan_response_data;
      int timeout_s;
      MultiAdvCb timeout_cb;
    };

    std::unique_ptr<CreatorParams> c;
    c.reset(new CreatorParams());

    c->self = this;
    c->cb = std::move(cb);
    c->params = *params;
    c->advertise_data = std::move(advertise_data);
    c->scan_response_data = std::move(scan_response_data);
    c->timeout_s = timeout_s;
    c->timeout_cb = std::move(timeout_cb);
    c->inst_id = advertiser_id;

    using c_type = std::unique_ptr<CreatorParams>;

    // this code is intentionally left formatted this way to highlight the
    // asynchronous flow
    // clang-format off
    c->self->SetParameters(c->inst_id, &c->params, Bind(
      [](c_type c, uint8_t status) {
        if (status != 0) {
          LOG(ERROR) << "setting parameters failed, status: " << +status;
          c->cb.Run(status);
          return;
        }

        c->self->SetData(c->inst_id, false, std::move(c->advertise_data), Bind(
          [](c_type c, uint8_t status) {
            if (status != 0) {
              LOG(ERROR) << "setting advertise data failed, status: " << +status;
              c->cb.Run(status);
              return;
            }

            c->self->SetData(c->inst_id, true, std::move(c->scan_response_data), Bind(
              [](c_type c, uint8_t status) {
                if (status != 0) {
                  LOG(ERROR) << "setting scan response data failed, status: " << +status;
                  c->cb.Run(status);
                  return;
                }

                c->self->Enable(c->inst_id, true, c->cb, c->timeout_s, c->timeout_cb);

            }, base::Passed(&c)));
        }, base::Passed(&c)));
      }, base::Passed(&c)));
    // clang-format on
  }

  void EnableWithTimerCb(uint8_t inst_id, int timeout_s, MultiAdvCb timeout_cb,
                         uint8_t status) {
    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    p_inst->timeout_s = timeout_s;
    p_inst->timeout_cb = std::move(timeout_cb);

    p_inst->timeout_timer = alarm_new("btm_ble.adv_timeout");
    alarm_set_on_queue(p_inst->timeout_timer, p_inst->timeout_s * 1000, nullptr,
                       p_inst, btu_general_alarm_queue);
  }

  void Enable(uint8_t inst_id, bool enable, MultiAdvCb cb, int timeout_s,
              MultiAdvCb timeout_cb) {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    VLOG(1) << __func__ << "enable: " << enable;
    if (!p_inst->in_use) {
      LOG(ERROR) << "Invalid or no active instance";
      cb.Run(BTM_BLE_MULTI_ADV_FAILURE);
      return;
    }

    if (enable && timeout_s) {
      GetHciInterface()->Enable(
          enable, p_inst->inst_id,
          Bind(&BleAdvertisingManagerImpl::EnableWithTimerCb,
               base::Unretained(this), inst_id, timeout_s, timeout_cb));
    } else {
      if (p_inst->timeout_timer) {
        alarm_cancel(p_inst->timeout_timer);
        alarm_free(p_inst->timeout_timer);
        p_inst->timeout_timer = nullptr;
      }

      GetHciInterface()->Enable(enable, p_inst->inst_id, cb);
    }
  }

  void SetParameters(uint8_t inst_id, tBTM_BLE_ADV_PARAMS* p_params,
                     MultiAdvCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    if (!p_inst->in_use) {
      LOG(ERROR) << "adv instance not in use" << +inst_id;
      cb.Run(BTM_BLE_MULTI_ADV_FAILURE);
      return;
    }

    // TODO: disable only if was enabled, currently no use scenario needs that,
    // we always set parameters before enabling
    // GetHciInterface()->Enable(false, inst_id, Bind(DoNothing));

    uint8_t own_address_type = BLE_ADDR_PUBLIC;
    BD_ADDR own_address;

#if (BLE_PRIVACY_SPT == TRUE)
    if (BTM_BleLocalPrivacyEnabled()) {
      own_address_type = BLE_ADDR_RANDOM;
      memcpy(own_address, p_inst->rpa, BD_ADDR_LEN);
    } else {
#else
    {
#endif
      memcpy(own_address, controller_get_interface()->get_address()->address,
             BD_ADDR_LEN);
    }

    BD_ADDR dummy = {0, 0, 0, 0, 0, 0};

    p_inst->adv_evt = p_params->adv_type;
    p_inst->tx_power = p_params->tx_power;

    GetHciInterface()->SetParameters(
        p_params->adv_int_min, p_params->adv_int_max, p_params->adv_type,
        own_address_type, own_address, 0, dummy, p_params->channel_map,
        p_params->adv_filter_policy, p_inst->inst_id, p_inst->tx_power, cb);

    // TODO: re-enable only if it was enabled, properly call
    // SetParamsCallback
    // currently no use scenario needs that
    // GetHciInterface()->Enable(true, inst_id, BTM_BleUpdateAdvInstParamCb);
  }

  void SetData(uint8_t inst_id, bool is_scan_rsp, std::vector<uint8_t> data,
               MultiAdvCb cb) override {
    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    VLOG(1) << "is_scan_rsp = " << is_scan_rsp;

    if (!is_scan_rsp && p_inst->adv_evt != BTM_BLE_NON_CONNECT_EVT) {
      uint8_t flags_val = BTM_GENERAL_DISCOVERABLE;

      if (p_inst->timeout_s) flags_val = BTM_LIMITED_DISCOVERABLE;

      std::vector<uint8_t> flags;
      flags.push_back(2);  // length
      flags.push_back(HCI_EIR_FLAGS_TYPE);
      flags.push_back(flags_val);

      data.insert(data.begin(), flags.begin(), flags.end());
    }

    // Find and fill TX Power with the correct value
    if (data.size()) {
      size_t i = 0;
      while (i < data.size()) {
        uint8_t type = data[i + 1];
        if (type == HCI_EIR_TX_POWER_LEVEL_TYPE) {
          data[i + 2] = adv_inst[inst_id].tx_power;
        }
        i += data[i] + 1;
      }
    }

    VLOG(1) << "data is: " << base::HexEncode(data.data(), data.size());

    if (is_scan_rsp) {
      GetHciInterface()->SetScanResponseData(data.size(), data.data(), inst_id,
                                             cb);
    } else {
      GetHciInterface()->SetAdvertisingData(data.size(), data.data(), inst_id,
                                            cb);
    }
  }

  void Unregister(uint8_t inst_id) override {
    AdvertisingInstance* p_inst = &adv_inst[inst_id];

    VLOG(1) << __func__ << " inst_id: " << +inst_id;
    if (inst_id >= inst_count) {
      LOG(ERROR) << "bad instance id " << +inst_id;
      return;
    }

    // TODO(jpawlowski): only disable when enabled or enabling
    GetHciInterface()->Enable(false, inst_id, Bind(DoNothing));

    alarm_cancel(p_inst->adv_raddr_timer);
    p_inst->in_use = false;
  }

  void OnAdvertisingStateChanged(uint8_t inst_id, uint8_t reason,
                                 uint16_t conn_handle) override {
    AdvertisingInstance* p_inst = &adv_inst[inst_id];
    VLOG(1) << __func__ << " inst_id: 0x" << std::hex << +inst_id
            << ", reason: 0x" << std::hex << +reason << ", conn_handle: 0x"
            << std::hex << +conn_handle;

#if (BLE_PRIVACY_SPT == TRUE)
    if (BTM_BleLocalPrivacyEnabled() && inst_id <= BTM_BLE_MULTI_ADV_MAX) {
      btm_acl_update_conn_addr(conn_handle, p_inst->rpa);
    }
#endif

    VLOG(1) << "reneabling advertising";

    if (p_inst->in_use == true) {
      // TODO(jpawlowski): we don't really allow to do directed advertising
      // right now. This should probably be removed, check with Andre.
      if (p_inst->adv_evt != BTM_BLE_CONNECT_DIR_EVT) {
        GetHciInterface()->Enable(true, inst_id, Bind(DoNothing));
      } else {
        /* mark directed adv as disabled if adv has been stopped */
        p_inst->in_use = false;
      }
    }
  }

 private:
  BleAdvertiserHciInterface* GetHciInterface() { return hci_interface; }

  BleAdvertiserHciInterface* hci_interface = nullptr;
  std::vector<AdvertisingInstance> adv_inst;
  uint8_t inst_count;
};

namespace {
BleAdvertisingManager* instance;
}

void BleAdvertisingManager::Initialize(BleAdvertiserHciInterface* interface) {
  instance = new BleAdvertisingManagerImpl(interface);
}

BleAdvertisingManager* BleAdvertisingManager::Get() {
  CHECK(instance);
  return instance;
};

void BleAdvertisingManager::CleanUp() {
  delete instance;
  instance = nullptr;
};

void btm_ble_adv_raddr_timer_timeout(void* data) {
  ((BleAdvertisingManagerImpl*)BleAdvertisingManager::Get())
      ->ConfigureRpa(((AdvertisingInstance*)data)->inst_id);
}

/*******************************************************************************
 *
 * Function         btm_ble_multi_adv_init
 *
 * Description      This function initialize the multi adv control block.
 *
 * Parameters       None
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_multi_adv_init() {
  BleAdvertiserHciInterface::Initialize();
  BleAdvertisingManager::Initialize(BleAdvertiserHciInterface::Get());
  BleAdvertiserHciInterface::Get()->SetAdvertisingEventObserver(
      (BleAdvertisingManagerImpl*)BleAdvertisingManager::Get());
}

/*******************************************************************************
 *
 * Function         btm_ble_multi_adv_cleanup
 *
 * Description      This function cleans up multi adv control block.
 *
 * Parameters
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_multi_adv_cleanup(void) {
  BleAdvertisingManager::CleanUp();
  BleAdvertiserHciInterface::CleanUp();
}
