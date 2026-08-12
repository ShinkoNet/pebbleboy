#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gb_input.h"

static void expect(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static void expect_pressed(uint8_t mask, const char *message) {
  expect((pb_input_joypad() & mask) == 0, message);
}

static void expect_released(uint8_t mask, const char *message) {
  expect((pb_input_joypad() & mask) == mask, message);
}

static void test_touch_quadrants(void) {
  pb_input_init();
  pb_input_touch_at(20, 114, 200, 228);
  expect_pressed(JOYPAD_LEFT, "left touch did not press left");
  expect_released((uint8_t)(JOYPAD_RIGHT | JOYPAD_UP | JOYPAD_DOWN),
                  "left touch pressed another direction");

  pb_input_touch_at(180, 114, 200, 228);
  expect_pressed(JOYPAD_RIGHT, "right touch did not press right");
  expect_released((uint8_t)(JOYPAD_LEFT | JOYPAD_UP | JOYPAD_DOWN),
                  "right touch did not release previous direction");

  pb_input_touch_at(100, 20, 200, 228);
  expect_pressed(JOYPAD_UP, "up touch did not press up");
  expect_released((uint8_t)(JOYPAD_LEFT | JOYPAD_RIGHT | JOYPAD_DOWN),
                  "up touch did not release previous direction");

  pb_input_touch_at(100, 200, 200, 228);
  expect_pressed(JOYPAD_DOWN, "down touch did not press down");
  expect_released((uint8_t)(JOYPAD_LEFT | JOYPAD_RIGHT | JOYPAD_UP),
                  "down touch did not release previous direction");

  pb_input_release_touch();
  expect(pb_input_joypad() == 0xFF, "touch release did not clear all touch input");
}

static void test_touch_tie_prefers_vertical(void) {
  pb_input_init();
  pb_input_touch_at(60, 74, 200, 228);
  expect_pressed(JOYPAD_UP, "diagonal up-left tie did not prefer up");
  expect_released(JOYPAD_LEFT, "diagonal up-left tie incorrectly pressed left");

  pb_input_touch_at(140, 154, 200, 228);
  expect_pressed(JOYPAD_DOWN, "diagonal down-right tie did not prefer down");
  expect_released(JOYPAD_RIGHT, "diagonal down-right tie incorrectly pressed right");
}

static void test_physical_button_sequence_clears_touch(void) {
  pb_input_init();
  pb_input_touch_at(20, 114, 200, 228);
  expect_pressed(JOYPAD_LEFT, "touch setup did not press left");

  pb_input_release_touch();
  pb_input_press(JOYPAD_A);
  expect_pressed(JOYPAD_A, "physical A press did not register");
  expect_released(JOYPAD_LEFT, "physical button sequence left touch direction stuck");

  pb_input_release(JOYPAD_A);
  expect(pb_input_joypad() == 0xFF, "physical A release did not restore neutral input");
}

static void test_start_select_double_chord(void) {
  pb_input_init();
  expect(!pb_input_start_select_toggle(100),
         "Select without Start armed the speed chord");

  pb_input_press(JOYPAD_START);
  expect(!pb_input_start_select_toggle(200),
         "first Start+Select chord toggled immediately");
  pb_input_release(JOYPAD_START);
  pb_input_press(JOYPAD_START);
  expect(pb_input_start_select_toggle(1200),
         "second Start+Select chord at one second did not toggle");

  expect(!pb_input_start_select_toggle(1300),
         "toggle did not disarm the chord sequence");
  expect(!pb_input_start_select_toggle(2301),
         "expired chord incorrectly toggled");
  expect(pb_input_start_select_toggle(3000),
         "fresh double chord did not toggle");
  pb_input_release(JOYPAD_START);
}

int main(void) {
  test_touch_quadrants();
  test_touch_tie_prefers_vertical();
  test_physical_button_sequence_clears_touch();
  test_start_select_double_chord();
  printf("input test passed\n");
  return 0;
}
