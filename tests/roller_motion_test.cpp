#include "roller_motion.h"
#include <cassert>
#include <cstdio>
#include <string_view>

using roller::Dir;
using roller::Motion;

struct Bench {
  Motion motor;
  uint32_t now{0};
  float fabric{1};
  static constexpr float up_ms = 19000, down_ms = 24000;
  void step(unsigned ms = 10) {
    const auto d = motor.direction();
    if (d == Dir::up) fabric += motor.duty() * ms / up_ms;
    if (d == Dir::down) fabric -= motor.duty() * ms / down_ms;
    now += ms;
    motor.tick(now);
  }
  void jog_to(float edge) {
    const auto dir = edge > fabric ? Dir::up : Dir::down;
    for (int step_count = 0; step_count < 12000; ++step_count) {
      if (dir == Dir::up ? fabric >= edge : fabric <= edge) break;
      if (step_count % 15 == 0) motor.jog(dir, now);
      step();
    }
    assert(std::fabs(fabric - edge) < 0.002);
    motor.stop(now);
  }
  void finish_move() {
    for (int n = 0; n < 12100 && motor.active(); ++n) step();
    assert(!motor.active() && motor.duty() == 0);
  }
};

void calibration_and_guarded_travel() {
  Bench b;
  b.motor.go(0, b.now);
  assert(!b.motor.active()); // no automatic run on factory times
  b.motor.mark(Dir::up, b.now);
  b.motor.mark(Dir::down, b.now);
  assert(std::string_view(b.motor.status()) == "retry" && !b.motor.ready());
  b.motor.mark(Dir::up, b.now);
  b.jog_to(0.45f);
  b.jog_to(0.54f); // correction in the opposite direction during the first pass
  b.jog_to(0);
  b.motor.mark(Dir::down, b.now);
  assert(std::string_view(b.motor.status()) == "top" && !b.motor.ready());
  b.jog_to(1);
  b.motor.mark(Dir::up, b.now);
  assert(b.motor.ready());
  assert(std::fabs(b.motor.opening_seconds() - 19) < 0.07);
  assert(std::fabs(b.motor.closing_seconds() - 24) < 0.07);
  b.motor.go(0, b.now);
  b.finish_move();
  const float first_bottom = b.fabric;
  assert(first_bottom > 0.015f && first_bottom < 0.04f);
  for (int i = 0; i < 8; ++i) {
    b.motor.go(1, b.now);
    for (int n = 0; n < 100; ++n) b.step();
    b.motor.stop(b.now); // short upward trip and immediate reversal
    b.motor.go(0, b.now);
    b.finish_move();
    assert(b.fabric > 0.015f && std::fabs(b.fabric - first_bottom) < 0.003f);
    b.motor.go(0, b.now);
    b.motor.jog(Dir::down, b.now);
    assert(!b.motor.active()); // repeated down commands do not push further
  }

  // Interrupted travel preserves the learned profile but not a claimed position.
  b.motor.go(1, b.now);
  b.step(500);
  Motion restarted;
  restarted.restore(b.motor.snapshot(false));
  assert(!restarted.ready() && std::string_view(restarted.status()) == "reference");
  restarted.go(1, 100);
  assert(!restarted.active());
  restarted.mark(Dir::up, 100);
  assert(restarted.ready() && restarted.position() == 1);
  assert(std::fabs(restarted.closing_seconds() - b.motor.closing_seconds()) < 0.001);
  Motion idle_restart;
  idle_restart.restore(restarted.snapshot(true));
  assert(idle_restart.ready() && idle_restart.position() == 1);
}

void bounded_jog_and_interlock() {
  Motion m;
  // Check timing through millis() wrap as well as browser heartbeat loss.
  const uint32_t start = UINT32_MAX - 250;
  m.jog(Dir::down, start);
  m.tick(start + 149);
  assert(m.duty() == 0);
  m.tick(start + 160);
  assert(m.duty() > 0);
  m.tick(start + 500);
  assert(!m.active() && m.duty() == 0);
  m.jog(Dir::down, 1000);
  m.tick(1400);
  assert(m.duty() > 0);
  m.jog(Dir::up, 1410);
  assert(m.duty() == 0 && m.direction() == Dir::up);
  m.tick(1559);
  assert(m.duty() == 0);
  m.tick(1560);
  assert(m.duty() > 0);
  m.stop(1570);
  m.tick(2200);
  assert(!m.active() && m.duty() == 0);
}

int main() {
  calibration_and_guarded_travel();
  bounded_jog_and_interlock();
  std::puts("Passed: two-way learning with backtracking, guarded partial/repeated moves, reboot reference, jog timeout and reversal.");
}
