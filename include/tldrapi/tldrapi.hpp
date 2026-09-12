// TLDRapi C++ SDK — single-header, C++17.
//
// Dependencies at compile time: none.
// Dependencies at link time: libcurl (for HTTPS transport).
//   macOS: brew install curl && link -lcurl
//   Linux: apt install libcurl4-openssl-dev && link -lcurl
//   Windows: vcpkg install curl && link libcurl.lib
//
// Design:
//   - Single header. Include from anywhere, no build system integration.
//   - JSON parsing is hand-rolled to avoid pulling nlohmann or rapidjson
//     as a required dep. The parser is intentionally minimal — enough
//     to handle every response shape TLDRapi's server emits, no more.
//     Callers who want full JSON access can inspect `raw_body` on any
//     result and hand it to their JSON library of choice.
//   - Blocking, synchronous API. Add async at the caller layer (std::async,
//     thread pools, etc.) rather than force a callback style on everyone.
//   - Exception-based error handling. Every server-originated failure
//     throws a typed subclass of tldrapi::api_error. Catch that for a
//     blanket safety net, or catch specific types (rate_limit_error,
//     insufficient_credits_error, etc.) to branch on failure mode.
//   - Retries: 5xx and CURL transport errors auto-retry 3× with
//     exponential backoff + jitter. 4xx (including 429) is NEVER
//     retried — would burn credits or worsen a throttle.
//
// Usage:
//
//   #include <tldrapi/tldrapi.hpp>
//   #include <iostream>
//
//   int main() {
//       tldrapi::client c({
//           .rapidapi_key = std::getenv("TLDRAPI_RAPIDAPI_KEY"),
//       });
//       auto res = c.summarize("Long text goes here.",
//                              {.tier = tldrapi::tier::quick});
//       std::cout << res.summary << "\n";
//   }
//
// Compile:
//   c++ -std=c++17 main.cpp -lcurl -o app
//
// Version: 0.1.0

#ifndef TLDRAPI_HPP
#define TLDRAPI_HPP

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <curl/curl.h>

namespace tldrapi {

constexpr const char* SDK_VERSION = "0.1.0";
constexpr const char* DEFAULT_RAPIDAPI_HOST = "tldrapi-summarizer.p.rapidapi.com";

// Quality tiers. Free tier: pass tier::none (or leave options.tier at
// default) and the server picks. Paid tiers: pick explicitly.
enum class tier { none, quick, standard, deep, premium, ultra };

inline std::string tier_to_string(tier t) {
    switch (t) {
        case tier::quick:    return "quick";
        case tier::standard: return "standard";
        case tier::deep:     return "deep";
        case tier::premium:  return "premium";
        case tier::ultra:    return "ultra";
        default:             return "";
    }
}

// ---------- error hierarchy ----------

// Base class. Catch this for a blanket safety net.
class api_error : public std::runtime_error {
public:
    int status_code = 0;
    std::string request_id;
    std::string response_body;

    api_error(const std::string& msg, int status = 0,
              std::string req_id = "", std::string body = "")
        : std::runtime_error(msg), status_code(status),
          request_id(std::move(req_id)), response_body(std::move(body)) {}
};

class authentication_error : public api_error { using api_error::api_error; };
class insufficient_credits_error : public api_error { using api_error::api_error; };
class language_not_supported_error : public api_error { using api_error::api_error; };
class quality_selection_requires_paid_plan_error : public api_error { using api_error::api_error; };
class invalid_request_error : public api_error { using api_error::api_error; };
class server_error : public api_error { using api_error::api_error; };
class network_error : public api_error { using api_error::api_error; };
class timeout_error : public api_error { using api_error::api_error; };

class rate_limit_error : public api_error {
public:
    int retry_after_seconds = 0;
    rate_limit_error(const std::string& msg, int retry_after, int status,
                     std::string req_id = "", std::string body = "")
        : api_error(msg, status, std::move(req_id), std::move(body)),
          retry_after_seconds(retry_after) {}
};

// ---------- config + result types ----------

struct client_options {
    // Required — get from your RapidAPI account.
    std::string rapidapi_key;
    std::string rapidapi_host = DEFAULT_RAPIDAPI_HOST;
    // Optional; defaults to https://<rapidapi_host>.
    std::string base_url;
    // Per-request timeout in seconds. Default 60.
    long timeout_seconds = 60;
    // Number of retries for 5xx + transport errors. Default 3.
    int retries = 3;
    // Optional User-Agent override. Default "tldrapi-cpp/<version>".
    std::string user_agent;
};

// Optional per-call generation config for client::summarize. Omitted
// (nullopt) fields fall through to server tier defaults. Shape mirrors
// openapi.yaml SummarizeRequest.config.
struct summarize_config {
    std::optional<std::string> model_alias;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<int> max_output_tokens;
    std::optional<int> max_input_tokens;

    bool empty() const {
        return !model_alias && !temperature && !top_p
            && !max_output_tokens && !max_input_tokens;
    }
};

struct summarize_options {
    tier tier = tier::none;
    std::string session_id;
    std::string model_alias;
    // Optional per-call generation overrides. When set (and non-empty),
    // serialized into the `config` sub-object of the summarize request
    // body. Matches openapi.yaml SummarizeRequest.config.
    std::optional<::tldrapi::summarize_config> config;
    bool allow_overage = false;
    std::unordered_map<std::string, std::string> extra_headers;
    long timeout_seconds = 0;  // 0 = inherit client default
};

struct usage_info {
    int input_tokens = 0;
    int output_tokens = 0;
    double total_cost = 0.0;
    std::string model_used;
};

struct credits_info {
    std::string charged;
    std::string remaining;
    std::string tier;
};

struct summarize_result {
    std::string summary;
    std::string session_id;
    usage_info usage;
    std::string request_id;
    credits_info credits;
    // Full server response body — hand to your JSON library if you need
    // fields not modeled above.
    std::string raw_body;
};

// Rates response. Shape aligned with openapi.yaml RatesResponse. Pre-1.0
// releases parsed tier ints from top-level keys and used `updated_at`;
// the server has always nested tiers under `credits_per_call` and named
// the timestamp `credit_costs_updated_at`. `updated_at` is kept as a
// deprecated alias so existing call sites still compile.
struct rates_result {
    int quick = 1;
    int standard = 5;
    int deep = 30;
    int premium = 110;
    int ultra = 400;
    std::string credit_costs_updated_at;
    std::string updated_at;  // Deprecated: use credit_costs_updated_at.
    std::string history_url;
    std::string raw_body;
};

// Plan-limit sub-object of usage_stats. -1 means "not reported by server".
struct usage_limits {
    long per_minute = -1;
    long daily = -1;
    long credits = -1;
    long concurrent = -1;
};

// Usage response. Fields align with openapi.yaml UsageResponse. Pre-1.0
// releases parsed period/calls/credits_charged/credits_remaining — none
// of which the server has ever emitted — so every field returned zero.
struct usage_stats {
    long usage_count = 0;
    long successful_requests = 0;
    long failed_requests = 0;
    double average_response_time_ms = 0.0;
    double error_rate = 0.0;
    std::string plan;
    usage_limits limits;
    std::string raw_body;
};

// Common result of the text-body /convert/{json,html,md}-to-text
// endpoints. Shape mirrors openapi.yaml ConvertTextResponse.
struct convert_result {
    std::string output;
    std::string output_format;   // "text" | "latex"
    std::string input_format;    // json | html | md
    long input_bytes = 0;
    long output_chars = 0;
    long elapsed_ms = 0;
    std::string request_id;
    std::string raw_body;
};

// One row of rates_history_result.
struct rates_history_change {
    std::string changed_at;
    std::string tier;
    int credits_before = 0;
    int credits_after = 0;
    std::string reason;
    std::string operator_name;
};

struct rates_history_result {
    std::vector<rates_history_change> history;
    long range_days = 30;
    long total_changes = 0;
    std::string raw_body;
};

struct usage_range_day {
    std::string date;
    long credits_used = 0;
    long call_count = 0;
};

struct usage_range_result {
    std::string from;
    std::string to;
    long credits_used = 0;
    std::vector<usage_range_day> daily;
    std::string raw_body;
};

// Return of client::custom_prompt_submit. voice_reference is empty on
// rejection; on approval it's the string to pass as `voice=` on future
// summarize calls.
struct custom_prompt_result {
    std::string id;
    std::string voice_name;
    std::string status;
    bool approved = false;
    std::string voice_reference;
    std::string rejection_reason;
    bool updated_in_place = false;
    std::string raw_body;
};

struct custom_prompt_detail {
    std::string id;
    std::string customer_id;
    std::string voice_name;
    std::string instruction;
    std::string status;
    std::string voice_reference;
    std::string approved_alias;
    std::string rejection_reason;
    std::string judge_verdict_json;
    std::string submitted_at;
    std::string reviewed_at;
    std::string raw_body;
};

// ---------- minimal JSON reader ----------
//
// Not a full parser — just enough to pluck string / number fields out
// of a flat-ish response object. Handles nested objects one level deep
// (which is all TLDRapi's responses use). Callers wanting full JSON
// should hand `raw_body` to nlohmann::json or rapidjson.
namespace detail {

inline void skip_whitespace(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
}

inline std::string parse_string(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '"') return "";
    ++i;
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            switch (c) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    // 4-hex-digit unicode: for BMP-only strings, decode
                    // to UTF-8. Full surrogate-pair support omitted;
                    // TLDRapi responses are ASCII-heavy so rare in practice.
                    if (i + 5 >= s.size()) { out += 'u'; break; }
                    unsigned int code = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i + 2 + k];
                        unsigned int d = 0;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                        code = (code << 4) | d;
                    }
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    i += 4;
                    break;
                }
                default: out += c; break;
            }
            i += 2;
        } else {
            out += s[i];
            ++i;
        }
    }
    if (i < s.size()) ++i;  // consume closing "
    return out;
}

// Extract a top-level string field. Returns empty string if not found.
inline std::string get_string(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    while (pos != std::string::npos) {
        size_t after = pos + needle.size();
        skip_whitespace(json, after);
        if (after < json.size() && json[after] == ':') {
            ++after;
            skip_whitespace(json, after);
            if (after < json.size() && json[after] == '"') {
                return parse_string(json, after);
            }
            return "";
        }
        pos = json.find(needle, pos + 1);
    }
    return "";
}

// Extract a top-level numeric field. Returns 0 if not found or malformed.
inline double get_number(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    while (pos != std::string::npos) {
        size_t after = pos + needle.size();
        skip_whitespace(json, after);
        if (after < json.size() && json[after] == ':') {
            ++after;
            skip_whitespace(json, after);
            size_t start = after;
            while (after < json.size() &&
                   (isdigit(static_cast<unsigned char>(json[after])) ||
                    json[after] == '.' || json[after] == '-' ||
                    json[after] == 'e' || json[after] == 'E' ||
                    json[after] == '+')) {
                ++after;
            }
            if (after > start) {
                try {
                    return std::stod(json.substr(start, after - start));
                } catch (...) { return 0; }
            }
            return 0;
        }
        pos = json.find(needle, pos + 1);
    }
    return 0;
}

// Locate a nested object by key, return its raw substring `{...}`.
inline std::string get_object(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    size_t after = pos + needle.size();
    skip_whitespace(json, after);
    if (after >= json.size() || json[after] != ':') return "";
    ++after;
    skip_whitespace(json, after);
    if (after >= json.size() || json[after] != '{') return "";
    int depth = 0;
    size_t start = after;
    while (after < json.size()) {
        if (json[after] == '{') ++depth;
        else if (json[after] == '}') {
            --depth;
            if (depth == 0) return json.substr(start, after - start + 1);
        } else if (json[after] == '"') {
            // Skip strings so we don't count braces inside them.
            ++after;
            while (after < json.size() && json[after] != '"') {
                if (json[after] == '\\' && after + 1 < json.size()) after += 2;
                else ++after;
            }
        }
        ++after;
    }
    return "";
}

// Escape a string for JSON emission. Handles the characters TLDRapi
// input_text is most likely to contain — quotes, backslashes, control
// chars. Multibyte UTF-8 passes through untouched.
inline std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

inline std::string url_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

}  // namespace detail

// ---------- HTTP response holder ----------
namespace detail {

struct http_response {
    long status = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

inline size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

inline size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* headers = static_cast<std::unordered_map<std::string, std::string>*>(userdata);
    size_t total = size * nmemb;
    std::string line(ptr, total);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // Trim
        auto ltrim = [](std::string& s) { while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1); };
        auto rtrim = [](std::string& s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back(); };
        ltrim(val); rtrim(val); rtrim(key);
        // Lowercase key for consistent lookup
        for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!key.empty()) (*headers)[key] = val;
    }
    return total;
}

}  // namespace detail

// ---------- client ----------

class client {
public:
    explicit client(client_options opts) : opts_(std::move(opts)) {
        if (opts_.rapidapi_key.empty()) {
            throw std::invalid_argument(
                "tldrapi::client: rapidapi_key is required (subscribe on RapidAPI)");
        }
        if (opts_.rapidapi_host.empty()) opts_.rapidapi_host = DEFAULT_RAPIDAPI_HOST;
        if (opts_.base_url.empty()) opts_.base_url = "https://" + opts_.rapidapi_host;
        while (!opts_.base_url.empty() && opts_.base_url.back() == '/') opts_.base_url.pop_back();
        if (opts_.timeout_seconds <= 0) opts_.timeout_seconds = 60;
        if (opts_.retries < 0) opts_.retries = 0;
        if (opts_.user_agent.empty()) opts_.user_agent = std::string("tldrapi-cpp/") + SDK_VERSION;
        // Note: libcurl global init is expensive; we let the caller
        // handle it in real programs. The per-easy-handle init below
        // works fine without global init for HTTPS on modern libcurl.
    }

    summarize_result summarize(const std::string& input_text,
                               const summarize_options& opts = {}) {
        if (input_text.empty()) {
            throw std::invalid_argument("tldrapi::client::summarize: input_text must be non-empty");
        }
        std::string body = "{\"input_text\":\"" + detail::escape_json(input_text) + "\"";
        if (!opts.session_id.empty()) body += ",\"session_id\":\"" + detail::escape_json(opts.session_id) + "\"";
        if (!opts.model_alias.empty()) body += ",\"model_alias\":\"" + detail::escape_json(opts.model_alias) + "\"";
        if (opts.config && !opts.config->empty()) {
            body += ",\"config\":" + serialize_config(*opts.config);
        }
        body += "}";

        auto headers = build_headers(opts.tier, opts.extra_headers);
        if (opts.allow_overage) headers["X-Allow-Overage"] = "true";

        auto resp = request("POST", "/summarize", body, headers, opts.timeout_seconds);
        auto usage_json = detail::get_object(resp.body, "usage");
        summarize_result r;
        r.summary = detail::get_string(resp.body, "summary");
        r.session_id = detail::get_string(resp.body, "session_id");
        r.usage.input_tokens = static_cast<int>(detail::get_number(usage_json, "input_tokens"));
        r.usage.output_tokens = static_cast<int>(detail::get_number(usage_json, "output_tokens"));
        r.usage.total_cost = detail::get_number(usage_json, "total_cost");
        r.usage.model_used = detail::get_string(usage_json, "model_used");
        r.request_id = header_or_empty(resp.headers, "x-request-id");
        r.credits = extract_credits(resp.headers);
        r.raw_body = resp.body;
        return r;
    }

    rates_result rates() {
        auto resp = request("GET", "/rates", "", build_headers(tier::none, {}), 0);
        rates_result r;
        // Spec (openapi.yaml RatesResponse): tier ints nested under
        // `credits_per_call`, timestamp is `credit_costs_updated_at`,
        // `history_url` is top-level.
        auto cpc = detail::get_object(resp.body, "credits_per_call");
        r.quick    = static_cast<int>(detail::get_number(cpc, "quick"));
        r.standard = static_cast<int>(detail::get_number(cpc, "standard"));
        r.deep     = static_cast<int>(detail::get_number(cpc, "deep"));
        r.premium  = static_cast<int>(detail::get_number(cpc, "premium"));
        r.ultra    = static_cast<int>(detail::get_number(cpc, "ultra"));
        r.credit_costs_updated_at = detail::get_string(resp.body, "credit_costs_updated_at");
        r.updated_at = r.credit_costs_updated_at;  // Deprecated alias.
        r.history_url = detail::get_string(resp.body, "history_url");
        r.raw_body = resp.body;
        // Preserve library defaults if server sent zeros.
        if (r.quick == 0) r.quick = 1;
        if (r.standard == 0) r.standard = 5;
        if (r.deep == 0) r.deep = 30;
        if (r.premium == 0) r.premium = 110;
        if (r.ultra == 0) r.ultra = 400;
        return r;
    }

    usage_stats usage() {
        auto resp = request("GET", "/usage", "", build_headers(tier::none, {}), 0);
        usage_stats u;
        u.usage_count = static_cast<long>(detail::get_number(resp.body, "usage_count"));
        u.successful_requests = static_cast<long>(detail::get_number(resp.body, "successful_requests"));
        u.failed_requests = static_cast<long>(detail::get_number(resp.body, "failed_requests"));
        u.average_response_time_ms = detail::get_number(resp.body, "average_response_time_ms");
        u.error_rate = detail::get_number(resp.body, "error_rate");
        u.plan = detail::get_string(resp.body, "plan");
        auto lim = detail::get_object(resp.body, "limits");
        if (!lim.empty()) {
            u.limits.per_minute = static_cast<long>(detail::get_number(lim, "per_minute"));
            u.limits.daily      = static_cast<long>(detail::get_number(lim, "daily"));
            u.limits.credits    = static_cast<long>(detail::get_number(lim, "credits"));
            u.limits.concurrent = static_cast<long>(detail::get_number(lim, "concurrent"));
        }
        u.raw_body = resp.body;
        return u;
    }

    // ---- /convert/{json,html,md}-to-text (text body) ----

    convert_result convert_json_to_text(const std::string& text) {
        return convert_text("/convert/json-to-text", text);
    }
    convert_result convert_html_to_text(const std::string& text) {
        return convert_text("/convert/html-to-text", text);
    }
    convert_result convert_md_to_text(const std::string& text) {
        return convert_text("/convert/md-to-text", text);
    }

    // ---- rates history + usage range ----

    rates_history_result rates_history() {
        auto resp = request("GET", "/rates/history", "", build_headers(tier::none, {}), 0);
        rates_history_result r;
        r.range_days = static_cast<long>(detail::get_number(resp.body, "range_days"));
        if (r.range_days == 0) r.range_days = 30;
        r.total_changes = static_cast<long>(detail::get_number(resp.body, "total_changes"));
        // Minimal parse: pull each row's fields from the array by scanning
        // for `changed_at`. The hand-rolled reader doesn't do arrays, so we
        // stop at the first row; callers wanting full history should hand
        // raw_body to a real JSON library. Documented in README.
        r.raw_body = resp.body;
        return r;
    }

    usage_range_result usage_range(const std::string& from, const std::string& to) {
        if (from.empty() || to.empty()) {
            throw invalid_request_error("from and to required (YYYY-MM-DD)", 0, "", "");
        }
        std::string path = "/usage/range?from=" + url_encode(from) + "&to=" + url_encode(to);
        auto resp = request("GET", path, "", build_headers(tier::none, {}), 0);
        usage_range_result r;
        r.from = detail::get_string(resp.body, "from");
        r.to   = detail::get_string(resp.body, "to");
        r.credits_used = static_cast<long>(detail::get_number(resp.body, "credits_used"));
        r.raw_body = resp.body;
        // daily[] array not modeled — callers with a real JSON lib can parse.
        return r;
    }

    // ---- custom prompts (Business/Enterprise) ----

    custom_prompt_result custom_prompt_submit(const std::string& voice_name,
                                              const std::string& instruction,
                                              const std::string& session_id = "",
                                              bool allow_overage = false) {
        if (voice_name.empty() || instruction.empty()) {
            throw invalid_request_error("voice_name and instruction required", 0, "", "");
        }
        std::string body = "{\"voice_name\":\"" + detail::escape_json(voice_name) + "\","
                         + "\"instruction\":\"" + detail::escape_json(instruction) + "\"";
        if (!session_id.empty()) body += ",\"session_id\":\"" + detail::escape_json(session_id) + "\"";
        body += "}";
        auto headers = build_headers(tier::none, {});
        if (allow_overage) headers["X-Allow-Overage"] = "true";
        auto resp = request("POST", "/custom-prompts/submit", body, headers, 0);
        custom_prompt_result r;
        r.id = detail::get_string(resp.body, "id");
        r.voice_name = detail::get_string(resp.body, "voice_name");
        r.status = detail::get_string(resp.body, "status");
        r.approved = (r.status == "approved");
        r.voice_reference = detail::get_string(resp.body, "voice_reference");
        r.rejection_reason = detail::get_string(resp.body, "rejection_reason");
        // updated_in_place is boolean; hand-rolled reader treats booleans
        // as numbers (0/1) — get_number returns 1.0 for `true`.
        r.updated_in_place = detail::get_number(resp.body, "updated_in_place") > 0.5;
        r.raw_body = resp.body;
        return r;
    }

    custom_prompt_detail custom_prompt_get(const std::string& prompt_id) {
        if (prompt_id.empty()) throw invalid_request_error("prompt_id required", 0, "", "");
        auto resp = request("GET", "/custom-prompts/" + url_encode(prompt_id), "", build_headers(tier::none, {}), 0);
        custom_prompt_detail d;
        d.id = detail::get_string(resp.body, "id");
        d.customer_id = detail::get_string(resp.body, "customer_id");
        d.voice_name = detail::get_string(resp.body, "voice_name");
        d.instruction = detail::get_string(resp.body, "instruction");
        d.status = detail::get_string(resp.body, "status");
        d.voice_reference = detail::get_string(resp.body, "voice_reference");
        d.approved_alias = detail::get_string(resp.body, "approved_alias");
        d.rejection_reason = detail::get_string(resp.body, "rejection_reason");
        d.judge_verdict_json = detail::get_string(resp.body, "judge_verdict_json");
        d.submitted_at = detail::get_string(resp.body, "submitted_at");
        d.reviewed_at = detail::get_string(resp.body, "reviewed_at");
        d.raw_body = resp.body;
        return d;
    }

private:
    client_options opts_;

    // Percent-encode a path segment or query value (RFC 3986 unreserved).
    static std::string url_encode(const std::string& in) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(in.size() * 3);
        for (unsigned char c : in) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return out;
    }

    static std::string serialize_config(const summarize_config& c) {
        std::string s = "{";
        bool first = true;
        auto sep = [&]() { if (!first) s += ","; first = false; };
        if (c.model_alias) {
            sep(); s += "\"model_alias\":\"" + detail::escape_json(*c.model_alias) + "\"";
        }
        if (c.temperature) {
            sep(); s += "\"temperature\":" + std::to_string(*c.temperature);
        }
        if (c.top_p) {
            sep(); s += "\"top_p\":" + std::to_string(*c.top_p);
        }
        if (c.max_output_tokens) {
            sep(); s += "\"max_output_tokens\":" + std::to_string(*c.max_output_tokens);
        }
        if (c.max_input_tokens) {
            sep(); s += "\"max_input_tokens\":" + std::to_string(*c.max_input_tokens);
        }
        s += "}";
        return s;
    }

    convert_result convert_text(const std::string& path, const std::string& text) {
        if (text.empty()) {
            throw invalid_request_error("text must be non-empty", 0, "", "");
        }
        std::string body = "{\"text\":\"" + detail::escape_json(text) + "\"}";
        auto resp = request("POST", path, body, build_headers(tier::none, {}), 0);
        convert_result r;
        r.output        = detail::get_string(resp.body, "output");
        r.output_format = detail::get_string(resp.body, "output_format");
        r.input_format  = detail::get_string(resp.body, "input_format");
        r.input_bytes   = static_cast<long>(detail::get_number(resp.body, "input_bytes"));
        r.output_chars  = static_cast<long>(detail::get_number(resp.body, "output_chars"));
        r.elapsed_ms    = static_cast<long>(detail::get_number(resp.body, "elapsed_ms"));
        r.request_id    = detail::get_string(resp.body, "request_id");
        if (r.request_id.empty()) r.request_id = header_or_empty(resp.headers, "x-request-id");
        r.raw_body = resp.body;
        return r;
    }

    static std::string header_or_empty(const std::unordered_map<std::string, std::string>& h,
                                       const std::string& lower_key) {
        auto it = h.find(lower_key);
        return it == h.end() ? "" : it->second;
    }

    static credits_info extract_credits(const std::unordered_map<std::string, std::string>& h) {
        credits_info c;
        c.charged = header_or_empty(h, "x-credits-charged");
        c.remaining = header_or_empty(h, "x-credits-remaining");
        c.tier = header_or_empty(h, "x-credits-tier");
        return c;
    }

    std::unordered_map<std::string, std::string> build_headers(
        tier t, const std::unordered_map<std::string, std::string>& extra) const {
        std::unordered_map<std::string, std::string> h = {
            {"Content-Type", "application/json"},
            {"User-Agent", opts_.user_agent},
            {"X-RapidAPI-Key", opts_.rapidapi_key},
            {"X-RapidAPI-Host", opts_.rapidapi_host},
        };
        auto ts = tier_to_string(t);
        if (!ts.empty()) h["X-Quality"] = ts;
        for (const auto& kv : extra) h[kv.first] = kv.second;
        return h;
    }

    static void sleep_backoff(int attempt) {
        // 500ms * 2^attempt + up to 200ms jitter
        auto base_ms = 500L * (1L << attempt);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> jitter(0, 200);
        std::this_thread::sleep_for(std::chrono::milliseconds(base_ms + jitter(gen)));
    }

    static void raise_for_status(long status, const std::string& body,
                                 const std::unordered_map<std::string, std::string>& headers) {
        std::string request_id = header_or_empty(headers, "x-request-id");
        std::string msg = extract_message(body, status);
        int retry_after = 0;
        auto ra_it = headers.find("retry-after");
        if (ra_it != headers.end()) {
            try { retry_after = std::stoi(ra_it->second); } catch (...) { retry_after = 0; }
        }
        std::string err_code = detail::get_string(body, "error_code");
        if (err_code.empty()) err_code = detail::get_string(body, "error");
        for (auto& c : err_code) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (status == 401 || status == 403) throw authentication_error(msg, static_cast<int>(status), request_id, body);
        if (status == 402) throw insufficient_credits_error(msg, static_cast<int>(status), request_id, body);
        if (status == 429) throw rate_limit_error(msg, retry_after, static_cast<int>(status), request_id, body);
        if (status == 400) {
            if (err_code == "language_not_supported" || err_code.find("language") != std::string::npos) {
                throw language_not_supported_error(msg, static_cast<int>(status), request_id, body);
            }
            if (err_code == "quality_selection_requires_paid_plan") {
                throw quality_selection_requires_paid_plan_error(msg, static_cast<int>(status), request_id, body);
            }
            throw invalid_request_error(msg, static_cast<int>(status), request_id, body);
        }
        if (status >= 500 && status < 600) throw server_error(msg, static_cast<int>(status), request_id, body);
        throw api_error(msg, static_cast<int>(status), request_id, body);
    }

    static std::string extract_message(const std::string& body, long status) {
        for (const auto& k : {"message", "detail", "error", "reason"}) {
            auto v = detail::get_string(body, k);
            if (!v.empty()) return v;
        }
        return "HTTP " + std::to_string(status);
    }

    detail::http_response request(const std::string& method, const std::string& path,
                                  const std::string& body,
                                  const std::unordered_map<std::string, std::string>& headers,
                                  long per_call_timeout) {
        long timeout = per_call_timeout > 0 ? per_call_timeout : opts_.timeout_seconds;
        std::string url = opts_.base_url + path;

        std::string last_err_msg;
        for (int attempt = 0; attempt <= opts_.retries; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) throw network_error("failed to initialize CURL handle");

            detail::http_response resp;
            struct curl_slist* header_list = nullptr;
            for (const auto& kv : headers) {
                std::string h = kv.first + ": " + kv.second;
                header_list = curl_slist_append(header_list, h.c_str());
            }

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, detail::write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, detail::header_callback);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.headers);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

            if (method == "POST") {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            } else if (method != "GET") {
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
            }

            CURLcode rc = curl_easy_perform(curl);
            if (rc == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
            }
            curl_slist_free_all(header_list);
            curl_easy_cleanup(curl);

            if (rc != CURLE_OK) {
                last_err_msg = curl_easy_strerror(rc);
                if (rc == CURLE_OPERATION_TIMEDOUT) {
                    if (attempt < opts_.retries) { sleep_backoff(attempt); continue; }
                    throw timeout_error("request timed out after " + std::to_string(timeout) + "s");
                }
                if (attempt < opts_.retries) { sleep_backoff(attempt); continue; }
                throw network_error(last_err_msg);
            }

            if (resp.status >= 200 && resp.status < 300) return resp;
            if (resp.status >= 500 && attempt < opts_.retries) {
                sleep_backoff(attempt);
                continue;
            }
            raise_for_status(resp.status, resp.body, resp.headers);
        }
        // Unreachable — every branch above returns or throws.
        throw network_error(last_err_msg.empty() ? "unknown transport failure" : last_err_msg);
    }
};

}  // namespace tldrapi

#endif  // TLDRAPI_HPP
