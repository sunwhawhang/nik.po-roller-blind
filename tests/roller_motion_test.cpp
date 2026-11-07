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
  float up_ms = 22000, down_ms = 22000;
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

void two_marks_and_endpoint_trim() {
  Bench b;
  b.motor.go(0, b.now);
  assert(!b.motor.active());
  b.motor.mark(Dir::up, b.now);
  b.motor.mark(Dir::down, b.now);
  assert(std::string_view(b.motor.status()) == "retry" && !b.motor.ready());
  b.motor.mark(Dir::up, b.now);
  b.jog_to(0.45f);
  b.jog_to(0.54f); // backtracking still measures net signed travel
  b.jog_to(0);
  b.motor.mark(Dir::down, b.now);
  assert(b.motor.ready()); // two marks, no return trip
  assert(std::fabs(b.motor.closing_seconds() - 22) < 0.07);
  b.motor.go(0, b.now);
  assert(!b.motor.active()); // normal Down respects the estimated bottom
  const float old_span = b.motor.closing_seconds();
  const float old_bottom = b.fabric;
  b.motor.jog(Dir::down, b.now);
  b.finish_move();
  assert(b.fabric < old_bottom - 0.002f);
  b.motor.mark(Dir::down, b.now);
  assert(b.motor.ready() && b.motor.position() == 0);
  assert(b.motor.closing_seconds() > old_span + 0.1f); // re-mark changed the span
  b.motor.go(1, b.now);
  b.finish_move();
  assert(b.fabric > 0.93f && b.fabric < 1.0f); // opposite reference retained

  // Adjust the top inward without restarting calibration.
  const float top_before = b.fabric;
  b.motor.jog(Dir::down, b.now);
  b.finish_move();
  assert(b.fabric < top_before);
  const float expanded_span = b.motor.opening_seconds();
  b.motor.mark(Dir::up, b.now);
  assert(b.motor.ready() && b.motor.position() == 1);
  assert(b.motor.opening_seconds() < expanded_span);
  b.motor.go(0, b.now);
  b.finish_move();
  const float first_bottom = b.fabric;
  for (int i = 0; i < 8; ++i) {
    b.motor.go(1, b.now);
    for (int n = 0; n < 100; ++n) b.step();
    b.motor.stop(b.now);
    b.motor.go(0, b.now);
    b.finish_move();
    assert(std::fabs(b.fabric - first_bottom) < 0.003f);
    b.motor.go(0, b.now);
    assert(!b.motor.active());
  }
}

void retains_existing_calibration() {
  Motion existing;
  existing.restore({1,19000,24000,0.5f,1}); // already deployed snapshot format
  assert(existing.ready());
  assert(existing.opening_seconds() == 19 && existing.closing_seconds() == 24);
  Motion interrupted;
  interrupted.restore(existing.snapshot(false));
  assert(!interrupted.ready() && std::string_view(interrupted.status()) == "reference");
  interrupted.go(1, 100);
  assert(!interrupted.active());
  interrupted.mark(Dir::up, 100);
  assert(interrupted.ready() && interrupted.position() == 1);
  assert(interrupted.opening_seconds() == 19 && interrupted.closing_seconds() == 24);
  Motion restarted;
  restarted.restore(interrupted.snapshot(true));
  assert(restarted.ready() && restarted.position() == 1);
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
  two_marks_and_endpoint_trim();
  retains_existing_calibration();
  bounded_jog_and_interlock();
  std::puts("Passed: two marks, endpoint trimming, guarded partial moves, saved calibration, jog timeout and reversal.");
}
