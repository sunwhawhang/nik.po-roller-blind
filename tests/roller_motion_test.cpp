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
  bool reversed_wiring{};
  void step(unsigned ms = 10) {
    const auto d = Motion::reverse_if(motor.direction(), reversed_wiring);
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

  // Complete the optional return at the intended top, then test a local trim.
  b.jog_to(1);
  b.motor.mark(Dir::up, b.now);
  assert(std::string_view(b.motor.status()) == "ready");
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

void physical_buttons_before_calibration() {
  Bench b;
  b.motor.press_physical(Dir::up, false, false, b.now);
  assert(b.motor.active() && b.motor.direction() == Dir::up);
  b.step(70);
  b.motor.poll_physical(false, false, b.now); // tap released before motor start
  for (int n = 0; n < 44; ++n) b.step();
  assert(!b.motor.active() && b.fabric > 1.002f);

  b.motor.press_physical(Dir::down, false, false, b.now);
  for (int n = 0; n < 100; ++n) {
    b.step();
    b.motor.poll_physical(false, true, b.now);
  }
  assert(b.motor.active() && b.motor.duty() == 1.0f);
  b.motor.poll_physical(false, false, b.now);
  assert(!b.motor.active() && b.motor.duty() == 0);

  b.motor.press_physical(Dir::up, true, false, b.now);
  assert(b.motor.direction() == Dir::down);
  b.motor.poll_physical(true, true, b.now);
  assert(!b.motor.active());
  b.motor.poll_physical(true, false, b.now);
  assert(!b.motor.active()); // releasing only one of two buttons cannot restart

  b.motor.press_physical(Dir::up, false, false, b.now);
  b.step(400);
  b.motor.mark(Dir::up, b.now);
  b.motor.poll_physical(true, false, b.now);
  assert(!b.motor.active()); // marking an end cancels the physical hold
  b.motor.press_physical(Dir::down, false, false, b.now);
  b.motor.stop(b.now);
  b.motor.poll_physical(false, true, b.now);
  assert(!b.motor.active());
}

void calibration_orders_survive_idle_restart() {
  for (Dir first : {Dir::up, Dir::down}) {
    Bench b;
    const Dir second = first == Dir::up ? Dir::down : Dir::up;
    b.fabric = first == Dir::up ? 1.0f : 0.0f;
    b.motor.mark(first, b.now);
    assert(b.motor.has_reference() && b.motor.take_save());
    const auto first_mark = b.motor.snapshot(true);
    Motion restarted;
    restarted.restore(first_mark);
    assert(std::string_view(restarted.status()) == (second == Dir::up ? "top" : "bottom"));
    b.motor = restarted;
    b.motor.press_physical(second, false, false, b.now);
    for (int i = 0; i < 5000; ++i) {
      b.step();
      b.motor.poll_physical(second == Dir::up, second == Dir::down, b.now);
      if (second == Dir::up ? b.fabric >= 1.0f : b.fabric <= 0.0f) break;
    }
    b.motor.poll_physical(false, false, b.now);
    assert(!b.motor.active() && std::fabs(b.fabric - (second == Dir::up ? 1 : 0)) < 0.002);
    const auto arrived = b.motor.snapshot(true);
    Motion after_trip_restart;
    after_trip_restart.restore(arrived);
    after_trip_restart.mark(second, b.now);
    assert(after_trip_restart.ready()); // both directions finish on mark number two
    assert(std::fabs(after_trip_restart.opening_seconds() - 22.0f) < 0.07);

    Motion interrupted;
    interrupted.restore(first_mark);
    interrupted.press_physical(second, false, false, b.now);
    Motion powered_back_on;
    powered_back_on.restore(interrupted.snapshot(false));
    assert(std::string_view(powered_back_on.status()) == "interrupted");
    assert(!powered_back_on.ready() && !powered_back_on.active());
    powered_back_on.mark(second, b.now);
    assert(std::string_view(powered_back_on.status()) == (first == Dir::up ? "top" : "bottom"));
  }
}

void reversed_buttons_and_nudges_calibrate_either_order() {
  for (Dir first : {Dir::up, Dir::down}) {
    const Dir second = first == Dir::up ? Dir::down : Dir::up;
    Bench b;
    b.reversed_wiring = true;
    b.fabric = first == Dir::up ? 1.0f : 0.0f;
    b.motor.mark(first,b.now);
    const Dir same_as_button = Motion::reverse_if(second,true);
    b.motor.press_physical(second,true,false,b.now);
    assert(b.motor.direction() == same_as_button);
    b.motor.stop(b.now);
    for (int n=0; n<5000; ++n) {
      if (second == Dir::up ? b.fabric >= 1 : b.fabric <= 0) break;
      if (n % 15 == 0) b.motor.jog(same_as_button,b.now);
      b.step();
    }
    b.motor.stop(b.now);
    assert(std::fabs(b.fabric-(second == Dir::up ? 1 : 0)) < 0.002f);
    b.motor.mark(second,b.now);
    assert(b.motor.ready());
    assert(b.motor.opens(Dir::down) && !b.motor.opens(Dir::up));
    Motion restored;
    restored.restore(b.motor.snapshot(true));
    assert(restored.ready() && restored.opens(Dir::down));
    b.motor = restored;
    b.motor.go(first == Dir::up ? 1 : 0,b.now);
    assert(b.motor.direction() == Motion::reverse_if(first,true));
    b.finish_move();
    assert(first == Dir::up ? b.fabric > 0.93f && b.fabric < 1.0f :
                              b.fabric < 0.07f && b.fabric > 0.0f);
  }
  Motion legacy;
  legacy.restore({2,19000,24000,0.5f,7});
  assert(legacy.ready() && legacy.opens(Dir::up));
  legacy.go(1,100);
  assert(legacy.direction()==Dir::up);
  Motion one_direction;
  one_direction.restore({4,22000,22000,0.4f,5});
  assert(one_direction.ready() && std::string_view(one_direction.status()) == "ready_find_bottom");
  one_direction.mark(Dir::down,100);
  assert(std::string_view(one_direction.status()) == "ready_up");
}

void optional_return_measures_both_directions() {
  for (Dir first : {Dir::up, Dir::down}) {
    Bench b;
    const Dir second = Motion::reverse_if(first, true);
    b.fabric = first == Dir::up ? 1.0f : 0.0f;
    // The return is much slower and would hit the old manual trim limit.
    b.up_ms = first == Dir::up ? 42000 : 22000;
    b.down_ms = first == Dir::up ? 22000 : 42000;
    b.motor.mark(first, b.now);
    b.jog_to(1 - b.fabric);
    b.motor.mark(second, b.now);
    assert(b.motor.ready());
    assert(std::string_view(b.motor.status()) == (first == Dir::up ? "ready_up" : "ready_down"));
    assert(std::fabs(b.motor.opening_seconds() - 22.0f) < 0.1f);
    assert(std::fabs(b.motor.closing_seconds() - 22.0f) < 0.1f);

    // Both marks and the current marked end survive an idle reboot.
    Motion restored;
    restored.restore(b.motor.snapshot(true));
    b.motor = restored;
    b.motor.go(first == Dir::up ? 1 : 0, b.now);
    assert(b.motor.active()); // a return mark is optional
    b.finish_move();
    assert(std::fabs(b.fabric - (first == Dir::up ? 1 : 0)) > 0.4f);
    // Backtrack a little before reaching the physical endpoint.
    b.jog_to(b.fabric + (first == Dir::up ? -0.06f : 0.06f));
    b.jog_to(first == Dir::up ? 1 : 0);
    b.motor.mark(first, b.now);
    assert(std::string_view(b.motor.status()) == "ready");
    const float measured_return = first == Dir::up ? b.motor.opening_seconds() : b.motor.closing_seconds();
    const float original_direction = first == Dir::up ? b.motor.closing_seconds() : b.motor.opening_seconds();
    assert(std::fabs(measured_return - 42.0f) < 0.2f);
    assert(std::fabs(original_direction - 22.0f) < 0.1f);
    b.motor.go(second == Dir::up ? 1 : 0, b.now);
    b.finish_move();
    assert(std::fabs(b.fabric - (second == Dir::up ? 1 : 0)) < 0.02f);
    b.motor.go(first == Dir::up ? 1 : 0, b.now);
    b.finish_move();
    assert(std::fabs(b.fabric - (first == Dir::up ? 1 : 0)) < 0.02f);
  }
}

int main() {
  two_marks_and_endpoint_trim();
  retains_existing_calibration();
  bounded_jog_and_interlock();
  physical_buttons_before_calibration();
  calibration_orders_survive_idle_restart();
  reversed_buttons_and_nudges_calibrate_either_order();
  optional_return_measures_both_directions();
  std::puts("Passed: two marks, endpoint trimming, guarded partial moves, saved calibration, jog timeout, reversal physical controls, both mark orders interrupted calibration, reversed jog calibration and legacy direction preservation.");
}
