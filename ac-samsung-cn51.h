// Samsung AC Wi-Fi kit protocol on CN51 (original module SWL-B70F), 9600 8N1.
// Protocol from kumy's sniff: https://reverseengineering.stackexchange.com/q/25786
//
// Frame: D0 C0 02 <len> 00 00 00 00 00 <counter> FE <type hi> <type lo> <payload len> <regs> <xor> E0
//   len = total length - 4, xor = XOR of every byte before it.
//   regs = repeated <id> <len> <value...>.
// The module starts with 00 FC, then 1204 (enable). Every message is acked by echoing it with
// type lo + 1 (1204 -> 1205, 1206 -> 1207, ...). Odd type lo = ack/response, never acked.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ac_cn51 {

static const char *const TAG = "cn51";

struct State {
  int power = -1;         // reg 0x02 (group 12): 0x0F on, 0xF0 off
  int opmode = -1;        // reg 0x43 (group 12): 0x12 cool, 0x22 dry, 0x32 fan, 0x42 heat, 0xE2 auto
  int comode = -1;        // reg 0x44 (group 12): 0x12 off, 0x52 quiet, ...
  int direction = -1;     // reg 0x63 (group 12)
  int sleep = -1;         // reg 0x73 (group 12): good'sleep off timer, in half hours (0 = off)
  int autoclean = -1;     // reg 0x32 (group 13): 0x22 on, 0x23 off
  int filter_alarm = -1;  // reg 0x44 (group 13)
  int64_t used_power = -1;  // reg 0xE0 (group 14), raw
  int64_t used_time = -1;   // reg 0xE4 (group 14), raw
  int64_t filter_use = -1;  // reg 0xE6 (group 14), raw
  int filter_interval = -1;  // reg 0xE9 (group 14): index into FILTER_INTERVALS_H
  uint32_t last_rx_ms = 0;
};

inline State state;
inline uint8_t counter = 0;
inline std::vector<uint8_t> rx;

inline std::string hex(const std::vector<uint8_t> &v, size_t from, size_t to) {
  std::string s;
  char b[4];
  for (size_t i = from; i < to && i < v.size(); i++) {
    snprintf(b, sizeof(b), "%02X ", v[i]);
    s += b;
  }
  return s;
}

inline uint8_t xor_of(const std::vector<uint8_t> &f, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++)
    c ^= f[i];
  return c;
}

inline std::vector<uint8_t> build(uint8_t type_hi, uint8_t type_lo, const std::vector<uint8_t> &regs,
                                  uint8_t cnt) {
  std::vector<uint8_t> f = {0xD0, 0xC0, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, cnt,
                            0xFE, type_hi, type_lo, (uint8_t) regs.size()};
  f.insert(f.end(), regs.begin(), regs.end());
  f[3] = (uint8_t) (f.size() + 2 - 4);
  f.push_back(xor_of(f, f.size()));
  f.push_back(0xE0);
  return f;
}

inline void send(esphome::uart::UARTComponent *uart, uint8_t type_hi, uint8_t type_lo,
                 const std::vector<uint8_t> &regs) {
  auto f = build(type_hi, type_lo, regs, counter++);
  ESP_LOGD(TAG, "TX %02X%02X: %s", type_hi, type_lo, hex(f, 14, f.size() - 2).c_str());
  uart->write_array(f);
}

// Whether our commands beep (AC_FUN_BIP). The remote's own beeps are not affected.
inline bool beep = true;

// User command, as the original module sends it: the registers plus AC_FUN_BIP (0x74),
// 0x0F = beep, 0xF0 = silent (the value the module's enable message uses).
inline void command(esphome::uart::UARTComponent *uart, uint8_t group, std::vector<uint8_t> regs) {
  regs.insert(regs.end(), {0x74, 0x01, (uint8_t) (beep ? 0x0F : 0xF0)});
  send(uart, group, 0x04, regs);
}

inline void command(esphome::uart::UARTComponent *uart, uint8_t group, uint8_t reg, uint8_t value) {
  command(uart, group, std::vector<uint8_t>{reg, 0x01, value});
}

inline void handshake(esphome::uart::UARTComponent *uart) {
  ESP_LOGI(TAG, "Handshake");
  uart->write_array(std::vector<uint8_t>{0x00, 0xFC});
  send(uart, 0x12, 0x04, {0x01, 0x01, 0x0F, 0x74, 0x01, 0xF0});  // AC_FUN_ENABLE
}

// Tell the AC that "Wi-Fi" and "internet" are up, so its Wi-Fi indicator lights.
inline void notify_online(esphome::uart::UARTComponent *uart, bool online) {
  uint8_t v = online ? 0x0F : 0xF0;
  send(uart, 0x14, 0x04, {0x37, 0x01, v});
  send(uart, 0x14, 0x04, {0x38, 0x01, v});
}

// Ask for counters and versions (answered with 1403).
inline void request_counters(esphome::uart::UARTComponent *uart) {
  send(uart, 0x14, 0x02,
       {0x32, 0x00, 0xF6, 0x00, 0xF4, 0x00, 0xF3, 0x00, 0xF5, 0x00, 0x39, 0x00, 0xE0, 0x00, 0xE4, 0x00,
        0xE8, 0x00, 0xE9, 0x00, 0xE6, 0x00});
}

inline int64_t be(const std::vector<uint8_t> &f, size_t at, size_t len) {
  int64_t v = 0;
  for (size_t i = 0; i < len; i++)
    v = (v << 8) | f[at + i];
  return v;
}

inline void decode(const std::vector<uint8_t> &f) {
  uint8_t group = f[11];
  size_t end = 14 + f[13];
  for (size_t i = 14; i + 2 <= end;) {
    uint8_t id = f[i], len = f[i + 1];
    size_t at = i + 2;
    if (at + len > end)
      break;
    if (len > 0) {
      if (group == 0x12 && id == 0x02)
        state.power = f[at];
      else if (group == 0x12 && id == 0x43)
        state.opmode = f[at];
      else if (group == 0x12 && id == 0x44)
        state.comode = f[at];
      else if (group == 0x12 && id == 0x63)
        state.direction = f[at];
      else if (group == 0x12 && id == 0x73)
        state.sleep = f[at];
      else if (group == 0x13 && id == 0x32)
        state.autoclean = f[at];
      else if (group == 0x13 && id == 0x44)
        state.filter_alarm = f[at];
      else if (group == 0x14 && id == 0xE0)
        state.used_power = be(f, at, len);
      else if (group == 0x14 && id == 0xE4)
        state.used_time = be(f, at, len);
      else if (group == 0x14 && id == 0xE6)
        state.filter_use = be(f, at, len);
      else if (group == 0x14 && id == 0xE9)
        state.filter_interval = f[at];
    }
    i = at + len;
  }
}

inline void handle(esphome::uart::UARTComponent *uart, const std::vector<uint8_t> &f) {
  state.last_rx_ms = esphome::millis();
  ESP_LOGD(TAG, "RX %02X%02X: %s", f[11], f[12], hex(f, 14, f.size() - 2).c_str());
  decode(f);
  if ((f[12] & 0x01) == 0) {  // a report from the AC: ack it as type + 1
    std::vector<uint8_t> ack = f;
    ack[12] += 1;
    ack[ack.size() - 2] = xor_of(ack, ack.size() - 2);
    uart->write_array(ack);
  }
}

// Call often: reads whatever arrived and handles every complete frame.
inline void poll(esphome::uart::UARTComponent *uart) {
  uint8_t b;
  while (uart->available() && uart->read_byte(&b))
    rx.push_back(b);
  while (true) {
    size_t start = 0;
    while (start + 1 < rx.size() && !(rx[start] == 0xD0 && rx[start + 1] == 0xC0))
      start++;
    if (start > 0)
      rx.erase(rx.begin(), rx.begin() + start);  // drops 00 00 and line noise
    if (rx.size() < 14)
      return;
    size_t total = rx[3] + 4;
    if (total < 16 || total > 300) {
      rx.erase(rx.begin());
      continue;
    }
    if (rx.size() < total)
      return;
    std::vector<uint8_t> f(rx.begin(), rx.begin() + total);
    rx.erase(rx.begin(), rx.begin() + total);
    if (f[total - 1] != 0xE0 || f[total - 2] != xor_of(f, total - 2)) {
      ESP_LOGW(TAG, "Bad frame: %s", hex(f, 0, f.size()).c_str());
      continue;
    }
    handle(uart, f);
  }
}

// AC_FUN_COMODE (reg 0x44) values, mapped from the remote's buttons: none 0x12, quiet 0x52,
// fast 0x22, comfort 0x62, good'sleep 0x42, single user 0x32.
inline const uint8_t COMODE_GOOD_SLEEP = 0x42;
inline const uint8_t COMODE_NONE = 0x12;

// AC_FUN_DIRECTION (reg 0x63) names, from kumy's table.
struct Direction {
  const char *name;
  uint8_t value;
};
// Only fixed and up/down swing do anything on AR12HSFSAWKNZE (tested 2026-10-03): the horizontal blades
// are manual, and indirect (0x21), direct (0x31), long (0x81), off (0x12) had no effect.
inline const Direction DIRECTIONS[] = {
    {"Fixed", 0xC2},
    {"Swing up/down", 0x92},
};

// AC_ADD2_FILTERTIME (reg 0xE9) values, in hours (kumy: 00:0 01:180 02:300 03:500 04:700).
inline const int FILTER_INTERVALS_H[] = {0, 180, 300, 500, 700};
// The filter counter can't be reset from here: 1304 reg 0x44 = 00/01/0F/12/22/F0/FF and rewriting
// 0xE9 are acked but ignored (2026-10-04). Only the remote's filter reset zeroes 0xE6.
// Never write 1404 0xE8 (AC_ADD2_CLEAR_POWERTIME): per kumy it clears the energy counter.

inline int filter_interval_h() {
  return state.filter_interval >= 0 && state.filter_interval <= 4 ? FILTER_INTERVALS_H[state.filter_interval] : -1;
}

// samsung_ac Mode / FanMode enum order -> CN51 values.
inline const uint8_t OPMODES[] = {0xE2, 0x12, 0x22, 0x32, 0x42};  // auto, cool, dry, fan, heat
inline const uint8_t FANS[] = {0x00, 0x12, 0x14, 0x16, 0x18};     // auto, low, mid, high, turbo

// A climate request from samsung_ac, sent as one CN51 command (F1/F2 stays read-only).
inline void apply_request(esphome::uart::UARTComponent *uart, const esphome::samsung_ac::ProtocolRequest &r,
                          uint8_t good_sleep_half_hours) {
  using namespace esphome::samsung_ac;
  std::vector<uint8_t> regs;
  if (r.power.has_value() && !r.power.value()) {
    command(uart, 0x12, 0x02, 0xF0);  // off: nothing else matters
    return;
  }
  if (r.power.has_value() || r.mode.has_value())
    regs.insert(regs.end(), {0x02, 0x01, 0x0F});
  if (r.mode.has_value() && (int) r.mode.value() >= 0 && (int) r.mode.value() <= 4)
    regs.insert(regs.end(), {0x43, 0x01, OPMODES[(int) r.mode.value()]});
  if (r.target_temp.has_value())
    regs.insert(regs.end(), {0x5A, 0x01, (uint8_t) lroundf(r.target_temp.value())});
  if (r.fan_mode.has_value() && (int) r.fan_mode.value() >= 0 && (int) r.fan_mode.value() <= 4)
    regs.insert(regs.end(), {0x62, 0x01, FANS[(int) r.fan_mode.value()]});
  if (r.swing_mode.has_value())
    regs.insert(regs.end(), {0x63, 0x01, (uint8_t) (r.swing_mode.value() == SwingMode::Fix ? 0xC2 : 0x92)});
  if (r.alt_mode.has_value()) {
    // samsung_ac always registers its own "None" = 0 first, so choosing "None" sends 0 (the AC answers
    // FE); our "None" = 0x12 entry is only there to name the reported value.
    uint8_t comode = r.alt_mode.value() == 0 ? COMODE_NONE : r.alt_mode.value();
    regs.insert(regs.end(), {0x44, 0x01, comode});
    if (comode == COMODE_GOOD_SLEEP)
      regs.insert(regs.end(), {0x73, 0x01, good_sleep_half_hours});
  }
  if (!regs.empty())
    command(uart, 0x12, regs);
}

// Auto clean rules:
// 1. Turning on, or switching mode, into cool/dry turns auto clean on. Only on those transitions,
//    so a manual "off" afterwards sticks.
// 2. Every turn-on clears auto clean for the first 30 s: an off within 30 s (e.g. on/off to stop an
//    auto clean already running after a remote "off") then shuts down at once. After 30 s the
//    previous setting comes back, or rule 1 applies.
struct AutoClean {
  int last_power = -1;
  int last_opmode = -1;
  uint32_t window_end_ms = 0;  // 0 = no window
  bool restore = false;        // auto clean was on before the turn-on (kept across a quick on/off)
};
inline AutoClean autoclean;

inline bool cool_or_dry(int m) { return m == 0x12 || m == 0x22; }

inline void set_autoclean(esphome::uart::UARTComponent *uart, bool on) {
  ESP_LOGI(TAG, "Auto clean -> %s", on ? "on" : "off");
  command(uart, 0x13, 0x32, on ? 0x22 : 0x23);
}

// Call after poll(): reacts to power / mode changes reported on CN51.
inline void autoclean_tick(esphome::uart::UARTComponent *uart) {
  auto &a = autoclean;
  uint32_t now = esphome::millis();
  bool on = state.power == 0x0F;
  if (a.last_power == 0xF0 && on) {
    a.restore = a.restore || state.autoclean == 0x22;
    a.window_end_ms = now + 30000;
    if (a.window_end_ms == 0)
      a.window_end_ms = 1;
    if (a.restore)
      set_autoclean(uart, false);
  } else if (on && a.window_end_ms == 0 && a.last_opmode >= 0 && state.opmode != a.last_opmode &&
             cool_or_dry(state.opmode) && state.autoclean != 0x22) {
    set_autoclean(uart, true);
  }
  if (a.window_end_ms != 0) {
    if (!on) {
      a.window_end_ms = 0;  // off inside the window: auto clean stays off for this stop, restore kept
    } else if ((int32_t) (now - a.window_end_ms) >= 0) {
      a.window_end_ms = 0;
      if ((a.restore || cool_or_dry(state.opmode)) && state.autoclean != 0x22)
        set_autoclean(uart, true);
      a.restore = false;
    }
  }
  if (state.power >= 0)
    a.last_power = state.power;
  if (state.opmode >= 0)
    a.last_opmode = state.opmode;
}

}  // namespace ac_cn51
