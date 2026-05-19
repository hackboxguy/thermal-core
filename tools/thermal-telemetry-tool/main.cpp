/* tools/thermal-telemetry-tool/main.cpp
 *
 * Stage 19 -- host tool for the CH32V003 host-command channel (the
 * CH32_COMMAND=1 bench firmware, platform/ch32v003/). It speaks the
 * line-based ASCII command protocol over a serial port: it streams
 * the telemetry CSV, sets and reads PWM duty and tach RPM, toggles
 * the regulating control loop, and runs a PWM->RPM sweep that
 * produces the fan-health baseline table consumed by Stage 20.
 *
 * Pure C++17 + POSIX termios; no external dependencies.
 *
 *   thermal-telemetry-tool --device=/dev/ttyUSB0 --baud=115200 \
 *       --action=<log|pwmset|pwmget|rpmget|loop|pwmsweep|ping> [--value=...]
 *
 * The command protocol (platform/ch32v003/ch32_command.h): the tool
 * writes a command line; the firmware answers with a `+`-prefixed
 * line on success or `-err` on failure. Telemetry rows (which start
 * with a digit) and `#` diagnostics interleave and are skipped while
 * waiting for a response.
 */
#define _DEFAULT_SOURCE 1   /* expose cfmakeraw / CRTSCTS under -std=c++17 */

#include <termios.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

/* --- percent <-> raw 0..255 duty (the wire protocol carries raw) --- */
int pct_to_duty(int pct)
{
    if (pct < 0)   { pct = 0; }
    if (pct > 100) { pct = 100; }
    return (pct * 255 + 50) / 100;
}
int duty_to_pct(long duty)
{
    if (duty < 0)   { duty = 0; }
    if (duty > 255) { duty = 255; }
    return static_cast<int>((duty * 100 + 127) / 255);
}

std::optional<speed_t> baud_to_speed(int baud)
{
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return std::nullopt;
    }
}

/* A line-buffered serial port. Reads honour a millisecond deadline so
 * commands can time out and be retried. */
class SerialPort {
public:
    SerialPort() = default;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    ~SerialPort() { if (fd_ >= 0) { ::close(fd_); } }

    bool open_port(const std::string& dev, int baud)
    {
        auto sp = baud_to_speed(baud);
        if (!sp) {
            std::cerr << "error: unsupported baud " << baud
                      << " (use 9600/19200/38400/57600/115200/230400)\n";
            return false;
        }
        fd_ = ::open(dev.c_str(), O_RDWR | O_NOCTTY);
        if (fd_ < 0) {
            std::cerr << "error: cannot open " << dev << ": "
                      << std::strerror(errno) << "\n";
            return false;
        }
        termios tio{};
        if (tcgetattr(fd_, &tio) != 0) {
            std::cerr << "error: tcgetattr: " << std::strerror(errno)
                      << "\n";
            return false;
        }
        cfmakeraw(&tio);
        tio.c_cflag |= (CLOCAL | CREAD);
        tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);   /* 1 stop bit */
        tio.c_cflag &= ~static_cast<tcflag_t>(PARENB);   /* no parity  */
#ifdef CRTSCTS
        tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);  /* no HW flow */
#endif
        cfsetispeed(&tio, *sp);
        cfsetospeed(&tio, *sp);
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 1;                 /* 0.1 s read granularity */
        if (tcsetattr(fd_, TCSANOW, &tio) != 0) {
            std::cerr << "error: tcsetattr: " << std::strerror(errno)
                      << "\n";
            return false;
        }
        tcflush(fd_, TCIOFLUSH);
        return true;
    }

    bool write_line(const std::string& s)
    {
        std::string out = s + "\n";
        size_t off = 0;
        while (off < out.size()) {
            ssize_t w = ::write(fd_, out.data() + off, out.size() - off);
            if (w < 0) {
                if (errno == EINTR) { continue; }
                std::cerr << "error: serial write: "
                          << std::strerror(errno) << "\n";
                return false;
            }
            off += static_cast<size_t>(w);
        }
        return true;
    }

    /* Next complete line (terminators stripped), or nullopt if none
     * arrives within timeout_ms. */
    std::optional<std::string> read_line(int timeout_ms)
    {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        for (;;) {
            size_t nl = rxbuf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = rxbuf_.substr(0, nl);
                rxbuf_.erase(0, nl + 1);
                while (!line.empty() &&
                       (line.back() == '\r' || line.back() == '\0')) {
                    line.pop_back();
                }
                return line;
            }
            if (g_stop || clock::now() >= deadline) {
                return std::nullopt;
            }
            char buf[256];
            ssize_t r = ::read(fd_, buf, sizeof(buf));
            if (r > 0) {
                rxbuf_.append(buf, static_cast<size_t>(r));
            } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
                std::cerr << "error: serial read: "
                          << std::strerror(errno) << "\n";
                return std::nullopt;
            }
            /* r == 0: VTIME elapsed with no byte -- loop, re-check. */
        }
    }

private:
    int         fd_ = -1;
    std::string rxbuf_;
};

bool is_response(const std::string& line)
{
    return !line.empty() && (line[0] == '+' || line[0] == '-');
}

/* The trailing integer of a "+key <int>" response. */
std::optional<long> resp_value(const std::string& line)
{
    size_t sp = line.find(' ');
    if (sp == std::string::npos) { return std::nullopt; }
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(line.c_str() + sp + 1, &end, 10);
    if (errno != 0 || end == line.c_str() + sp + 1) {
        return std::nullopt;
    }
    return v;
}

/* Send `cmd`, return the firmware's `+`/`-` response line. Retries to
 * ride out the ~0.8 s DS18B20 conversion blackout that drops RX bytes
 * while the regulating loop is running; once the loop is bypassed the
 * firmware answers on the first attempt. */
std::optional<std::string> send_command(SerialPort& port,
                                        const std::string& cmd,
                                        int attempts = 10,
                                        int per_attempt_ms = 1500)
{
    for (int a = 0; a < attempts && !g_stop; ++a) {
        if (!port.write_line(cmd)) {
            return std::nullopt;
        }
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() +
                        std::chrono::milliseconds(per_attempt_ms);
        while (!g_stop) {
            auto now = clock::now();
            if (now >= deadline) { break; }   /* timed out: retry */
            int remain = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count());
            auto line = port.read_line(remain > 0 ? remain : 1);
            if (!line) { break; }
            if (is_response(*line)) { return line; }
            /* otherwise a telemetry / `#` line -- keep waiting */
        }
    }
    return std::nullopt;
}

/* Printed when the firmware does not answer -- the usual causes. */
void print_link_hint()
{
    std::cerr <<
"  check: the firmware is a CH32_COMMAND=1 build; --baud matches its\n"
"  CH32_TELEMETRY_BAUD; and the USB-serial adapter's TX is wired to\n"
"  the CH32 PD6 (RX) pin, with a common ground. The telemetry tap\n"
"  only needs the TX direction, so PD6 is easy to leave unwired.\n";
}

/* --- actions ---------------------------------------------------- */

int action_ping(SerialPort& port)
{
    auto r = send_command(port, "ping");
    if (r && *r == "+pong") {
        std::cout << "ok: firmware responding\n";
        return 0;
    }
    std::cerr << "error: no `+pong` from the firmware.\n";
    print_link_hint();
    return 1;
}

int action_loop(SerialPort& port, const std::string& v)
{
    if (v != "on" && v != "off") {
        std::cerr << "error: loop needs --value=on or --value=off\n";
        return 2;
    }
    auto r = send_command(port, "loop " + v);
    if (r && *r == "+loop " + v) {
        std::cout << "control loop " << v << "\n";
        return 0;
    }
    std::cerr << "error: `loop " << v << "` not acknowledged"
              << (r ? " (got: " + *r + ")" : " (no response)") << "\n";
    return 1;
}

int action_pwmset(SerialPort& port, int pct)
{
    if (pct < 0 || pct > 100) {
        std::cerr << "error: pwmset --value must be a percent 0..100\n";
        return 2;
    }
    int duty = pct_to_duty(pct);
    auto r = send_command(port, "pwmset " + std::to_string(duty));
    if (r && r->rfind("+pwm", 0) == 0) {
        std::cout << "pwm set to " << pct << "% (" << duty << "/255)\n";
        return 0;
    }
    if (r && *r == "-err") {
        std::cerr << "error: pwmset rejected -- bypass the loop first"
                     " (--action=loop --value=off)\n";
        return 1;
    }
    std::cerr << "error: pwmset not acknowledged (no response)\n";
    return 1;
}

int action_pwmget(SerialPort& port)
{
    auto r = send_command(port, "pwmget");
    auto v = r ? resp_value(*r) : std::nullopt;
    if (r && r->rfind("+pwm", 0) == 0 && v) {
        std::cout << "pwm: " << duty_to_pct(*v) << "% (" << *v
                  << "/255)\n";
        return 0;
    }
    std::cerr << "error: pwmget not acknowledged\n";
    return 1;
}

int action_rpmget(SerialPort& port)
{
    auto r = send_command(port, "rpmget");
    auto v = r ? resp_value(*r) : std::nullopt;
    if (r && r->rfind("+rpm", 0) == 0 && v) {
        std::cout << "rpm: " << *v << "\n";
        return 0;
    }
    std::cerr << "error: rpmget not acknowledged\n";
    return 1;
}

int action_log(SerialPort& port, const std::string& path)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot write " << path << "\n";
        return 1;
    }
    std::cerr << "logging telemetry to " << path
              << " -- Ctrl-C to stop\n";
    long  lines  = 0;
    bool  warned = false;
    auto  start  = std::chrono::steady_clock::now();
    while (!g_stop) {
        auto line = port.read_line(500);
        if (line) {
            out << *line << "\n";
            out.flush();
            ++lines;
        } else if (!warned && lines == 0 &&
                   std::chrono::steady_clock::now() - start >
                       std::chrono::seconds(5)) {
            std::cerr << "  (no telemetry yet -- check the baud rate, "
                         "and that the control loop is running)\n";
            warned = true;
        }
    }
    std::cerr << "\nstopped: " << lines << " line(s) written to "
              << path << "\n";
    return 0;
}

int action_pwmsweep(SerialPort& port, const std::string& path,
                     int dwell_s, int step)
{
    if (step < 1 || step > 100) {
        std::cerr << "error: --step must be 1..100\n";
        return 2;
    }
    std::cerr << "sweep: bypassing the control loop...\n";
    auto r = send_command(port, "loop off");
    if (!r || *r != "+loop off") {
        std::cerr << "error: could not enter bypass mode -- no "
                     "`+loop off` from the firmware.\n";
        print_link_hint();
        return 1;
    }

    std::ofstream out(path);
    if (!out) {
        std::cerr << "error: cannot write " << path << "\n";
        send_command(port, "loop on");
        return 1;
    }
    out << "pwm_pct,duty_0_255,rpm\n";

    int rc = 0;
    for (int pct = step; pct <= 100 && !g_stop; pct += step) {
        int  duty = pct_to_duty(pct);
        auto sr = send_command(port, "pwmset " + std::to_string(duty));
        if (!sr || sr->rfind("+pwm", 0) != 0) {
            std::cerr << "error: pwmset " << duty
                      << " failed -- aborting sweep\n";
            rc = 1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(dwell_s));
        auto rr  = send_command(port, "rpmget");
        auto rpm = rr ? resp_value(*rr) : std::nullopt;
        if (!rr || rr->rfind("+rpm", 0) != 0 || !rpm) {
            std::cerr << "error: rpmget failed at " << pct
                      << "% -- aborting sweep\n";
            rc = 1;
            break;
        }
        out << pct << "," << duty << "," << *rpm << "\n";
        out.flush();
        std::cerr << "  " << pct << "% (duty " << duty << ") -> "
                  << *rpm << " rpm\n";
    }

    /* Always restore regulation -- bypass leaves the fan unprotected. */
    send_command(port, "loop on");
    if (g_stop) {
        std::cerr << "sweep interrupted; control loop resumed\n";
    } else if (rc == 0) {
        std::cerr << "sweep complete: " << path
                  << " (control loop resumed)\n";
    } else {
        std::cerr << "sweep aborted; control loop resumed\n";
    }
    return rc;
}

void print_usage()
{
    std::cerr <<
"thermal-telemetry-tool -- host tool for the CH32V003 command channel\n"
"\n"
"  --device=PATH    serial device (e.g. /dev/ttyUSB0)   [required]\n"
"  --baud=N         baud rate, must match the firmware  [default 115200]\n"
"  --action=ACTION  one of the actions below            [required]\n"
"  --value=V        action argument (see below)\n"
"  --dwell=SECONDS  pwmsweep per-step settle time       [default 4]\n"
"  --step=PCT       pwmsweep step in percent            [default 1]\n"
"\n"
"Actions:\n"
"  ping                  check the firmware responds\n"
"  log       --value=CSV stream telemetry to a file (Ctrl-C to stop)\n"
"  loop      --value=on|off  resume / bypass the control loop\n"
"  pwmset    --value=PCT set fan duty to PCT% (needs loop off)\n"
"  pwmget                read the current fan duty\n"
"  rpmget                read the current tach RPM\n"
"  pwmsweep  --value=CSV sweep duty and log a PWM->RPM table\n"
"\n"
"Example -- capture a fan baseline:\n"
"  thermal-telemetry-tool --device=/dev/ttyUSB0 --baud=115200 \\\n"
"      --action=pwmsweep --value=/tmp/nf-a8-sweep-table.csv\n";
}

}  /* namespace */

int main(int argc, char** argv)
{
    std::signal(SIGINT, on_sigint);

    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) != 0) {
            std::cerr << "error: unexpected argument `" << a << "`\n\n";
            print_usage();
            return 2;
        }
        size_t eq = a.find('=');
        if (eq == std::string::npos) {
            args[a.substr(2)] = "";
        } else {
            args[a.substr(2, eq - 2)] = a.substr(eq + 1);
        }
    }

    if (args.count("help") || args.empty()) {
        print_usage();
        return args.empty() ? 2 : 0;
    }

    const std::string device = args.count("device") ? args["device"] : "";
    const std::string action = args.count("action") ? args["action"] : "";
    const std::string value  = args.count("value")  ? args["value"]  : "";
    const int baud  = args.count("baud")  ? std::atoi(args["baud"].c_str())
                                          : 115200;
    const int dwell = args.count("dwell") ? std::atoi(args["dwell"].c_str())
                                          : 4;
    const int step  = args.count("step")  ? std::atoi(args["step"].c_str())
                                          : 1;

    if (device.empty()) {
        std::cerr << "error: --device is required\n\n";
        print_usage();
        return 2;
    }
    if (action.empty()) {
        std::cerr << "error: --action is required\n\n";
        print_usage();
        return 2;
    }

    SerialPort port;
    if (!port.open_port(device, baud)) {
        return 1;
    }

    if (action == "ping")   { return action_ping(port); }
    if (action == "pwmget") { return action_pwmget(port); }
    if (action == "rpmget") { return action_rpmget(port); }
    if (action == "loop")   { return action_loop(port, value); }
    if (action == "pwmset") {
        return action_pwmset(port, std::atoi(value.c_str()));
    }
    if (action == "log") {
        if (value.empty()) {
            std::cerr << "error: log needs --value=<output.csv>\n";
            return 2;
        }
        return action_log(port, value);
    }
    if (action == "pwmsweep") {
        if (value.empty()) {
            std::cerr << "error: pwmsweep needs --value=<output.csv>\n";
            return 2;
        }
        return action_pwmsweep(port, value, dwell, step);
    }

    std::cerr << "error: unknown action `" << action << "`\n\n";
    print_usage();
    return 2;
}
