/*
 * platform.cpp — host environment fingerprint, embedded in every result file.
 *
 * A published throughput number is only meaningful next to the machine it was
 * measured on (anti-cheat criterion #6: platform misreporting). Every field
 * here is best-effort — anything unavailable is reported as JSON null rather
 * than failing the run.
 */
#include "harness.h"
#include "sha256.h"

#include <sys/utsname.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

/* Read a whole file into a string ("" if it cannot be opened). */
std::string slurp(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* Run a shell command, capture trimmed stdout ("" on any failure). */
std::string run_cmd(const char* cmd) {
    FILE* p = popen(cmd, "r");
    if (!p) return {};
    std::string out;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

/* Value of the first "<key> ... : <value>" line in /proc-style text. */
std::string field(const std::string& text, const char* key) {
    std::istringstream ss(text);
    std::string line;
    size_t klen = std::strlen(key);
    while (std::getline(ss, line)) {
        if (line.compare(0, klen, key) != 0) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string v = line.substr(colon + 1);
        size_t a = v.find_first_not_of(" \t");
        size_t b = v.find_last_not_of(" \t\r");
        if (a != std::string::npos) return v.substr(a, b - a + 1);
    }
    return {};
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

/* JSON string literal, or the bare token null for an empty value. */
std::string jstr(const std::string& s) {
    return s.empty() ? "null" : ("\"" + json_escape(s) + "\"");
}

}  // namespace

std::string platform_fingerprint_json() {
    const std::string cpuinfo = slurp("/proc/cpuinfo");
    const std::string meminfo = slurp("/proc/meminfo");

    /* CPU model: x86 exposes "model name"; aarch64 typically does not. */
    std::string cpu_model = field(cpuinfo, "model name");
    if (cpu_model.empty()) cpu_model = field(cpuinfo, "Model");
    if (cpu_model.empty()) {
        std::string impl = field(cpuinfo, "CPU implementer");
        std::string part = field(cpuinfo, "CPU part");
        if (!impl.empty())
            cpu_model = "aarch64 (implementer " + impl + ", part " + part + ")";
    }

    int cores = 0;
    {
        std::istringstream ss(cpuinfo);
        std::string line;
        while (std::getline(ss, line))
            if (line.compare(0, 9, "processor") == 0) ++cores;
    }

    /* A digest of the raw /proc/cpuinfo — tamper-evident, platform-stable. */
    char cpu_hash[65] = {0};
    if (!cpuinfo.empty()) sha256_hex(cpuinfo.data(), cpuinfo.size(), cpu_hash);

    const std::string mem_total = field(meminfo, "MemTotal");

    struct utsname u {};
    std::string kernel, arch;
    if (uname(&u) == 0) {
        kernel = std::string(u.sysname) + " " + u.release;
        arch   = u.machine;
    }

    /* dmidecode needs root; AWS IMDS needs to be on EC2 — both best-effort. */
    std::string product = run_cmd("dmidecode -s system-product-name 2>/dev/null");
    std::string aws = run_cmd(
        "TOK=$(curl -s -X PUT 'http://169.254.169.254/latest/api/token' "
        "-H 'X-aws-ec2-metadata-token-ttl-seconds: 60' "
        "--connect-timeout 1 --max-time 1 2>/dev/null); "
        "curl -s -H \"X-aws-ec2-metadata-token: $TOK\" --connect-timeout 1 "
        "--max-time 1 'http://169.254.169.254/latest/meta-data/instance-type' "
        "2>/dev/null");
    if (aws.size() > 64 || aws.find('<') != std::string::npos) aws.clear();

    const std::string compiler =
#if defined(__clang__)
        "clang " __clang_version__;
#elif defined(__GNUC__)
        "gcc " __VERSION__;
#else
        "unknown";
#endif

    std::string j = "{";
    j += "\"cpu_model\": "         + jstr(cpu_model) + ", ";
    j += "\"cpu_cores\": "         + std::to_string(cores) + ", ";
    j += "\"cpuinfo_sha256\": "    + jstr(cpu_hash) + ", ";
    j += "\"mem_total\": "         + jstr(mem_total) + ", ";
    j += "\"kernel\": "            + jstr(kernel) + ", ";
    j += "\"arch\": "              + jstr(arch) + ", ";
    j += "\"system_product\": "    + jstr(product) + ", ";
    j += "\"aws_instance_type\": " + jstr(aws) + ", ";
    j += "\"compiler\": "          + jstr(compiler);
    j += "}";
    return j;
}
