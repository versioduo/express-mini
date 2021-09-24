// © Kay Sievers <kay@vrfy.org>, 2020-2021
// SPDX-License-Identifier: Apache-2.0

#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Potentiometer.h>

V2DEVICE_METADATA("com.versioduo.express-mini", 18, "versioduo:samd:express-mini");

constexpr static uint8_t n_potis = 2;
static V2LED::WS2812 LED(n_potis, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2Link::Port Plug(&SerialPlug);
static V2Link::Port Socket(&SerialSocket);

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "express-mini";
    metadata.description = "2 Channel Analog Expression Controller";
    metadata.home        = "https://versioduo.com/#express-mini";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    configuration = {.magic{0x9dfe0000 | usb.pid}, .size{sizeof(config)}, .data{&config}};
  }

  // Config, written to EEPROM
  struct {
    uint8_t channel;
  } config{.channel{}};

  void play(int8_t note, int8_t velocity) {
    if (note < V2MIDI::C(3))
      return;

    note -= V2MIDI::C(3);
    if (note >= n_potis)
      return;

    float fraction = (float)velocity / 127;

    if (velocity > 0) {
      digitalWrite(PIN_LED_ONBOARD, HIGH);
      LED.setHSV(note, 120, 1, fraction);

    } else {
      digitalWrite(PIN_LED_ONBOARD, LOW);
      LED.setBrightness(note, 0);
    }
  }

private:
  const struct V2Potentiometer::Config _config { .n_steps{128}, .min{0.05}, .max{0.7}, .alpha{0.3}, .lag{0.007}, };

  V2Potentiometer _potis[n_potis]{V2Potentiometer(&_config), V2Potentiometer(&_config)};
  uint8_t _steps[n_potis]{};
  unsigned long _measure_usec{};
  unsigned long _events_usec{};
  V2MIDI::Packet _midi{};

  void handleReset() override {
    digitalWrite(PIN_LED_ONBOARD, LOW);
    LED.reset();

    for (uint8_t i = 0; i < n_potis; i++)
      _potis[i].reset();

    memset(_steps, 0, sizeof(_steps));
    _measure_usec = 0;
    _events_usec  = micros();
    _midi         = {};
  }

  void allNotesOff() {
    sendEvents(true);
  }

  void handleLoop() override {
    if ((unsigned long)(micros() - _measure_usec) > 10 * 1000) {
      for (uint8_t i = 0; i < n_potis; i++)
        _potis[i].measure(analogRead(PIN_CHANNEL_SENSE + i) / 1023.f);

      _measure_usec = micros();
    }

    if ((unsigned long)(micros() - _events_usec) > 50 * 1000) {
      sendEvents();
      _events_usec = micros();
    }
  }

  bool handleSend(V2MIDI::Packet *midi) override {
    usb.midi.send(midi);
    Plug.send(midi);
    return true;
  }

  void sendEvents(bool force = false) {
    for (uint8_t i = 0; i < n_potis; i++) {
      if (!force && _steps[i] == _potis[i].getStep())
        continue;

      LED.setBrightness(i, (float)_potis[i].getFraction());
      send(_midi.setControlChange(config.channel, V2MIDI::CC::GeneralPurpose1 + i, _potis[i].getStep()));
      _steps[i] = _potis[i].getStep();
    }
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    play(note, velocity);
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    play(note, 0);
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    switch (controller) {
      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        break;
    }
  }

  void handleSystemReset() override {
    reset();
  }

  void exportSettings(JsonArray json) override {
    JsonObject json_midi = json.createNestedObject();
    json_midi["type"]    = "midi";
    json_midi["channel"] = "midi.channel";

    // The object in the configuration record.
    JsonObject json_configuration = json_midi.createNestedObject("configuration");
    json_configuration["path"]    = "midi";
    json_configuration["field"]   = "channel";
  }

  void importConfiguration(JsonObject json) override {
    JsonObject json_midi = json["midi"];
    if (json_midi) {
      if (!json_midi["channel"].isNull()) {
        uint8_t channel = json_midi["channel"];

        if (channel < 1)
          config.channel = 0;
        else if (channel > 16)
          config.channel = 15;
        else
          config.channel = channel - 1;
      }
    }
  }

  void exportConfiguration(JsonObject json) override {
    json["#midi"]         = "The MIDI settings";
    JsonObject json_midi  = json.createNestedObject("midi");
    json_midi["#channel"] = "The channel to send notes and control values to";
    json_midi["channel"]  = config.channel + 1;

    // The object in the configuration record.
    JsonObject json_configuration = json_midi.createNestedObject("configuration");
    json_configuration["path"]    = "midi";
    json_configuration["field"]   = "channel";
  }

  void exportInput(JsonObject json) override {
    // The range of notes we receive to drive the LEDs.
    JsonObject json_chromatic = json.createNestedObject("chromatic");
    json_chromatic["start"]   = V2MIDI::C(3);
    json_chromatic["count"]   = n_potis;
  }

  void exportOutput(JsonObject json) override {
    json["channel"] = config.channel;

    // List of controllers we send out; generic CC values, one per channel.
    JsonArray json_controllers = json.createNestedArray("controllers");
    for (uint8_t i = 0; i < n_potis; i++) {
      char name[11];
      sprintf(name, "Channel %d", i + 1);

      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = name;
      json_controller["number"]  = V2MIDI::CC::GeneralPurpose1 + i;
    }
  }
} Device;

// Dispatch MIDI packets
static class MIDI {
public:
  void loop() {
    if (!Device.usb.midi.receive(&_midi))
      return;

    if (_midi.getPort() == 0) {
      Device.dispatch(&Device.usb.midi, &_midi);

    } else {
      _midi.setPort(_midi.getPort() - 1);
      Socket.send(&_midi);
    }
  }

private:
  V2MIDI::Packet _midi{};
} MIDI;

// Dispatch Link packets
static class Link : public V2Link {
public:
  Link() : V2Link(&Plug, &Socket) {}

private:
  V2MIDI::Packet _midi{};

  // Receive a host event from our parent device
  void receivePlug(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      packet->receive(&_midi);
      Device.dispatch(&Plug, &_midi);
    }
  }

  // Forward children device events to the host
  void receiveSocket(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      uint8_t address = packet->getAddress();
      if (address == 0x0f)
        return;

      if (Device.usb.midi.connected()) {
        packet->receive(&_midi);
        _midi.setPort(address + 1);
        Device.usb.midi.send(&_midi);
      }
    }
  }
} Link;

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.5);
  Plug.begin();
  Socket.begin();

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialPlug, 2);
  setSerialPriority(&SerialSocket, 2);

  Device.begin();
  Device.reset();
}

void loop() {
  LED.loop();
  MIDI.loop();
  Link.loop();
  Device.loop();

  if (Link.idle() && Device.idle())
    Device.sleep();
}
