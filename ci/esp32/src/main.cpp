#include <Arduino.h>
#include <oa3/model/version.h>

void setup() {
    Serial.begin(115200);
    Serial.printf("oa3 %s, OpenADR %s\n", oa3::library_version(), oa3::spec_version());
}

void loop() {}
