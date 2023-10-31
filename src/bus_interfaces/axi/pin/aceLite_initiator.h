/*******************************************************************************
 * Copyright 2021-2022 MINRES Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/

#ifndef _BUS_AXI_PIN_ACELITE_INITIATOR_H_
#define _BUS_AXI_PIN_ACELITE_INITIATOR_H_

#include <axi/axi_tlm.h>
#include <axi/fsm/base.h>
#include <axi/fsm/protocol_fsm.h>
#include <axi/signal_if.h>
#include <systemc>
#include <tlm_utils/peq_with_cb_and_phase.h>
#include <tlm/scc/tlm_mm.h>

//! TLM2.0 components modeling AHB
namespace axi {
//! pin level adapters
namespace pin {

using namespace axi::fsm;
namespace acel {
const sc_core::sc_time CLK_DELAY=1_ps;
}

template <typename CFG>
struct aceLite_initiator : public sc_core::sc_module,
                    public aw_ch_aceLite<CFG, typename CFG::master_types>,
                    public wdata_ch_aceLite<CFG, typename CFG::master_types>,
                    public b_ch_aceLite<CFG, typename CFG::master_types>,
                    public ar_ch_aceLite<CFG, typename CFG::master_types>,
                    public rresp_ch_aceLite<CFG, typename CFG::master_types>,
                    protected axi::fsm::base,
                    public axi::ace_fw_transport_if<axi::axi_protocol_types> {
    SC_HAS_PROCESS(aceLite_initiator);

    using payload_type = axi::axi_protocol_types::tlm_payload_type;
    using phase_type = axi::axi_protocol_types::tlm_phase_type;

    sc_core::sc_in<bool> clk_i{"clk_i"};

    axi::axi_target_socket<CFG::BUSWIDTH> tsckt{"tsckt"};

    aceLite_initiator(sc_core::sc_module_name const& nm)
    : sc_core::sc_module(nm)
    // coherent= true
    , base(CFG::BUSWIDTH, true) {
        instance_name = name();
        tsckt(*this);
        SC_METHOD(clk_delay);
        sensitive << clk_i.pos();
        SC_THREAD(ar_t);
        SC_THREAD(r_t);
        SC_THREAD(aw_t);
        SC_THREAD(wdata_t);
        SC_THREAD(b_t);
    }

private:
    void b_transport(payload_type& trans, sc_core::sc_time& t) override {
        trans.set_dmi_allowed(false);
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    tlm::tlm_sync_enum nb_transport_fw(payload_type& trans, phase_type& phase, sc_core::sc_time& t) override {
        assert(trans.get_extension<axi::ace_extension>() && "missing ACE extension");
        sc_core::sc_time delay; // FIXME: calculate delay correctly
        fw_peq.notify(trans, phase, delay);
        return tlm::TLM_ACCEPTED;
    }

    bool get_direct_mem_ptr(payload_type& trans, tlm::tlm_dmi& dmi_data) override {
        trans.set_dmi_allowed(false);
        return false;
    }

    unsigned int transport_dbg(payload_type& trans) override { return 0; }

    void end_of_elaboration() override { clk_if = dynamic_cast<sc_core::sc_clock*>(clk_i.get_interface()); }

    fsm_handle* create_fsm_handle() { return new fsm_handle(); }

    void setup_callbacks(fsm_handle* fsm_hndl);

    void clk_delay() {
        clk_delayed.notify(acel::CLK_DELAY);
    }

    void ar_t();
    void r_t();
    void aw_t();
    void wdata_t();
    void b_t();
/**
 * @fn CFG::data_t get_cache_data_for_beat(fsm::fsm_handle*)
 * @brief
 *
 * @param fsm_hndl
 * @return
 */
    static typename CFG::data_t get_cache_data_for_beat(fsm::fsm_handle* fsm_hndl);
    std::array<unsigned, 3> outstanding_cnt;
    std::array<fsm_handle*, 3> active_req;
    std::array<fsm_handle*, 3> active_resp;
    sc_core::sc_clock* clk_if;
    sc_core::sc_event clk_delayed, clk_self, r_end_req_evt, aw_evt, ar_evt;
       void nb_fw(payload_type& trans, const phase_type& phase) {
        auto t = sc_core::SC_ZERO_TIME;
        base::nb_fw(trans, phase, t);
    }
    tlm_utils::peq_with_cb_and_phase<aceLite_initiator> fw_peq{this, &aceLite_initiator::nb_fw};
    std::unordered_map<unsigned, std::deque<fsm_handle*>> rd_resp_by_id, wr_resp_by_id;
    sc_core::sc_buffer<uint8_t> wdata_vl;
    void write_ar(tlm::tlm_generic_payload& trans);
    void write_aw(tlm::tlm_generic_payload& trans);
    void write_wdata(tlm::tlm_generic_payload& trans, unsigned beat, bool last = false);
};

} // namespace pin
} // namespace axi

template <typename CFG> inline void axi::pin::aceLite_initiator<CFG>::write_ar(tlm::tlm_generic_payload& trans) {
    sc_dt::sc_uint<CFG::ADDRWIDTH> addr = trans.get_address();
    this->ar_addr.write(addr);
    if(auto ext = trans.get_extension<axi::ace_extension>()) {
        this->ar_id->write(sc_dt::sc_uint<CFG::IDWIDTH>(ext->get_id()));
        this->ar_len->write(sc_dt::sc_uint<8>(ext->get_length()));
        this->ar_size->write(sc_dt::sc_uint<3>(ext->get_size()));
        this->ar_burst->write(sc_dt::sc_uint<2>(axi::to_int(ext->get_burst())));
     // TBD??   this->ar_lock->write(ext->get_lock());
        this->ar_cache->write(sc_dt::sc_uint<4>(ext->get_cache()));
        this->ar_prot.write(ext->get_prot());
        this->ar_qos->write(ext->get_qos());
        this->ar_region->write(ext->get_region());
        this->ar_domain->write(sc_dt::sc_uint<2>((uint8_t)ext->get_domain()));
        this->ar_snoop->write(sc_dt::sc_uint<4>((uint8_t)ext->get_snoop()));
        this->ar_bar->write(sc_dt::sc_uint<2>((uint8_t)ext->get_barrier()));
        this->ar_user->write(ext->get_user(axi::common::id_type::CTRL));
    }
}

template <typename CFG> inline void axi::pin::aceLite_initiator<CFG>::write_aw(tlm::tlm_generic_payload& trans) {
    sc_dt::sc_uint<CFG::ADDRWIDTH> addr = trans.get_address();
    this->aw_addr.write(addr);
    if(auto ext = trans.get_extension<axi::ace_extension>()) {
        this->aw_prot.write(ext->get_prot());
        //TBD??   this->aw_lock.write();
        if(this->aw_id.get_interface())
            this->aw_id->write(sc_dt::sc_uint<CFG::IDWIDTH>(ext->get_id()));
        this->aw_len->write(sc_dt::sc_uint<8>(ext->get_length()));
        this->aw_size->write(sc_dt::sc_uint<3>(ext->get_size()));
        this->aw_burst->write(sc_dt::sc_uint<2>(axi::to_int(ext->get_burst())));
        this->aw_cache->write(sc_dt::sc_uint<4>(ext->get_cache()));
        this->aw_qos->write(sc_dt::sc_uint<4>(ext->get_qos()));
        this->aw_region->write(sc_dt::sc_uint<4>(ext->get_region()));
        this->aw_user->write(ext->get_user(axi::common::id_type::CTRL));
        this->aw_domain->write(sc_dt::sc_uint<2>((uint8_t)ext->get_domain()));
        this->aw_snoop->write(sc_dt::sc_uint<CFG::SNOOPWIDTH>((uint8_t)ext->get_snoop()));
        this->aw_bar->write(sc_dt::sc_uint<2>((uint8_t)ext->get_barrier()));
        /* aceLite doe not have unique* */
       // this->aw_unique->write(ext->get_unique());
        if(ext->is_stash_nid_en()) {
            this->aw_stashniden->write(true);
            this->aw_stashnid->write(sc_dt::sc_uint<11>(ext->get_stash_nid()));
        }
        if(ext->is_stash_lpid_en()){
            this->aw_stashlpiden->write(true);
            this->aw_stashlpid->write(sc_dt::sc_uint<5>(ext->get_stash_lpid()));
        }
    }
}

// FIXME: strb not yet correct
template <typename CFG>
inline void axi::pin::aceLite_initiator<CFG>::write_wdata(tlm::tlm_generic_payload& trans, unsigned beat, bool last) {
    typename CFG::data_t data{0};
    sc_dt::sc_uint<CFG::BUSWIDTH / 8> strb{0};
    auto ext = trans.get_extension<axi::ace_extension>();
    auto size = 1u << ext->get_size();
    auto byte_offset = beat * size;
    auto offset = (trans.get_address()+byte_offset) & (CFG::BUSWIDTH / 8 - 1);
    auto beptr = trans.get_byte_enable_length() ? trans.get_byte_enable_ptr() + byte_offset : nullptr;
    if(offset && (size + offset) > (CFG::BUSWIDTH / 8)) { // un-aligned multi-beat access
        if(beat == 0) {
            auto dptr = trans.get_data_ptr();
            for(size_t i = offset; i < size; ++i, ++dptr) {
                auto bit_offs = i * 8;
                data(bit_offs + 7, bit_offs) = *dptr;
                if(beptr) {
                    strb[i] = *beptr == 0xff;
                    ++beptr;
                } else
                    strb[i] = true;
            }
        } else {
            auto beat_start_idx = byte_offset - offset;
            auto data_len = trans.get_data_length();
            auto dptr = trans.get_data_ptr() + beat_start_idx;
            for(size_t i = 0; i < size && (beat_start_idx + i) < data_len; ++i, ++dptr) {
                auto bit_offs = i * 8;
                data(bit_offs + 7, bit_offs) = *dptr;
                if(beptr) {
                    strb[i] = *beptr == 0xff;
                    ++beptr;
                } else
                    strb[i] = true;
            }
        }
    } else { // aligned or single beat access
        auto dptr = trans.get_data_ptr() + byte_offset;
        for(size_t i = 0; i < size; ++i, ++dptr) {
            auto bit_offs = (offset+i) * 8;
            data(bit_offs + 7, bit_offs) = *dptr;
            if(beptr) {
                strb[offset+i] = *beptr == 0xff;
                ++beptr;
            } else
                strb[offset+i] = true;
        }
    }
    this->w_data.write(data);
    this->w_strb.write(strb);
    if(!CFG::IS_LITE) {
        this->w_id->write(ext->get_id());
        if(this->w_user.get_interface())
            this->w_user->write(ext->get_user(axi::common::id_type::DATA));
    }
}


template <typename CFG> typename CFG::data_t axi::pin::aceLite_initiator<CFG>::get_cache_data_for_beat(fsm_handle* fsm_hndl) {
    auto beat_count = fsm_hndl->beat_count;
    //SCCTRACE(SCMOD) << " " ;
    auto size = axi::get_burst_size(*fsm_hndl->trans);
    auto byte_offset = beat_count * size;
    auto offset = (fsm_hndl->trans->get_address()+byte_offset) & (CFG::BUSWIDTH / 8 - 1);
    typename CFG::data_t data{0};
    if(offset && (size + offset) > (CFG::BUSWIDTH / 8)) { // un-aligned multi-beat access
        if(beat_count == 0) {
            auto dptr = fsm_hndl->trans->get_data_ptr();
            for(size_t i = offset; i < size; ++i, ++dptr) {
                auto bit_offs = i * 8;
                data(bit_offs + 7, bit_offs) = *dptr;
            }
        } else {
            auto beat_start_idx = byte_offset - offset;
            auto data_len = fsm_hndl->trans->get_data_length();
            auto dptr = fsm_hndl->trans->get_data_ptr() + beat_start_idx;
            for(size_t i = offset; i < size && (beat_start_idx + i) < data_len; ++i, ++dptr) {
                auto bit_offs = i * 8;
                data(bit_offs + 7, bit_offs) = *dptr;
            }
        }
    } else { // aligned or single beat access
        auto dptr = fsm_hndl->trans->get_data_ptr() + byte_offset;
        for(size_t i = 0; i < size; ++i, ++dptr) {
            auto bit_offs = (offset + i) * 8;
            data(bit_offs + 7, bit_offs) = *dptr;
        }
    }
    return data;
}

template <typename CFG> inline void axi::pin::aceLite_initiator<CFG>::setup_callbacks(fsm_handle* fsm_hndl) {
    fsm_hndl->fsm->cb[RequestPhaseBeg] = [this, fsm_hndl]() -> void {
        if(fsm_hndl->is_snoop) {
            SCCTRACE(SCMOD)<<" for snoop in RequestPhaseBeg ";
        }else {
            fsm_hndl->beat_count = 0;
            outstanding_cnt[fsm_hndl->trans->get_command()]++;
            if(CFG::IS_LITE) {
                auto offset = fsm_hndl->trans->get_address() % (CFG::BUSWIDTH / 8);
                if(offset + fsm_hndl->trans->get_data_length() > CFG::BUSWIDTH / 8) {
                    SCCFATAL(SCMOD) << " transaction " << *fsm_hndl->trans << " is not AXI4Lite compliant";
                }
            }
        }
    };
    fsm_hndl->fsm->cb[BegPartReqE] = [this, fsm_hndl]() -> void {
        sc_assert(fsm_hndl->trans->is_write());
        if(fsm_hndl->beat_count == 0) {
            write_aw(*fsm_hndl->trans);
            aw_evt.notify(sc_core::SC_ZERO_TIME);
        }
        write_wdata(*fsm_hndl->trans, fsm_hndl->beat_count);
        active_req[tlm::TLM_WRITE_COMMAND] = fsm_hndl;
        wdata_vl.write(0x1);
    };
    fsm_hndl->fsm->cb[EndPartReqE] = [this, fsm_hndl]() -> void {
        active_req[tlm::TLM_WRITE_COMMAND] = nullptr;
        tlm::tlm_phase phase = axi::END_PARTIAL_REQ;
        sc_core::sc_time t = (clk_if?clk_if->period()-acel::CLK_DELAY-1_ps:sc_core::SC_ZERO_TIME);
        auto ret = tsckt->nb_transport_bw(*fsm_hndl->trans, phase, t);
        fsm_hndl->beat_count++;
    };
    fsm_hndl->fsm->cb[BegReqE] = [this, fsm_hndl]() -> void {
        SCCTRACEALL(SCMOD)<<"In BegReqE of setup_cb";
        switch(fsm_hndl->trans->get_command()) {
            case tlm::TLM_READ_COMMAND:
                active_req[tlm::TLM_READ_COMMAND] = fsm_hndl;
                write_ar(*fsm_hndl->trans);
                ar_evt.notify(sc_core::SC_ZERO_TIME);
                break;
            case tlm::TLM_WRITE_COMMAND:
                active_req[tlm::TLM_WRITE_COMMAND] = fsm_hndl;
                if(fsm_hndl->beat_count == 0) {
                    write_aw(*fsm_hndl->trans);
                    aw_evt.notify(sc_core::SC_ZERO_TIME);
                }
                write_wdata(*fsm_hndl->trans, fsm_hndl->beat_count, true);
                wdata_vl.write(0x3);
            }
    };
    fsm_hndl->fsm->cb[EndReqE] = [this, fsm_hndl]() -> void {
        SCCTRACEALL(SCMOD)<<"In EndReqE of setup_cb";
        switch(fsm_hndl->trans->get_command()) {
            case tlm::TLM_READ_COMMAND:
                rd_resp_by_id[axi::get_axi_id(*fsm_hndl->trans)].push_back(fsm_hndl);
                active_req[tlm::TLM_READ_COMMAND] = nullptr;
                break;
            case tlm::TLM_WRITE_COMMAND:
                wr_resp_by_id[axi::get_axi_id(*fsm_hndl->trans)].push_back(fsm_hndl);
                active_req[tlm::TLM_WRITE_COMMAND] = nullptr;
                fsm_hndl->beat_count++;
            }
            tlm::tlm_phase phase = tlm::END_REQ;
            sc_core::sc_time t = (clk_if?clk_if->period()-acel::CLK_DELAY-1_ps:sc_core::SC_ZERO_TIME);
            SCCTRACE(SCMOD) << " in EndReq before set_resp";
            auto ret = tsckt->nb_transport_bw(*fsm_hndl->trans, phase, t);
            fsm_hndl->trans->set_response_status(tlm::TLM_OK_RESPONSE);
    };

    fsm_hndl->fsm->cb[BegPartRespE] = [this, fsm_hndl]() -> void {
        // scheduling the response
        assert(fsm_hndl->trans->is_read());
        tlm::tlm_phase phase = axi::BEGIN_PARTIAL_RESP;
        sc_core::sc_time t(sc_core::SC_ZERO_TIME);
        auto ret = tsckt->nb_transport_bw(*fsm_hndl->trans, phase, t);

    };
    fsm_hndl->fsm->cb[EndPartRespE] = [this, fsm_hndl]() -> void {
       SCCTRACE(SCMOD) <<"in EndPartRespE of setup_cb ";
       fsm_hndl->beat_count++;
       r_end_req_evt.notify();
    };
    fsm_hndl->fsm->cb[BegRespE] = [this, fsm_hndl]() -> void {
        // scheduling the response
        tlm::tlm_phase phase = tlm::BEGIN_RESP;
        sc_core::sc_time t(sc_core::SC_ZERO_TIME);
        auto ret = tsckt->nb_transport_bw(*fsm_hndl->trans, phase, t);
    };
    fsm_hndl->fsm->cb[EndRespE] = [this, fsm_hndl]() -> void {
        SCCTRACE(SCMOD)<< "in EndResp of setup_cb, next should become ack " ;
    };
    fsm_hndl->fsm->cb[Ack] = [this, fsm_hndl]() -> void {
        SCCTRACE(SCMOD)<< "in ACK of setup_cb " ;
        r_end_req_evt.notify();
        if(fsm_hndl->trans->is_read())
            rd_resp_by_id[axi::get_axi_id(*fsm_hndl->trans)].pop_front();
        if(fsm_hndl->trans->is_write())
            wr_resp_by_id[axi::get_axi_id(*fsm_hndl->trans)].pop_front();
    };
}
template <typename CFG> inline void axi::pin::aceLite_initiator<CFG>::ar_t() {
    this->ar_valid.write(false);
    wait(sc_core::SC_ZERO_TIME);
    while(true) {
        wait(ar_evt);
        this->ar_valid.write(true);
        do {
            wait(this->ar_ready.posedge_event() | clk_delayed);
            if(this->ar_ready.read())
                react(axi::fsm::protocol_time_point_e::EndReqE, active_req[tlm::TLM_READ_COMMAND]);
        } while(!this->ar_ready.read());
        wait(clk_i.posedge_event());
        this->ar_valid.write(false);
    }
}

template <typename CFG> inline void axi::pin::aceLite_initiator<CFG>::r_t() {
    this->r_ready.write(false);
    wait(sc_core::SC_ZERO_TIME);
    while(true) {
        wait(this->r_valid.posedge_event() | clk_delayed);
        if(this->r_valid.event() || (!active_resp[tlm::TLM_READ_COMMAND] && this->r_valid.read())) {
            wait(sc_core::SC_ZERO_TIME);
            auto id = CFG::IS_LITE ? 0U : this->r_id->read().to_uint();
            auto data = this->r_data.read();
            auto resp = this->r_resp.read();
            auto& q = rd_resp_by_id[id];
            sc_assert(q.size());
            auto* fsm_hndl = q.front();
            auto beat_count = fsm_hndl->beat_count;
            auto size = axi::get_burst_size(*fsm_hndl->trans);
            auto byte_offset = beat_count * size;
            auto offset = (fsm_hndl->trans->get_address()+byte_offset) & (CFG::BUSWIDTH / 8 - 1);
            if(offset && (size + offset) > (CFG::BUSWIDTH / 8)) { // un-aligned multi-beat access
                if(beat_count == 0) {
                    auto dptr = fsm_hndl->trans->get_data_ptr();
                    for(size_t i = offset; i < size; ++i, ++dptr) {
                        auto bit_offs = i * 8;
                        *dptr = data(bit_offs + 7, bit_offs).to_uint();
                    }
                } else {
                    auto beat_start_idx = beat_count * size - offset;
                    auto data_len = fsm_hndl->trans->get_data_length();
                    auto dptr = fsm_hndl->trans->get_data_ptr() + beat_start_idx;
                    for(size_t i = offset; i < size && (beat_start_idx + i) < data_len; ++i, ++dptr) {
                        auto bit_offs = i * 8;
                        *dptr = data(bit_offs + 7, bit_offs).to_uint();
                    }
                }
            } else { // aligned or single beat access
                auto dptr = fsm_hndl->trans->get_data_ptr() + beat_count * size;
                for(size_t i = 0; i < size; ++i, ++dptr) {
                    auto bit_offs = (offset+i) * 8;
                    *dptr = data(bit_offs + 7, bit_offs).to_uint();
                }
            }
            axi::ace_extension* e;
            fsm_hndl->trans->get_extension(e);
            e->set_resp(axi::into<axi::resp_e>(resp));
            e->add_to_response_array(*e);
            auto tp = CFG::IS_LITE || this->r_last->read() ? axi::fsm::protocol_time_point_e::BegRespE
                                                           : axi::fsm::protocol_time_point_e::BegPartRespE;
            react(tp, fsm_hndl);
            // r_end_req_evt notified in EndPartialResp or ACK
            wait(r_end_req_evt);
            this->r_ready->write(true);
            wait(clk_i.posedge_event());
            this->r_ready.write(false);
        }
    }
}

template <typename CFG> inline void axi::pin::aceLite_initiator<CFG>::aw_t() {
    this->aw_valid.write(false);
    wait(sc_core::SC_ZERO_TIME);
    while(true) {
        wait(aw_evt);
        this->aw_valid.write(true);
        do {
            wait(this->aw_ready.posedge_event() | clk_delayed);
        } while(!this->aw_ready.read());
        wait(clk_i.posedge_event());
        this->aw_valid.write(false);
    }
}

template <typename CFG> inline void axi::pin::aceLite_initiator<CFG>::wdata_t() {
    this->w_valid.write(false);
    wait(sc_core::SC_ZERO_TIME);
    while(true) {
        if(!CFG::IS_LITE)
            this->w_last->write(false);
        wait(wdata_vl.default_event());
        auto val = wdata_vl.read();
        this->w_valid.write(val & 0x1);
        if(!CFG::IS_LITE)
            this->w_last->write(val & 0x2);
        do {
            wait(this->w_ready.posedge_event() | clk_delayed);
            if(this->w_ready.read()) {
                auto evt = CFG::IS_LITE || (val & 0x2) ? axi::fsm::protocol_time_point_e::EndReqE
                                                       : axi::fsm::protocol_time_point_e::EndPartReqE;
                react(evt, active_req[tlm::TLM_WRITE_COMMAND]);
            }
        } while(!this->w_ready.read());
        wait(clk_i.posedge_event());
        this->w_valid.write(false);
    }
}

template <typename CFG> inline void axi::pin::aceLite_initiator<CFG>::b_t() {
    this->b_ready.write(false);
    wait(sc_core::SC_ZERO_TIME);
    while(true) {
        wait(this->b_valid.posedge_event() | clk_delayed);
        SCCTRACEALL(SCMOD)<<" b_t() received b_valid ";
        if(this->b_valid.event() || (!active_resp[tlm::TLM_WRITE_COMMAND] && this->b_valid.read())) {
            auto id = !CFG::IS_LITE ? this->b_id->read().to_uint() : 0U;
            auto resp = this->b_resp.read();
            auto& q = wr_resp_by_id[id];
            sc_assert(q.size());
            auto* fsm_hndl = q.front();
            axi::ace_extension* e;
            fsm_hndl->trans->get_extension(e);
            e->set_resp(axi::into<axi::resp_e>(resp));
            react(axi::fsm::protocol_time_point_e::BegRespE, fsm_hndl);
            // r_end_req_evt notified in EndPartialResp or ACK
            wait(r_end_req_evt);
            this->b_ready.write(true);
            wait(clk_i.posedge_event());
            this->b_ready.write(false);
        }
    }
}
#endif /* _BUS_AXI_PIN_ACELITE_INITIATOR_H_ */