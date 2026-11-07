#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace roller {
enum class Dir : int { down = -1, off = 0, up = 1 };

struct Snapshot {
  uint32_t version{};
  float opening{}, closing{}, position{};
  uint32_t known{};
};

// Travel units are milliseconds weighted by the PWM actually applied. That is
// an estimate of travel, not a position measurement. Jog and normal motion
// use the same ramp so their estimates can be combined.
class Motion {
 public:
  static constexpr uint32_t kJogMs = 500;
  static constexpr uint32_t kRunMaxMs = 120000;

  bool ready() const { return profile() && known_; }
  bool profile() const { return opening_ > 0 && closing_ > 0; }
  bool active() const { return direction_ != Dir::off; }
  Dir direction() const { return direction_; }
  float duty() const { return duty_; }
  float position() const {
    return ready() ? std::clamp((physical_ - low()) / (high() - low()), 0.0f, 1.0f)
                   : physical_;
  }
  float opening_seconds() const { return opening_ / 1000.0f; }
  float closing_seconds() const { return closing_ / 1000.0f; }
  const char *status() const {
    if (failed_) return "storage_error";
    if (ready()) return "ready";
    if (profile()) return "reference";
    if (invalid_) return "retry";
    if (anchor_ == Dir::off) return "first";
    return anchor_ == Dir::up ? "bottom" : "top";
  }

  Snapshot snapshot(bool stopped) const {
    return {1, opening_, closing_, physical_, static_cast<uint32_t>(stopped && ready())};
  }
  void restore(const Snapshot &s) {
    if (s.version != 1 || !valid_span(s.opening) || !valid_span(s.closing) ||
        !std::isfinite(s.position) || s.position < 0 || s.position > 1) return;
    opening_ = s.opening;
    closing_ = s.closing;
    physical_ = s.position;
    known_ = s.known == 1;
  }

  bool take_save() {
    const bool save = save_;
    save_ = false;
    return save;
  }

  void tick(uint32_t now) {
    if (!active()) return;
    const auto elapsed = now - last_;
    last_ = now;
    if (duty_ > 0) {
      const float travel = elapsed * duty_;
      if (ready()) {
        physical_ = std::clamp(physical_ + (direction_ == Dir::up ? travel / opening_
                                                                : -travel / closing_),
                               0.0f, 1.0f);
      } else if (!ready()) {
        if (anchor_ != Dir::off && !profile()) (direction_ == Dir::up ? leg_.up : leg_.down) += travel;
        leg_motor_ms_ += elapsed;
      }
    }
    if ((!target_mode_ && now - last_jog_ >= kJogMs) ||
        now - started_ >= kRunMaxMs ||
        (ready() && reached()) || (leg_motor_ms_ >= kRunMaxMs && !ready())) {
      end();
      return;
    }
    const auto run = now - started_;
    duty_ = run < 150 ? 0.0f : run < 350 ? 0.3f : run < 550 ? 0.7f : 1.0f;
  }

  void go(float fraction, uint32_t now) {
    tick(now);
    if (!ready() || failed_ || !std::isfinite(fraction)) return;
    const float target = low() + std::clamp(fraction, 0.0f, 1.0f) * (high() - low());
    // An endpoint request cannot drive the blind back inward if it is
    // already on or outside that endpoint's guard.
    if (std::fabs(target - physical_) < 0.0005f ||
        (fraction <= 0 && physical_ <= low()) || (fraction >= 1 && physical_ >= high())) {
      end();
      return;
    }
    const Dir dir = target > physical_ ? Dir::up : Dir::down;
    begin(dir, now);
    target_mode_ = true;
    target_ = target;
  }

  void jog(Dir dir, uint32_t now) {
    tick(now);
    if (failed_ || dir == Dir::off || (!ready() && leg_motor_ms_ >= kRunMaxMs)) return;
    if (ready() && (dir == Dir::up ? physical_ >= high() : physical_ <= low())) {
      end();
      return;
    }
    begin(dir, now);
    target_mode_ = false;
    last_jog_ = now;
    target_ = dir == Dir::up ? high() : low();
  }

  void stop(uint32_t now) { tick(now); end(); }

  void reset(uint32_t now) {
    stop(now);
    known_ = false;
    opening_ = closing_ = 0;
    anchor_ = Dir::off;
    leg_ = open_leg_ = close_leg_ = {};
    leg_motor_ms_ = 0;
    invalid_ = false;
    save_ = true;
  }

  void fail(uint32_t now) {
    stop(now);
    known_ = false;
    failed_ = true;
  }

  void mark(Dir edge, uint32_t now) {
    stop(now);
    if (failed_ || edge == Dir::off) return;
    const float position = edge == Dir::up ? 1.0f : 0.0f;
    if (profile()) {
      physical_ = position;
      known_ = true;
      save_ = true;
      return;
    }
    invalid_ = false;
    if (anchor_ != Dir::off && edge != anchor_) {
      if (leg_motor_ms_ >= kRunMaxMs || (edge == Dir::up ? leg_.up : leg_.down) < 1000) {
        invalid_ = true;
        return;
      }
      (edge == Dir::up ? open_leg_ : close_leg_) = leg_;
      if (open_leg_.up > 0 && close_leg_.down > 0) {
        // Each full traversal is one unit of signed travel. Solve the
        // two equations together to allow small jogs back in either leg.
        const float determinant = open_leg_.up * close_leg_.down - close_leg_.up * open_leg_.down;
        const float opening = determinant / (open_leg_.down + close_leg_.down);
        const float closing = determinant / (open_leg_.up + close_leg_.up);
        if (!valid_span(opening) || !valid_span(closing)) {
          open_leg_ = close_leg_ = {};
          invalid_ = true;
        } else {
          opening_ = opening;
          closing_ = closing;
          known_ = true;
          save_ = true;
        }
      }
    }
    physical_ = position;
    anchor_ = edge;
    leg_ = {};
    leg_motor_ms_ = 0;
  }

 private:
  struct Leg { float up{}, down{}; };
  static bool valid_span(float n) { return std::isfinite(n) && n >= 1000 && n <= kRunMaxMs; }
  static float margin(float duration) { return std::min(0.24f, std::max(0.02f, 250.0f / duration)); }
  float low() const { return profile() ? margin(closing_) : 0; }
  float high() const { return profile() ? 1 - margin(opening_) : 1; }
  bool reached() const { return direction_ == Dir::up ? physical_ >= target_ : physical_ <= target_; }
  void end() { direction_ = Dir::off; duty_ = 0; }
  void begin(Dir dir, uint32_t now) {
    if (direction_ == dir) return;
    end();  // also supplies a 150 ms coast interval before reversing
    direction_ = dir;
    started_ = last_ = now;
  }

  Dir direction_{Dir::off}, anchor_{Dir::off};
  bool known_{}, target_mode_{}, invalid_{}, save_{}, failed_{};
  uint32_t started_{}, last_{}, last_jog_{}, leg_motor_ms_{};
  float opening_{}, closing_{}, physical_{0.5f}, target_{}, duty_{};
  Leg leg_, open_leg_, close_leg_;
};
inline Motion &controller() {
  static Motion motion;
  return motion;
}
}  // namespace roller
