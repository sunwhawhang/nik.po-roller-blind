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
  static constexpr uint32_t kPhysicalHoldMs = 300;
  static constexpr uint32_t kRunMaxMs = 120000;
  static constexpr float kTrim = 0.20f;  // bounded manual adjustment beyond an old mark

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
    const uint32_t flags = (learned_ << 1) | static_cast<uint32_t>(stopped && ready());
    return {2, opening_, closing_, physical_, flags};
  }
  void restore(const Snapshot &s) {
    if ((s.version != 1 && s.version != 2) || !valid_span(s.opening) || !valid_span(s.closing) ||
        !std::isfinite(s.position) || s.position < -kTrim || s.position > 1 + kTrim) return;
    const uint32_t learned = s.version == 1 ? 3 : s.known >> 1;
    if (learned == 0 || learned > 3) return;
    opening_ = s.opening;
    closing_ = s.closing;
    physical_ = s.position;
    learned_ = learned;
    known_ = (s.known & 1) != 0;
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
                               -kTrim, 1.0f + kTrim);
      } else if (!ready()) {
        if (anchor_ != Dir::off && !profile()) (direction_ == Dir::up ? leg_.up : leg_.down) += travel;
        leg_motor_ms_ += elapsed;
      }
    }
    if ((!target_mode_ && now - last_jog_ >= kJogMs) ||
        now - started_ >= kRunMaxMs ||
        (ready() && (target_mode_ ? reached() : trim_exhausted(direction_))) ||
        (leg_motor_ms_ >= kRunMaxMs && !ready())) {
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
    physical_button_ = Dir::off;
    jog_pulse(dir, now);
  }

  // A short physical press gets the same bounded pulse as a web nudge.
  // A hold keeps the pulse alive only while the debounced input stays down.
  void press_physical(Dir button, bool swapped, bool both, uint32_t now) {
    if (ready() || button == Dir::off) return;
    if (both) {
      stop(now);
      return;
    }
    physical_button_ = button;
    physical_direction_ = swapped ? (button == Dir::up ? Dir::down : Dir::up) : button;
    physical_pressed_at_ = now;
    jog_pulse(physical_direction_, now);
  }

  void poll_physical(bool up, bool down, uint32_t now) {
    if (physical_button_ == Dir::off) return;
    const bool held = physical_button_ == Dir::up ? up : down;
    const bool other = physical_button_ == Dir::up ? down : up;
    const bool long_press = now - physical_pressed_at_ >= kPhysicalHoldMs;
    if (!held || other || ready()) {
      physical_button_ = Dir::off;
      if (other || ready() || long_press) stop(now);
      return;
    }
    // Stop and calibration commands clear physical_button_. A timeout must
    // not be able to restart a move while a physical button is still held.
    if (long_press && active()) jog_pulse(physical_direction_, now);
  }

  void stop(uint32_t now) {
    physical_button_ = Dir::off;
    tick(now);
    end();
  }

 private:
  void jog_pulse(Dir dir, uint32_t now) {
    tick(now);
    if (failed_ || dir == Dir::off || (!ready() && leg_motor_ms_ >= kRunMaxMs)) return;
    if (ready() && trim_exhausted(dir)) {
      end();
      return;
    }
    begin(dir, now);
    target_mode_ = false;
    last_jog_ = now;
  }

 public:
  void reset(uint32_t now) {
    stop(now);
    known_ = false;
    opening_ = closing_ = 0;
    anchor_ = Dir::off;
    learned_ = 0;
    leg_ = {};
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
      if (known_) {
        // The new mark moves this end of the span; preserve the opposite mark.
        // Use the raw estimate, including any manual jog beyond the old mark.
        const float scale = edge == Dir::up ? physical_ : 1.0f - physical_;
        if (!valid_span(opening_ * scale) || !valid_span(closing_ * scale)) return;
        opening_ *= scale;
        closing_ *= scale;
      }
      physical_ = position;
      known_ = true;
      save_ = true;
      return;
    }
    invalid_ = false;
    if (anchor_ != Dir::off && edge != anchor_) {
      const float span = edge == Dir::up ? leg_.up - leg_.down : leg_.down - leg_.up;
      if (leg_motor_ms_ >= kRunMaxMs || !valid_span(span)) {
        invalid_ = true;
        return;
      }
      // One trip provides a span and measures one direction. Use the same
      // estimated span in the other direction, with a larger stopping margin.
      opening_ = closing_ = span;
      learned_ = edge == Dir::up ? 1 : 2;
      known_ = true;
      save_ = true;
    }
    physical_ = position;
    anchor_ = edge;
    leg_ = {};
    leg_motor_ms_ = 0;
  }

 private:
  struct Leg { float up{}, down{}; };
  static bool valid_span(float n) { return std::isfinite(n) && n >= 1000 && n <= kRunMaxMs; }
  static float margin(float duration, bool measured) {
    return std::min(0.24f, std::max(measured ? 0.005f : 0.05f,
                                   (measured ? 250.0f : 500.0f) / duration));
  }
  float low() const { return profile() ? margin(closing_, learned_ & 2) : 0; }
  float high() const { return profile() ? 1 - margin(opening_, learned_ & 1) : 1; }
  bool trim_exhausted(Dir dir) const { return dir == Dir::up ? physical_ >= 1 + kTrim : physical_ <= -kTrim; }
  bool reached() const { return direction_ == Dir::up ? physical_ >= target_ : physical_ <= target_; }
  void end() { direction_ = Dir::off; duty_ = 0; }
  void begin(Dir dir, uint32_t now) {
    if (direction_ == dir) return;
    end();  // also supplies a 150 ms coast interval before reversing
    direction_ = dir;
    started_ = last_ = now;
  }

  Dir direction_{Dir::off}, anchor_{Dir::off};
  Dir physical_button_{Dir::off}, physical_direction_{Dir::off};
  uint32_t physical_pressed_at_{};
  bool known_{}, target_mode_{}, invalid_{}, save_{}, failed_{};
  uint32_t started_{}, last_{}, last_jog_{}, leg_motor_ms_{};
  float opening_{}, closing_{}, physical_{0.5f}, target_{}, duty_{};
  uint32_t learned_{};
  Leg leg_;
};
inline Motion &controller() {
  static Motion motion;
  return motion;
}
}  // namespace roller
