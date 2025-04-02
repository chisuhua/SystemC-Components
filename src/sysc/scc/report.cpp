/*******************************************************************************
 * Copyright 2017, 2018 MINRES Technologies GmbH
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
/*
 * report.cpp
 *
 *  Created on: 19.09.2017
 *      Author: ubuntu
 */

#include "report.h"
#include "configurer.h"
#include <array>
#include <chrono>
#include <fstream>
#include <mutex>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <tuple>
#include <unordered_map>
#ifdef __GNUC__
#define GCC_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#if GCC_VERSION < 40900
#define USE_C_REGEX
#endif
#define likely(x) __builtin_expect(x, 1)
#define unlikely(x) __builtin_expect(x, 0)
#else
#define likely(x) x
#define unlikely(x) x
#endif

#ifdef USE_C_REGEX
#include <regex.h>
#else
#include <regex>
#endif
#ifdef ERROR
#undef ERROR
#endif
using namespace std;
using namespace sc_core;
using namespace scc;

namespace {
struct char_equal_to : public std::equal_to<char const*> {
    bool operator()(char const* __x, char const* __y) const { return strcmp(__x, __y) == 0; }
};

struct char_hash {
    // BKDR hash algorithm
    uint64_t operator()(char const* str) const {
        constexpr unsigned int seed = 131; // 31  131 1313 13131131313 etc//
        uint64_t hash = 0;
        while(*str) {
            hash = (hash * seed) + (*str);
            str++;
        }
        return hash;
    }
};
#ifdef MTContext
struct Lut {
    std::unordered_map<char const*, sc_core::sc_verbosity, char_hash, char_equal_to> table;
    std::vector<std::string> cache;
    void insert(char const* key, sc_core::sc_verbosity verb) {
        cache.push_back(key);
        table.insert({cache.back().c_str(), verb});
    }
    void clear() {
        table.clear();
        cache.clear();
    }
    static std::map<sc_simcontext*, Lut*> active;
    static std::mutex mutex;
    static Lut& Get() {
      std::lock_guard<std::mutex> lock(mutex);
      sc_simcontext* ctx = sc_core::sc_get_curr_simcontext();
      auto it = active.find(ctx);
      if (it == active.end())  {
        active.insert({ctx,new Lut});
        it = active.find(ctx);
      }
      return *(it->second);
    }
};

std::map<sc_simcontext*, Lut*> Lut::active;
std::mutex Lut::mutex;

#define lut Lut::Get()
#else
thread_local struct {
    std::unordered_map<char const*, sc_core::sc_verbosity, char_hash, char_equal_to> table;
    std::vector<std::string> cache;
    void insert(char const* key, sc_core::sc_verbosity verb) {
        cache.push_back(key);
        table.insert({cache.back().c_str(), verb});
    }
    void clear() {
        table.clear();
        cache.clear();
    }
} lut;
#endif

#ifdef MTI_SYSTEMC
static const cci::cci_originator originator;
#else
static const cci::cci_originator originator("reporting");
#endif

#ifdef MTContext
bool& sc_stop_called() {
    static std::map<sc_simcontext*, bool> active;
    static bool active_default = false;
    sc_simcontext* ctx = sc_get_curr_simcontext();
    auto it = active.find(ctx);
    if (it == active.end()) {
        active.insert({ctx, active_default});
        return active_default;
    }
    return it->second;
}
#else
#endif

bool& inst_based_logging() {
#ifdef MTContext
    static std::map<sc_simcontext*, bool> active;
    static bool active_default = getenv("SCC_DISABLE_INSTANCE_BASED_LOGGING") == nullptr;
    sc_simcontext* ctx = sc_get_curr_simcontext();
    auto it = active.find(ctx);
    if (it == active.end()) {
      active.insert({ctx, active_default});
      return active_default;
    }
    return it->second;
#else
    thread_local bool active = getenv("SCC_DISABLE_INSTANCE_BASED_LOGGING") == nullptr;
    return active;
#endif
}

struct ExtLogConfig : public scc::LogConfig {
    shared_ptr<spdlog::logger> file_logger;
    shared_ptr<spdlog::logger> console_logger;
#ifdef USE_C_REGEX
    regex_t start_state{};
#else
    regex reg_ex;
#endif
    sc_time cycle_base{0, SC_NS};
    auto operator=(const scc::LogConfig& o) -> ExtLogConfig& {
        scc::LogConfig::operator=(o);
        return *this;
    }
    auto match(const char* type) -> bool {
#ifdef USE_C_REGEX
        return regexec(&start_state, type, 0, nullptr, 0) == 0;
#else
        return regex_search(type, reg_ex);
#endif
    }
    bool initialized{false};
};

#ifdef MTContext
class LogCfg {
public:
    static ExtLogConfig& Get() {
        sc_simcontext* ctx = sc_core::sc_get_curr_simcontext();
        auto it = m_logcfg.find(ctx); 
        if (it == m_logcfg.end()) {
            auto ctx_logcfg = new ExtLogConfig();
            m_logcfg.insert({ctx, ctx_logcfg});
            return *ctx_logcfg;
        }
        return *(it->second);
    }
    static std::map<sc_simcontext*, ExtLogConfig*> m_logcfg;
};
std::map<sc_simcontext*, ExtLogConfig*> LogCfg::m_logcfg;
#define log_cfg LogCfg::Get()
#else
thread_local ExtLogConfig log_cfg;
#endif

auto get_tuple(const sc_time& t) -> tuple<sc_time::value_type, sc_time_unit> {
    auto val = t.value();
    auto tr = (uint64_t)(sc_time::from_value(1).to_seconds() * 1E15);
    auto scale = 0U;
    while((tr % 10) == 0) {
        tr /= 10;
        scale++;
    }
    sc_assert(tr == 1);

    auto tu = scale / 3;
    while(tu < SC_SEC && (val % 10) == 0) {
        val /= 10;
        scale++;
        tu += (0 == (scale % 3));
    }
    for(scale %= 3; scale != 0; scale--)
        val *= 10;
    return make_tuple(val, static_cast<sc_time_unit>(tu));
}

auto time2string(const sc_time& t) -> string {
    const array<const char*, 6> time_units{"fs", "ps", "ns", "us", "ms", "s "};
    const array<uint64_t, 6> multiplier{
        1ULL, 1000ULL, 1000ULL * 1000, 1000ULL * 1000 * 1000, 1000ULL * 1000 * 1000 * 1000, 1000ULL * 1000 * 1000 * 1000 * 1000};
    ostringstream oss;
    if(!t.value()) {
        oss << "0 s ";
    } else {
        const auto tt = get_tuple(t);
        const auto val = get<0>(tt);
        const auto scale = get<1>(tt);
        const auto fs_val = val * multiplier[scale];
        for(int j = multiplier.size() - 1; j >= scale; --j) {
            if(fs_val >= multiplier[j]) {
                const auto i = val / multiplier[j - scale];
                const auto f = val % multiplier[j - scale];
                oss << i << '.' << setw(3 * (j - scale)) << setfill('0') << right << f << ' ' << time_units[j];
                break;
            }
        }
    }
    return oss.str();
}
auto compose_message(const sc_report& rep, const scc::LogConfig& cfg) -> const string {
    if(rep.get_severity() > SC_INFO || cfg.log_filter_regex.length() == 0 || rep.get_verbosity() == sc_core::SC_MEDIUM ||
       log_cfg.match(rep.get_msg_type())) {
        stringstream os;
        if(likely(cfg.print_sim_time)) {
            if(unlikely(log_cfg.cycle_base.value())) {
                if(unlikely(cfg.print_delta))
                    os << "[" << std::setw(7) << std::setfill(' ') << sc_time_stamp().value() / log_cfg.cycle_base.value() << "(" << setw(5)
                       << sc_delta_count() << ")]";
                else
                    os << "[" << std::setw(7) << std::setfill(' ') << sc_time_stamp().value() / log_cfg.cycle_base.value() << "]";
            } else {
                auto t = time2string(sc_time_stamp());
                if(unlikely(cfg.print_delta))
                    os << "[" << std::setw(20) << std::setfill(' ') << t << "(" << setw(5) << sc_delta_count() << ")]";
                else
                    os << "[" << std::setw(20) << std::setfill(' ') << t << "]";
            }
        }
        if(unlikely(rep.get_id() >= 0))
            os << "("
               << "IWEF"[rep.get_severity()] << rep.get_id() << ") " << rep.get_msg_type() << ": ";
        else if(cfg.msg_type_field_width) {
            if(cfg.msg_type_field_width == std::numeric_limits<unsigned>::max())
                os << rep.get_msg_type() << ": ";
            else
                os << util::padded(rep.get_msg_type(), cfg.msg_type_field_width) << ": ";
        }
        if(*rep.get_msg())
            os << rep.get_msg();
        if(rep.get_severity() > SC_INFO) {
            if(rep.get_line_number())
                os << "\n         [FILE:" << rep.get_file_name() << ":" << rep.get_line_number() << "]";
            sc_simcontext* simc = sc_get_curr_simcontext();
            if(simc && sc_is_running()) {
                const char* proc_name = rep.get_process_name();
                if(proc_name)
                    os << "\n         [PROCESS:" << proc_name << "]";
            }
        }
        return os.str();
    } else
        return "";
}

inline auto get_verbosity(const sc_report& rep) -> int {
    return rep.get_verbosity() > sc_core::SC_NONE && rep.get_verbosity() < sc_core::SC_LOW ? rep.get_verbosity() * 10 : rep.get_verbosity();
}

inline void log2logger(spdlog::logger& logger, const sc_report& rep, const scc::LogConfig& cfg) {
    auto msg = compose_message(rep, cfg);
    if(!msg.size())
        return;
    switch(rep.get_severity()) {
    case SC_INFO:
        switch(get_verbosity(rep)) {
        case SC_DEBUG:
        case SC_FULL:
            logger.trace(msg);
            break;
        case SC_HIGH:
            logger.debug(msg);
            break;
        default:
            logger.info(msg);
            break;
        }
        break;
    case SC_WARNING:
        logger.warn(msg);
        break;
    case SC_ERROR:
        logger.error(msg);
        break;
    case SC_FATAL:
        logger.critical(msg);
        break;
    default:
        break;
    }
}

inline void log2logger(spdlog::logger& logger, scc::log lvl, const string& msg) {
    switch(lvl) {
    case scc::log::DBGTRACE:
    case scc::log::TRACE:
        logger.trace(msg);
        return;
    case scc::log::DEBUG:
        logger.debug(msg);
        return;
    case scc::log::INFO:
        logger.info(msg);
        return;
    case scc::log::WARNING:
        logger.warn(msg);
        return;
    case scc::log::ERROR:
        logger.error(msg);
        return;
    case scc::log::FATAL:
        logger.critical(msg);
        return;
    default:
        break;
    }
}

void report_handler(const sc_report& rep, const sc_actions& actions) {
#ifndef MTContext
    thread_local bool sc_stop_called = false;
#endif
    if(actions & SC_DO_NOTHING)
        return;
    if(rep.get_severity() == sc_core::SC_INFO || !log_cfg.report_only_first_error || sc_report_handler::get_count(SC_ERROR) < 2) {
        if((actions & SC_DISPLAY) && (!log_cfg.file_logger || get_verbosity(rep) < SC_HIGH))
            log2logger(*log_cfg.console_logger, rep, log_cfg);
        if((actions & SC_LOG) && log_cfg.file_logger) {
            scc::LogConfig lcfg(log_cfg);
            lcfg.print_sim_time = true;
            if(!lcfg.msg_type_field_width)
                lcfg.msg_type_field_width = 24;
            log2logger(*log_cfg.file_logger, rep, lcfg);
        }
    }
    if(actions & SC_STOP) {
        this_thread::sleep_for(chrono::milliseconds(static_cast<unsigned>(log_cfg.level) * 10));
        if(sc_is_running() && !sc_stop_called()) {
            sc_stop();
            sc_stop_called() = true;
        }
    }
    if(actions & SC_ABORT) {
        this_thread::sleep_for(chrono::milliseconds(static_cast<unsigned>(log_cfg.level) * 20));
        abort();
    }
    if(actions & SC_THROW) {
        this_thread::sleep_for(chrono::milliseconds(static_cast<unsigned>(log_cfg.level) * 20));
        throw rep;
    }
    if(sc_time_stamp().value() && !sc_is_running()) {
        log_cfg.console_logger->flush();
        if(log_cfg.file_logger)
            log_cfg.file_logger->flush();
        this_thread::sleep_for(chrono::milliseconds(static_cast<unsigned>(log_cfg.level) * 10));
    }
}
} // namespace

scc::stream_redirection::stream_redirection(ostream& os, log level)
: os(os)
, level(level) {
    old_buf = os.rdbuf(this); // save and redirect
}

scc::stream_redirection::~stream_redirection() {
    os.rdbuf(old_buf); // restore
}

void scc::stream_redirection::reset() {
    os.rdbuf(old_buf); // restore
    old_buf = nullptr;
}

auto scc::stream_redirection::xsputn(const char_type* s, streamsize n) -> streamsize {
    auto sz = stringbuf::xsputn(s, n);
    if(s[n - 1] == '\n') {
        sync();
    }
    return sz;
}

static const array<sc_severity, 8> severity = {SC_FATAL,    // scc::log::NONE
                                               SC_FATAL,    // scc::log::FATAL
                                               SC_ERROR,    // scc::log::ERROR
                                               SC_WARNING,  // scc::log::WARNING
                                               SC_INFO,     // scc::log::INFO
                                               SC_INFO,     // scc::log::DEBUG
                                               SC_INFO,     // scc::log::TRACE
                                               SC_INFO};    // scc::log::DBGTRACE
static const array<sc_verbosity, 8> verbosity = {SC_NONE,   // scc::log::NONE
                                                 SC_LOW,    // scc::log::FATAL
                                                 SC_LOW,    // scc::log::ERROR
                                                 SC_LOW,    // scc::log::WARNING
                                                 SC_MEDIUM, // scc::log::INFO
                                                 SC_HIGH,   // scc::log::DEBUG
                                                 SC_FULL,   // scc::log::TRACE
                                                 SC_DEBUG}; // scc::log::DBGTRACE

auto scc::stream_redirection::sync() -> int {
    if(level <= log_cfg.level) {
        auto timestr = time2string(sc_time_stamp());
        istringstream buf(str());
        string line;
        while(getline(buf, line)) {
            ::sc_report_handler::report(severity[static_cast<unsigned>(level)], "SystemC", line.c_str(),
                                        verbosity[static_cast<unsigned>(level)], "", 0);
        }
        str(string(""));
    }
    return 0; // Success
}

static std::mutex cfg_guard;
static void configure_logging() {
    std::lock_guard<mutex> lock(cfg_guard);
    static bool spdlog_initialized = false;
    if(!log_cfg.dont_create_broker)
        scc::init_cci("SCCBroker");
    if(log_cfg.install_handler) {
        if(!log_cfg.instance_based_log_levels || getenv("SCC_DISABLE_INSTANCE_BASED_LOGGING"))
            inst_based_logging() = false;
        sc_report_handler::set_verbosity_level(verbosity[static_cast<unsigned>(log_cfg.level)]);
        sc_report_handler::set_handler(report_handler);
        if(!spdlog_initialized) {
            spdlog::init_thread_pool(1024U,
                                     log_cfg.log_file_name.size() ? 2U : 1U); // queue with 8k items and 1 backing thread.
            log_cfg.console_logger = log_cfg.log_async ? spdlog::stdout_color_mt<spdlog::async_factory>("console_logger")
                                                       : spdlog::stdout_color_mt("console_logger");
            auto logger_fmt = log_cfg.print_severity ? "[%L] %v" : "%v";
            if(log_cfg.colored_output) {
                std::ostringstream os;
                os << "%^" << logger_fmt << "%$";
                log_cfg.console_logger->set_pattern(os.str());
            } else
                log_cfg.console_logger->set_pattern("[%L] %v");
            log_cfg.console_logger->flush_on(spdlog::level::warn);
            log_cfg.console_logger->set_level(spdlog::level::level_enum::trace);
            if(log_cfg.log_file_name.size()) {
                {
                    ofstream ofs;
                    ofs.open(log_cfg.log_file_name, ios::out | ios::trunc);
                }
                log_cfg.file_logger = log_cfg.log_async
                                          ? spdlog::basic_logger_mt<spdlog::async_factory>("file_logger", log_cfg.log_file_name)
                                          : spdlog::basic_logger_mt("file_logger", log_cfg.log_file_name);
                if(log_cfg.print_severity)
                    log_cfg.file_logger->set_pattern("[%8l] %v");
                else
                    log_cfg.file_logger->set_pattern("%v");
                log_cfg.file_logger->flush_on(spdlog::level::warn);
                log_cfg.file_logger->set_level(spdlog::level::level_enum::trace);
            }
            spdlog_initialized = true;
        } else {
            log_cfg.console_logger = spdlog::get("console_logger");
            if(log_cfg.log_file_name.size())
                log_cfg.file_logger = spdlog::get("file_logger");
        }
        if(log_cfg.log_filter_regex.size()) {
#ifdef USE_C_REGEX
            regcomp(&log_cfg.start_state, log_cfg.log_filter_regex.c_str(), REG_EXTENDED);
#else
            log_cfg.reg_ex = regex(log_cfg.log_filter_regex, regex::extended | regex::icase);
#endif
        }
    }
}

void scc::reinit_logging() { reinit_logging(log_cfg.level); }

void scc::reinit_logging(scc::log level) {
    if(log_cfg.install_handler)
        sc_report_handler::set_handler(report_handler);
    log_cfg.level = level;
    lut.clear();
    if(!log_cfg.instance_based_log_levels || getenv("SCC_DISABLE_INSTANCE_BASED_LOGGING"))
        inst_based_logging() = false;
    log_cfg.initialized = true;
}

bool scc::is_logging_initialized() { return log_cfg.initialized; }

void scc::init_logging(scc::log level, unsigned type_field_width, bool print_time) {
    log_cfg.msg_type_field_width = type_field_width;
    log_cfg.print_sys_time = print_time;
    log_cfg.level = level;
    configure_logging();
    log_cfg.initialized = true;
}

void scc::init_logging(const scc::LogConfig& log_config) {
    log_cfg = log_config;
    configure_logging();
    log_cfg.initialized = true;
}

void scc::set_logging_level(scc::log level) {
    log_cfg.level = level;
    sc_report_handler::set_verbosity_level(verbosity[static_cast<unsigned>(level)]);
    log_cfg.console_logger->set_level(
        static_cast<spdlog::level::level_enum>(SPDLOG_LEVEL_OFF - min<int>(SPDLOG_LEVEL_OFF, static_cast<int>(log_cfg.level))));
    log_cfg.initialized = true;
}

auto scc::get_logging_level() -> scc::log { return log_cfg.level; }

void scc::set_cycle_base(sc_time period) { log_cfg.cycle_base = period; }

auto scc::LogConfig::logLevel(scc::log level) -> scc::LogConfig& {
    this->level = level;
    return *this;
}

auto scc::LogConfig::msgTypeFieldWidth(unsigned width) -> scc::LogConfig& {
    this->msg_type_field_width = width;
    return *this;
}

auto scc::LogConfig::printSysTime(bool enable) -> scc::LogConfig& {
    this->print_sys_time = enable;
    return *this;
}

auto scc::LogConfig::printSimTime(bool enable) -> scc::LogConfig& {
    this->print_sim_time = enable;
    return *this;
}

auto scc::LogConfig::printDelta(bool enable) -> scc::LogConfig& {
    this->print_delta = enable;
    return *this;
}

auto scc::LogConfig::printSeverity(bool enable) -> scc::LogConfig& {
    this->print_severity = enable;
    return *this;
}

auto scc::LogConfig::logFileName(string&& name) -> scc::LogConfig& {
    this->log_file_name = name;
    return *this;
}

auto scc::LogConfig::logFileName(const string& name) -> scc::LogConfig& {
    this->log_file_name = name;
    return *this;
}

auto scc::LogConfig::coloredOutput(bool enable) -> scc::LogConfig& {
    this->colored_output = enable;
    return *this;
}

auto scc::LogConfig::logFilterRegex(string&& expr) -> scc::LogConfig& {
    this->log_filter_regex = expr;
    return *this;
}

auto scc::LogConfig::logFilterRegex(const string& expr) -> scc::LogConfig& {
    this->log_filter_regex = expr;
    return *this;
}

auto scc::LogConfig::logAsync(bool v) -> scc::LogConfig& {
    this->log_async = v;
    return *this;
}

auto scc::LogConfig::dontCreateBroker(bool v) -> scc::LogConfig& {
    this->dont_create_broker = v;
    return *this;
}

auto scc::LogConfig::reportOnlyFirstError(bool v) -> scc::LogConfig& {
    this->report_only_first_error = v;
    return *this;
}
auto scc::LogConfig::instanceBasedLogLevels(bool v) -> scc::LogConfig& {
    this->instance_based_log_levels = v;
    return *this;
}
auto scc::LogConfig::installHandler(bool v) -> scc::LogConfig& {
    this->install_handler = v;
    return *this;
}

auto scc::get_log_verbosity(char const* str) -> sc_core::sc_verbosity {
    if(inst_based_logging()) {
        auto it = lut.table.find(str);
        if(it != lut.table.end())
            return it->second;
        if(strchr(str, '.') == nullptr || sc_core::sc_get_current_object()) {
            string current_name = std::string(str);
            auto broker = sc_core::sc_get_current_object() ? cci::cci_get_broker() : cci::cci_get_global_broker(originator);
            while(true) {
                string param_name = (current_name.empty()) ? SCC_LOG_LEVEL_PARAM_NAME : current_name + "." SCC_LOG_LEVEL_PARAM_NAME;
                auto h = broker.get_param_handle(param_name);
                if(h.is_valid()) {
                    sc_core::sc_verbosity ret = verbosity.at(std::min<unsigned>(h.get_cci_value().get_int(), verbosity.size() - 1));
                    lut.insert(str, ret);
                    return ret;
                } else {
                    auto val = broker.get_preset_cci_value(param_name);
                    if(val.is_int()) {
                        sc_core::sc_verbosity ret = verbosity.at(std::min<unsigned>(val.get_int(), verbosity.size() - 1));
                        lut.insert(str, ret);
                        return ret;
                    } else {
                        if(current_name.empty()) {
                            sc_core::sc_verbosity ret =
                                static_cast<sc_core::sc_verbosity>(::sc_core::sc_report_handler::get_verbosity_level());
                            lut.insert(str, ret);
                            return ret;
                        }
                        auto pos = current_name.rfind(".");
                        if(pos == std::string::npos) {
                            current_name = "";
                        } else {
                            current_name = current_name.substr(0, pos);
                        }
                    }
                }
            }
        }
    }
    return static_cast<sc_core::sc_verbosity>(::sc_core::sc_report_handler::get_verbosity_level());
}
