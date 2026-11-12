#include <Arduino.h>
#include <oa3/model/byte_reader.h>
#include <oa3/model/intervals.h>
#include <oa3/model/json_arena.h>
#include <oa3/model/parse.h>
#include <oa3/model/time.h>
#include <oa3/model/version.h>

#include "events_fixture.h"

static oa3::StaticJsonArena<> arena;
static oa3::Event events[OA3_MAX_EVENTS];

void setup() {
    Serial.begin(115200);
    Serial.printf("oa3 %s, OpenADR %s\n", oa3::library_version(), oa3::spec_version());

    oa3::BufferReader in(kEventsJson, sizeof(kEventsJson) - 1);
    size_t count = 0;
    oa3::ParseResult r = oa3::parse_events(in, arena, events, OA3_MAX_EVENTS, &count);
    Serial.printf("parse_events: %s, %u events, arena high water %u of %u\n",
                  oa3::to_string(r), static_cast<unsigned>(count),
                  static_cast<unsigned>(arena.high_water()),
                  static_cast<unsigned>(arena.capacity()));

    oa3::DateTime now;
    oa3::PriceAt price;
    if (oa3::parse_datetime("2026-11-14T08:30:00Z", &now) &&
        oa3::price_at(events, count, now, "PRICE", &price)) {
        Serial.printf("price at 2026-11-14T08:30:00Z: %s (%.6f), interval %u\n",
                      price.value->text, price.value->number,
                      static_cast<unsigned>(price.interval_index));
    } else {
        Serial.println("no price at 2026-11-14T08:30:00Z");
    }
}

void loop() {}
