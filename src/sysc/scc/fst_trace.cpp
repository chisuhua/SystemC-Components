/*******************************************************************************
 * Copyright 2021 MINRES Technologies GmbH
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

#include "fst_trace.h"
#include "utilities.h"
#include <util/ities.h>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include <deque>
#include <unordered_map>

#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_ver.h>
#include <sysc/kernel/sc_event.h>
#include <sysc/datatypes/bit/sc_bit.h>
#include <sysc/datatypes/bit/sc_logic.h>
#include <sysc/datatypes/bit/sc_lv_base.h>
#include <sysc/datatypes/int/sc_signed.h>
#include <sysc/datatypes/int/sc_unsigned.h>
#include <sysc/datatypes/int/sc_int_base.h>
#include <sysc/datatypes/int/sc_uint_base.h>
#include <sysc/datatypes/fx/fx.h>
#include <sysc/utils/sc_report.h> // sc_assert
#include <sysc/utils/sc_string_view.h>

#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <cmath>

namespace scc {

inline size_t get_buffer_size(int length){
    size_t sz = ( static_cast<size_t>(length) + 4096 ) & (~static_cast<size_t>(4096-1));
    return std::max(1024UL, sz);
}


struct fst_trace {

    fst_trace(std::string const& nm): name{nm}{}

    virtual void record(void* m_fst) = 0;

    virtual void update_and_record(void* m_fst) = 0;

    virtual uintptr_t get_hash() = 0;

    virtual ~fst_trace(){};

    const std::string name;
    fstHandle fst_hndl{0};
    bool is_alias{false};
    unsigned bits{0};
};

template<typename T, typename OT=T>
struct fst_trace_t : public fst_trace {
    fst_trace_t( const T& object_,
            const std::string& name, int width=-1)
    : fst_trace( name),
      act_val( object_ ),
      old_val( object_ )
    {bits=width<0?get_bits():width;}

    uintptr_t get_hash() override { return reinterpret_cast<uintptr_t>(&act_val);}

    inline bool changed() { return !is_alias && old_val!=act_val; }

    inline void update() { old_val=act_val; }

    void record(void* m_fst) override;

    void update_and_record(void* m_fst) override {update(); record(m_fst);};

    unsigned get_bits() { return sizeof(OT)*8;}
    OT old_val;
    const T& act_val;
};

template<typename T, typename OT>
inline void scc::fst_trace_t<T, OT>::record(void *m_fst) {
    if(sizeof(T)<=4)
        fstWriterEmitValueChange32(m_fst, fst_hndl, bits, old_val);
    else
        fstWriterEmitValueChange64(m_fst, fst_hndl, bits, old_val);
}

/*
 * bool
 */
template<> void fst_trace_t<bool, bool>::record(void* m_fst){
    fstWriterEmitValueChange(m_fst, fst_hndl, old_val ? "1" : "0");
}

template<> unsigned fst_trace_t<bool, bool>::get_bits(){ return 1; }
/*
 * sc_dt::sc_bit
 */
template<> void fst_trace_t<sc_dt::sc_bit, sc_dt::sc_bit>::record(void* m_fst){
    fstWriterEmitValueChange(m_fst, fst_hndl, old_val ? "1" : "0");
}

template<> unsigned fst_trace_t<sc_dt::sc_bit, sc_dt::sc_bit>::get_bits(){ return 1; }
/*
 * sc_dt::sc_logic
 */
template<> void fst_trace_t<sc_dt::sc_logic, sc_dt::sc_logic>::record(void* m_fst){
    char buf[2] = {0, 0};
    buf[0]=old_val.to_char();
    fstWriterEmitValueChange(m_fst, fst_hndl, buf);
}

template<> unsigned fst_trace_t<sc_dt::sc_logic, sc_dt::sc_logic>::get_bits(){ return 1; }
/*
 * float
 */
template<> void fst_trace_t<float, float>::record(void* m_fst){
    fstWriterEmitValueChange32(m_fst, fst_hndl, 32, *reinterpret_cast<uint32_t*>(&old_val));
}
/*
 * double
 */
template<> void fst_trace_t<double, double>::record(void* m_fst){
    fstWriterEmitValueChange32(m_fst, fst_hndl, 64, *reinterpret_cast<uint64_t*>(&old_val));
}
/*
 * sc_dt::sc_int_base
 */
template<> void fst_trace_t<sc_dt::sc_int_base, sc_dt::sc_int_base>::record(void* m_fst){
    static std::vector<char> rawdata(get_buffer_size(old_val.length()));
    char *rawdata_ptr  = &rawdata[0];
    for (int bitindex = old_val.length() - 1; bitindex >= 0; --bitindex) {
        *rawdata_ptr++ = '0'+old_val[bitindex].value();
    }
    fstWriterEmitValueChange(m_fst, fst_hndl, &rawdata[0]);
}

template<> unsigned fst_trace_t<sc_dt::sc_int_base, sc_dt::sc_int_base>::get_bits(){ return old_val.length(); }
/*
 * sc_dt::sc_uint_base
 */
template<> void fst_trace_t<sc_dt::sc_uint_base, sc_dt::sc_uint_base>::record(void* m_fst){
    static std::vector<char> rawdata(get_buffer_size(old_val.length()));
    char *rawdata_ptr  = &rawdata[0];
    for (int bitindex = old_val.length() - 1; bitindex >= 0; --bitindex) {
        *rawdata_ptr++ = '0'+old_val[bitindex].value();
    }
    fstWriterEmitValueChange(m_fst, fst_hndl, &rawdata[0]);
}

template<> unsigned fst_trace_t<sc_dt::sc_uint_base, sc_dt::sc_uint_base>::get_bits(){ return old_val.length(); }
/*
 * sc_dt::sc_signed
 */
template<> void fst_trace_t<sc_dt::sc_signed, sc_dt::sc_signed>::record(void* m_fst){
    static std::vector<char> rawdata(get_buffer_size(old_val.length()));
    char *rawdata_ptr  = &rawdata[0];
    for (int bitindex = old_val.length() - 1; bitindex >= 0; --bitindex) {
        *rawdata_ptr++ = '0'+old_val[bitindex].value();
    }
    fstWriterEmitValueChange(m_fst, fst_hndl, &rawdata[0]);
}

template<> unsigned fst_trace_t<sc_dt::sc_signed, sc_dt::sc_signed>::get_bits(){ return old_val.length(); }
/*
 * sc_dt::sc_unsigned
 */
template<> void fst_trace_t<sc_dt::sc_unsigned, sc_dt::sc_unsigned>::record(void* m_fst){
    static std::vector<char> rawdata(get_buffer_size(old_val.length()));
    char *rawdata_ptr  = &rawdata[0];
    for (int bitindex = old_val.length() - 1; bitindex >= 0; --bitindex) {
        *rawdata_ptr++ = '0'+old_val[bitindex].value();
    }
    fstWriterEmitValueChange(m_fst, fst_hndl, &rawdata[0]);
}

template<> unsigned fst_trace_t<sc_dt::sc_unsigned, sc_dt::sc_unsigned>::get_bits(){ return old_val.length(); }
/*
 * sc_dt::sc_fxval
 */
template<> void fst_trace_t<sc_dt::sc_fxval, sc_dt::sc_fxval>::record(void* m_fst){
    auto val = old_val.to_double();
    fstWriterEmitValueChange64(m_fst, fst_hndl, 64, *reinterpret_cast<uint64_t*>(&val));
}

template<> unsigned fst_trace_t<sc_dt::sc_fxval, sc_dt::sc_fxval>::get_bits(){ return 64; }
/*
 * sc_dt::sc_fxval_fast
 */
template<> void fst_trace_t<sc_dt::sc_fxval_fast, sc_dt::sc_fxval_fast>::record(void* m_fst){
    auto val = old_val.to_double();
    fstWriterEmitValueChange64(m_fst, fst_hndl, 64, *reinterpret_cast<uint64_t*>(&val));
}

template<> unsigned fst_trace_t<sc_dt::sc_fxval_fast, sc_dt::sc_fxval_fast>::get_bits(){ return 64; }
/*
 * sc_dt::sc_fxnum
 */
template<> void fst_trace_t<sc_dt::sc_fxnum, double>::record(void* m_fst){
    fstWriterEmitValueChange32(m_fst, fst_hndl, 64, *reinterpret_cast<uint64_t*>(&old_val));
}
/*
 * sc_dt::sc_fxnum_fast
 */
template<> void fst_trace_t<sc_dt::sc_fxnum_fast, double>::record(void* m_fst){
    fstWriterEmitValueChange32(m_fst, fst_hndl, 64, *reinterpret_cast<uint64_t*>(&old_val));
}
/*
 * sc_dt::sc_bv_base
 */
template<> void fst_trace_t<sc_dt::sc_bv_base, sc_dt::sc_bv_base>::record(void* m_fst){
    auto str = old_val.to_string();
    fstWriterEmitValueChange(m_fst, fst_hndl, str.c_str());
}

template<> unsigned fst_trace_t<sc_dt::sc_bv_base, sc_dt::sc_bv_base>::get_bits(){ return old_val.length(); }
/*
 * sc_dt::sc_lv_base
 */
template<> void fst_trace_t<sc_dt::sc_lv_base, sc_dt::sc_lv_base>::record(void* m_fst){
    auto str = old_val.to_string();
    fstWriterEmitValueChange(m_fst, fst_hndl, str.c_str());
}

template<> unsigned fst_trace_t<sc_dt::sc_lv_base, sc_dt::sc_lv_base>::get_bits(){ return old_val.length(); }


fst_trace_file::fst_trace_file(const char *name, std::function<bool()> &enable)
{
    std::stringstream ss;
    ss<<name<<".fst";
    m_fst = fstWriterCreate(ss.str().c_str(), 1);
    fstWriterSetPackType(m_fst, FST_WR_PT_LZ4);
    fstWriterSetTimescale(m_fst, 12);  // pico seconds 1*10-12
    fstWriterSetFileType(m_fst, FST_FT_VERILOG);
#if defined(WITH_SIM_PHASE_CALLBACKS)
    // remove from hierarchy
    sc_object::detach();
    // register regular (non-delta) callbacks
    sc_object::register_simulation_phase_callback( SC_BEFORE_TIMESTEP );
#else // explicitly register with simcontext
    sc_core::sc_get_curr_simcontext()->add_trace_file( this );
#endif
}

fst_trace_file::~fst_trace_file() {
    if (m_fst){
        fstWriterFlushContext(m_fst);
        fstWriterClose(m_fst);
    }
}

template<typename T, typename OT=T>
bool changed(fst_trace* trace) {
    if(reinterpret_cast<fst_trace_t<T, OT>*>(trace)->changed()){
        reinterpret_cast<fst_trace_t<T, OT>*>(trace)->update();
        return true;
    } else
        return false;
}
#define DECL_TRACE_METHOD_A(tp) void fst_trace_file::trace(const tp& object, const std::string& name)\
        {all_traces.emplace_back(&changed<tp>, new fst_trace_t<tp>(object, name));}
#define DECL_TRACE_METHOD_B(tp) void fst_trace_file::trace(const tp& object, const std::string& name, int width)\
        {all_traces.emplace_back(&changed<tp>, new fst_trace_t<tp>(object, name));}
#define DECL_TRACE_METHOD_C(tp, tpo) void fst_trace_file::trace(const tp& object, const std::string& name)\
        {all_traces.emplace_back(&changed<tp, tpo>, new fst_trace_t<tp, tpo>(object, name));}

#if (SYSTEMC_VERSION >= 20171012)
void fst_trace_file::trace(const sc_core::sc_event& object, const std::string& name){}
void fst_trace_file::trace(const sc_core::sc_time& object, const std::string& name){}
#endif
DECL_TRACE_METHOD_A( bool )
DECL_TRACE_METHOD_A( sc_dt::sc_bit )
DECL_TRACE_METHOD_A( sc_dt::sc_logic )

DECL_TRACE_METHOD_B( unsigned char )
DECL_TRACE_METHOD_B( unsigned short )
DECL_TRACE_METHOD_B( unsigned int )
DECL_TRACE_METHOD_B( unsigned long )
#ifdef SYSTEMC_64BIT_PATCHES
DECL_TRACE_METHOD_B( unsigned long long)
#endif
DECL_TRACE_METHOD_B( char )
DECL_TRACE_METHOD_B( short )
DECL_TRACE_METHOD_B( int )
DECL_TRACE_METHOD_B( long )
DECL_TRACE_METHOD_B( sc_dt::int64 )
DECL_TRACE_METHOD_B( sc_dt::uint64 )

DECL_TRACE_METHOD_A( float )
DECL_TRACE_METHOD_A( double )
DECL_TRACE_METHOD_A( sc_dt::sc_int_base )
DECL_TRACE_METHOD_A( sc_dt::sc_uint_base )
DECL_TRACE_METHOD_A( sc_dt::sc_signed )
DECL_TRACE_METHOD_A( sc_dt::sc_unsigned )

DECL_TRACE_METHOD_A( sc_dt::sc_fxval )
DECL_TRACE_METHOD_A( sc_dt::sc_fxval_fast )
DECL_TRACE_METHOD_C( sc_dt::sc_fxnum, double)
DECL_TRACE_METHOD_C( sc_dt::sc_fxnum_fast, double)

DECL_TRACE_METHOD_A( sc_dt::sc_bv_base )
DECL_TRACE_METHOD_A( sc_dt::sc_lv_base )
#undef DECL_TRACE_METHOD_A
#undef DECL_TRACE_METHOD_B

void fst_trace_file::trace(const unsigned int &object, const std::string &name, const char **enum_literals) {
}

void fst_trace_file::write_comment(const std::string &comment) {
}

void fst_trace_file::init() {
    std::sort(std::begin(all_traces), std::end(all_traces), [](trace_entry const& a, trace_entry const& b)->bool {return a.trc->name<b.trc->name;});
    std::unordered_map<uintptr_t, fstHandle> alias_map;

    std::deque<std::string> fst_scope;
    for(auto& e:all_traces){
        auto alias_it = alias_map.find(e.trc->get_hash());
        e.trc->is_alias=alias_it!=std::end(alias_map);
        auto hier_tokens = util::split(e.trc->name, '.');
        auto sig_name=hier_tokens.back();
        hier_tokens.pop_back();
        auto cur_it = fst_scope.begin();
        auto tok_it = hier_tokens.begin();
        while (cur_it != std::end(fst_scope) && tok_it != std::end(hier_tokens)) {
            if (*cur_it != *tok_it) break;
            ++cur_it;
            ++tok_it;
        }
        for(auto it=std::rbegin(fst_scope); cur_it!=it.base(); it++){
            fstWriterSetUpscope(m_fst);
            fst_scope.pop_back();
        }
        for(; tok_it!=std::end(hier_tokens); tok_it++){
            fstWriterSetScope(m_fst, FST_ST_VCD_SCOPE, tok_it->c_str(), nullptr);
            fst_scope.push_back(*tok_it);
        }
        e.trc->fst_hndl = fstWriterCreateVar(m_fst, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, e.trc->bits, sig_name.c_str(), e.trc->is_alias?alias_it->second:0);
        if(!e.trc->is_alias)
            alias_map.insert({e.trc->get_hash(), e.trc->fst_hndl});
    }
    std::copy_if(std::begin(all_traces), std::end(all_traces),
                std::back_inserter(active_traces),
                [](trace_entry const& e) { return !e.trc->is_alias; });
}

void fst_trace_file::cycle(bool delta_cycle) {
    if(delta_cycle) return;
    if(!initialized) init();
    fstWriterEmitTimeChange(m_fst, sc_core::sc_time_stamp().value()/(1_ps).value());
    if(!initialized) {
        initialized=true;
        for(auto& e: all_traces)
            e.trc->update_and_record(m_fst);
    } else {
        for(auto e: active_traces)
            if(e.compare_and_update(e.trc))
                e.trc->record(m_fst);
    }
}

sc_core::sc_trace_file* scc_create_fst_trace_file(const char *name, std::function<bool()> enable) {
    return  new fst_trace_file(name, enable);
}

void scc_close_fst_trace_file(sc_core::sc_trace_file *tf) {
    delete static_cast<fst_trace_file*>(tf);
}

void fst_trace_file::set_time_unit(double v, sc_core::sc_time_unit tu) {
}

}