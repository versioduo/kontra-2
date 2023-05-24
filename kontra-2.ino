// © Kay Sievers <kay@versioduo.com>, 2020-2022
// SPDX-License-Identifier: Apache-2.0

#include "MIDISong.h"
#include <V2Buttons.h>
#include <V2Color.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Music.h>

V2DEVICE_METADATA("com.versioduo.kontra-2", 31, "versioduo:samd:control");

static constexpr uint8_t notesMax = 30;
static V2LED::WS2812 LED(2, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2LED::WS2812 LEDExt(41, PIN_LED_WS2812_EXT, &sercom1, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2Link::Port Socket(&SerialSocket);

// The button switches the state with a multi-click long-press.
static class Manual {
public:
  enum class Mode { Notes, Song, Test, Tune, Turn } mode{};
  Mode getMode() const {
    return _mode;
  }

  void setMode(Mode mode = Mode::Notes) {
    _mode = mode;

    switch (_mode) {
      case Mode::Notes:
        LED.reset();
        LED.setHSV(V2Color::Orange, 1, 0.25);
        break;

      case Mode::Song:
        LED.reset();
        LED.setBrightness(0.25);
        break;

      case Mode::Test:
        LED.reset();
        LED.rainbow(1, 3, 0.4);
        break;

      case Mode::Tune:
        LED.reset();
        LED.setHSV(V2Color::Magenta, 1, 0.25);
        break;

      case Mode::Turn:
        LED.reset();
        LED.setHSV(V2Color::Cyan, 1, 0.25);
        break;
    }
  }

  void setColor(V2Color::Hue color) {
    LED.reset();
    LED.setHSV(color, 1, 0.25);
  }

  void splashColor(V2Color::Hue color) {
    LED.splashHSV(0.5, color, 1, 0.25);
  }

private:
  Mode _mode{};
} Manual;

static constexpr struct Configuration {
  struct {
    // The middle C, MIDI note 60, in this mapping is C3.
    uint8_t start{V2MIDI::E(0)};
    uint8_t count{29};
  } notes;
} ConfigurationDefault;

static struct Configuration Config {
  ConfigurationDefault
};

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "V2 kontra-2";
    metadata.description = "2 String Double Bass";
    metadata.home        = "https://versioduo.com/#kontra-2";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
    usb.pid            = 0xe950;
    usb.ports.standard = 3;

    configuration = {.size{sizeof(Config)}, .data{&Config}};
  }

  enum class Program { Bow, Pluck, _count };

  enum class CC {
    Volume       = V2MIDI::CC::ChannelVolume,
    FingerSpeed  = V2MIDI::CC::Controller3,
    VibratoRate  = V2MIDI::CC::SoundController7,
    VibratoDepth = V2MIDI::CC::SoundController8,
    BowPressure  = V2MIDI::CC::SoundController5,
    BowSpeed     = V2MIDI::CC::ModulationWheel,
    BowReverse   = V2MIDI::CC::Controller14,
    Light        = V2MIDI::CC::Controller89,
    Rainbow      = V2MIDI::CC::Controller90,
  };

  void playLight(uint8_t note, uint8_t velocity) {
    uint8_t n = (Config.notes.count - 1) - (note - Config.notes.start);

    // There are two LEDs per note in the lower pitch range.
    if (n > 17)
      n = 17 + ((n - 18) * 2) + 1;

    if (velocity == 0) {
      LEDExt.setBrightness(n, 0);
      return;
    }

    const float fraction = (float)velocity / 127;
    LEDExt.setHSV(n, V2Color::Yellow, 0.2, fraction * _lightMax);
  }

  void playNote(uint8_t note, uint8_t velocity) {
    if (note < Config.notes.start)
      return;

    if (note >= Config.notes.start + Config.notes.count)
      return;

    led.flash(0.03, 0.3);
    routeStrings(note, velocity);
  }

  void playAftertouch(uint8_t note, uint8_t pressure) {
    if (note < Config.notes.start)
      return;

    if (note >= Config.notes.start + Config.notes.count)
      return;

    routeStringsAftertouch(note, pressure);
  }

  void setProgram(Program number) {
    _program = number;

    switch (Manual.getMode()) {
      case Manual::Mode::Notes:
        Manual.setColor(_programs[(uint8_t)_program].color);
        break;

      case Manual::Mode::Song:
      case Manual::Mode::Test:
        Manual.splashColor(_programs[(uint8_t)_program].color);
        break;
    }

    sendAllStrings(_midi.setProgram(0, _programs[(uint8_t)_program].number));
  }

  void allNotesOff() {
    if (_force.trigger()) {
      reset();
      return;
    }

    stop();
    sendAllStrings(_midi.setControlChange(0, V2MIDI::CC::AllNotesOff, 0));
  }

private:
  V2Music::ForcedStop _force;

  const struct {
    uint8_t number;
    const char *name;
    V2Color::Hue color;
  } _programs[(uint8_t)Program::_count]{
    [(uint8_t)Program::Bow] =
      {
        .number{V2MIDI::GM::Program::Contrabass},
        .name{"Bow"},
        .color{V2Color::Orange},
      },
    [(uint8_t)Program::Pluck] =
      {
        .number{V2MIDI::GM::Program::PizzicatoStrings},
        .name{"Pizzicato"},
        .color{V2Color::Cyan},
      },
  };
  Program _program{};

  uint8_t _volume{100};
  float _speedMax{100.f / 127.f};
  float _rotationMax{1};
  bool _reverse{};
  struct {
    float rate{};
    float depth{0.5};
  } _vibrato;
  float _pressureMax{1};
  float _lightMax{100.f / 127.f};
  float _rainbow{};
  uint8_t _aftertouch{};

  V2MIDI::Packet _midi{};
  V2Link::Packet _link{};

  struct {
    uint8_t notes[2];
    uint8_t last;
  } _route{};

  void stop() {
    _volume        = 100;
    _speedMax      = 100.f / 127.f;
    _rotationMax   = 1;
    _reverse       = false;
    _vibrato.rate  = 0;
    _vibrato.depth = 0.5;

    _pressureMax = 1;
    _lightMax    = 100.f / 127.f;
    _aftertouch  = 0;
    _route       = {};

    Manual.setMode();
    LEDExt.reset();
  }

  void handleReset() override {
    _program = Program::Bow;
    _force.reset();
    stop();
    sendAllStrings(_midi.set(0, V2MIDI::Packet::Status::SystemReset, 0, 0));
  }

  void routeStrings(uint8_t note, uint8_t velocity) {
    playLight(note, velocity);

    // Free string.
    if (velocity == 0) {
      if (_route.notes[0] == note) {
        sendString(0, _midi.setNote(0, note, 0));
        _route.notes[0] = 0;
      }

      if (_route.notes[1] == note) {
        sendString(1, _midi.setNote(0, note, 0));
        _route.notes[1] = 0;
      }

      return;
    }

    // Try first string.
    if (_route.notes[0] == 0) {
      sendString(0, _midi.setNote(0, note, velocity));
      _route.notes[0] = note;
      _route.last     = 0;
      return;
    }

    // Try second string.
    if (_route.notes[1] == 0) {
      sendString(1, _midi.setNote(0, note, velocity));
      _route.notes[1] = note;
      _route.last     = 1;
      return;
    }

    // Both strings are busy, stop the earlier note, play the new one.
    if (_route.last == 0) {
      playLight(_route.notes[1], 0);
      sendString(1, _midi.setNote(0, _route.notes[1], 0));
      sendString(1, _midi.setNote(0, note, velocity));
      _route.notes[1] = note;
      _route.last     = 1;

    } else {
      playLight(_route.notes[0], 0);
      sendString(0, _midi.setNote(0, _route.notes[0], 0));
      sendString(0, _midi.setNote(0, note, velocity));
      _route.notes[0] = note;
      _route.last     = 0;
    }
  }

  void routeStringsAftertouch(uint8_t note, uint8_t pressure) {
    if (_route.notes[0] == note) {
      playLight(note, pressure);
      sendString(0, _midi.setAftertouchChannel(0, pressure));
    }

    if (_route.notes[1] == note) {
      playLight(note, pressure);
      sendString(1, _midi.setAftertouchChannel(0, pressure));
    }
  }

  void sendString(uint8_t n, V2MIDI::Packet *packet) {
    _link.send(packet);
    Socket.send(n, &_link);
  }

  void sendAllStrings(V2MIDI::Packet *packet) {
    sendString(0, packet);
    sendString(1, packet);
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    playNote(note, velocity);
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    playNote(note, 0);
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    if (channel != 0)
      return;

    switch (controller) {
      case (uint8_t)CC::Volume:
        _volume = value;
        sendAllStrings(_midi.setControlChange(0, controller, value));
        break;

      case (uint8_t)CC::FingerSpeed:
        _speedMax = (float)(value + 1) / 128.f;
        sendAllStrings(_midi.setControlChange(0, controller, value));
        break;

      case (uint8_t)CC::VibratoRate:
        _vibrato.rate = (float)value / 127.f;
        sendAllStrings(_midi.setControlChange(0, controller, value));
        break;

      case (uint8_t)CC::VibratoDepth:
        _vibrato.depth = (float)value / 127.f;
        sendAllStrings(_midi.setControlChange(0, controller, value));
        break;

      case (uint8_t)CC::BowPressure:
        _pressureMax = (float)(value + 1) / 128.f;
        sendAllStrings(_midi.setControlChange(0, controller, value));
        break;

      case (uint8_t)CC::BowSpeed:
        _rotationMax = (float)(value + 1) / 128.f;
        sendAllStrings(_midi.setControlChange(0, controller, value));
        break;

      case (uint8_t)CC::BowReverse:
        _reverse = value > 63;
        sendAllStrings(_midi.setControlChange(0, controller, value));
        break;

      case (uint8_t)CC::Light:
        _lightMax = (float)value / 127.f;
        if (_rainbow > 0.f)
          LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), _lightMax, true);
        break;

      case (uint8_t)CC::Rainbow:
        _rainbow = (float)value / 127.f;
        if (_rainbow <= 0.f)
          LEDExt.reset();
        else
          LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), _lightMax, true);
        break;

      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        break;
    }
  }

  void handleProgramChange(uint8_t channel, uint8_t program) override {
    if (channel != 0)
      return;

    for (uint8_t i = 0; i < (uint8_t)Program::_count; i++) {
      if (_programs[i].number != program)
        continue;

      setProgram((Program)i);
      break;
    }
  }

  void handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure) override {
    playAftertouch(note, pressure);
  }

  void handleAftertouchChannel(uint8_t channel, uint8_t pressure) override {
    if (channel != 0)
      return;

    sendAllStrings(_midi.setAftertouchChannel(0, pressure));
  }

  void handlePitchBend(uint8_t channel, int16_t value) override {
    if (channel != 0)
      return;

    sendAllStrings(_midi.setPitchBend(0, value));
  }

  void handleSystemReset() override {
    reset();
  }

  void exportInput(JsonObject json) override {
    JsonObject jsonAftertouch = json.createNestedObject("aftertouch");
    jsonAftertouch["value"]   = _aftertouch;

    JsonObject jsonPitchbend = json.createNestedObject("pitchbend");

    JsonArray jsonControllers = json.createNestedArray("controllers");
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Volume";
      jsonController["number"]  = (uint8_t)CC::Volume;
      jsonController["value"]   = _volume;
    }
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Finger Speed";
      jsonController["number"]  = (uint8_t)CC::FingerSpeed;
      jsonController["value"]   = (uint8_t)(_speedMax * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Vibrato Rate";
      jsonController["number"]  = (uint8_t)CC::VibratoRate;
      jsonController["value"]   = (uint8_t)(_vibrato.rate * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Vibrato Depth";
      jsonController["number"]  = (uint8_t)CC::VibratoDepth;
      jsonController["value"]   = (uint8_t)(_vibrato.depth * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Bow Pressure";
      jsonController["number"]  = (uint8_t)CC::BowPressure;
      jsonController["value"]   = (uint8_t)(_pressureMax * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Bow Speed";
      jsonController["number"]  = (uint8_t)CC::BowSpeed;
      jsonController["value"]   = (uint8_t)(_rotationMax * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Bow Reverse";
      jsonController["type"]    = "toggle";
      jsonController["number"]  = (uint8_t)CC::BowReverse;
      jsonController["value"]   = (uint8_t)(_reverse ? 127 : 0);
    }
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Brightness";
      jsonController["number"]  = (uint8_t)CC::Light;
      jsonController["value"]   = (uint8_t)(_lightMax * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.createNestedObject();
      jsonController["name"]    = "Rainbow";
      jsonController["number"]  = (uint8_t)CC::Rainbow;
      jsonController["value"]   = (uint8_t)(_rainbow * 127.f);
    }

    JsonArray jsonPrograms = json.createNestedArray("programs");
    for (uint8_t i = 0; i < (uint8_t)Program::_count; i++) {
      JsonObject jsonProgram = jsonPrograms.createNestedObject();
      jsonProgram["name"]    = _programs[i].name;
      jsonProgram["number"]  = _programs[i].number;
      if (i == (uint8_t)_program)
        jsonProgram["selected"] = true;
    }

    JsonObject jsonChromatic = json.createNestedObject("chromatic");
    jsonChromatic["start"]   = Config.notes.start;
    jsonChromatic["count"]   = Config.notes.count;
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "note";
      setting["title"]   = "Notes";
      setting["label"]   = "Start";
      setting["default"] = ConfigurationDefault.notes.start;
      setting["path"]    = "notes/start";
    }
    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "number";
      setting["label"]   = "Count";
      setting["min"]     = 1;
      setting["max"]     = notesMax;
      setting["default"] = ConfigurationDefault.notes.count;
      setting["path"]    = "notes/count";
    }
  }

  void importConfiguration(JsonObject json) override {
    JsonObject jsonNotes = json["notes"];
    if (jsonNotes) {
      if (!jsonNotes["start"].isNull()) {
        uint8_t start = jsonNotes["start"];
        if (start > 127)
          start = 127;

        Config.notes.start = start;
      }

      if (!jsonNotes["count"].isNull()) {
        uint8_t count = jsonNotes["count"];
        if (count < 1)
          count = 1;

        else if (count > notesMax)
          count = notesMax;

        if (count > 128 - Config.notes.start)
          count = 128 - Config.notes.start;

        Config.notes.count = count;
      }
    }
  }

  void exportConfiguration(JsonObject json) override {
    JsonObject jsonNotes = json.createNestedObject("notes");
    jsonNotes["#start"]  = "First note";
    jsonNotes["start"]   = Config.notes.start;
    jsonNotes["#count"]  = "Total number of notes ";
    jsonNotes["count"]   = Config.notes.count;
  }

  virtual void exportSystemMIDIFile(JsonObject json);

  void exportSystem(JsonObject json) override {
    exportSystemMIDIFile(json);
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
  Link() : V2Link(NULL, &Socket) {}

private:
  V2MIDI::Packet _midi{};

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

static class MIDIFile : public V2MIDI::File::Tracks {
public:
  MIDIFile() : V2MIDI::File::Tracks(MIDISong) {}

private:
  bool handleSend(uint16_t track, V2MIDI::Packet *packet) override {
    Device.dispatch(&Device.usb.midi, packet);
    return true;
  }

  void handleStateChange(V2MIDI::File::Tracks::State state) override {
    switch (state) {
      case V2MIDI::File::Tracks::State::Stop:
        Device.allNotesOff();
        for (uint8_t i = 0; i < 8; i++) {
          V2MIDI::Packet midi;

          midi.setPort(i);
          Socket.send(midi.setControlChange(0, V2MIDI::CC::AllNotesOff, 0));
        }
        break;
    }
  }
} MIDIFile;

void Device::exportSystemMIDIFile(JsonObject json) {
  JsonObject jsonTrack = json.createNestedObject("track");
  char s[128];
  if (MIDIFile.copyTag(V2MIDI::File::Event::Meta::Title, s, sizeof(s)) > 0)
    jsonTrack["title"] = s;

  if (MIDIFile.copyTag(V2MIDI::File::Event::Meta::Copyright, s, sizeof(s)) > 0)
    jsonTrack["creator"] = s;
}

static class {
public:
  void stop() {
    Device.allNotesOff();
    _enabled = false;
  }

  void play(Device::Program program) {
    LEDExt.reset();
    LEDExt.rainbow(2, 3, 1, true);

    _enabled  = true;
    _velocity = 10;
    _play     = {};
  }

  void loop() {
    if (!_enabled)
      return;

    playNote();
  }

private:
  void playNote() {
    if (V2Base::getUsecSince(_play.usec) < 500 * 1000)
      return;

    _play.usec = V2Base::getUsec();

    if (_play.note == 0) {
      _play.note = Config.notes.start;
      Device.playNote(_play.note, _velocity);
      Device.playNote(_play.note, _velocity);

    } else if (_play.note < Config.notes.start + Config.notes.count - 1) {
      Device.playNote(_play.note, 0);
      _play.note++;
      Device.playNote(_play.note, _velocity);
      Device.playNote(_play.note, _velocity);

    } else {
      Device.playNote(_play.note, 0);
      _play.note = 0;

      _velocity += 25;
      if (_velocity > 127) {
        stop();
        _enabled = false;
      }
    }
  }

  bool _enabled{};
  uint8_t _velocity{};
  struct {
    uint8_t note;
    uint32_t usec;
  } _play{};
} TestMode;

static class Button : public V2Buttons::Button {
public:
  Button() : V2Buttons::Button(&_config, PIN_BUTTON_REVISION_0) {}

private:
  const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};

  void handleClick(uint8_t count) override {
    switch (count) {
      case 0:
        MIDIFile.stop();
        TestMode.stop();
        Device.reset();
        Device.allNotesOff();
        break;

      case 1 ... static_cast<uint8_t>(Device::Program::_count):
        Device.setProgram(static_cast<Device::Program>(count - 1));
        break;

      case static_cast<uint8_t>(Device::Program::_count) + 1:
        MIDIFile.stop();
        TestMode.stop();
        Device.reset();
        break;
    }
  }

  void handleHold(uint8_t count) override {
    switch (count) {
      case 0:
        Device.allNotesOff();
        Manual.setMode(Manual::Mode::Song);
        MIDIFile.play();
        break;

      case 1:
        Device.allNotesOff();
        Manual.setMode(Manual::Mode::Test);
        TestMode.play(Device::Program::Bow);
        break;
    }
  }
} Button;

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.5);
  LEDExt.begin();
  LEDExt.setMaxBrightness(0.75);

  Socket.begin();
  Device.link = &Link;

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialSocket, 2);

  Button.begin();
  Device.usb.midi.setPortName(1, "Main");
  Device.usb.midi.setPortName(2, "String 1");
  Device.usb.midi.setPortName(3, "String 2");
  Device.begin();
  Device.reset();
}

void loop() {
  LED.loop();
  LEDExt.loop();
  MIDI.loop();
  Link.loop();
  V2Buttons::loop();
  Device.loop();

  switch (Manual.getMode()) {
    case Manual::Mode::Song:
      MIDIFile.loop();
      break;

    case Manual::Mode::Test:
      TestMode.loop();
      break;
  }

  if (Link.idle() && Device.idle())
    Device.sleep();
}
