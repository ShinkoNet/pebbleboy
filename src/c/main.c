#include <pebble.h>

#include "gb_app.h"

int main(void) {
  gb_app_push();
  app_event_loop();
  return 0;
}

