#include <V2Base.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Music.h>
#include <V2PowerSupply.h>
#include <V2Stepper.h>

V2DEVICE_METADATA("com.versioduo.kontra-2-string", 47, "versioduo:samd:step");

static constexpr uint8_t       notesMax  = 30;
static constexpr uint8_t       nSteppers = 4;
static V2LED::WS2812           LED(nSteppers, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2Link::Port            Plug(&SerialPlug);
static V2Link::Port            Socket(&SerialSocket);
static V2Base::Timer::Periodic Timer(2, 200000);
static V2Base::Analog::ADC     ADC(V2Base::Analog::ADC::getID(PIN_VOLTAGE_SENSE));

static class Stepper : public V2Stepper::Motor {
public:
  enum { Rail, Wheel, Pressure, Pluck };
  Stepper(const Motor::Config conf, uint8_t index) :
    Motor(conf, &Timer, &SPI, PIN_DRIVER_SELECT + index, PIN_DRIVER_STEP + index),
    _index(index) {}

  bool ready{};

private:
  const uint8_t _index;

  void handleMovement(Move move) override {
    switch (move) {
      case Move::Forward:
        LED.setHSV(_index, V2Colour::Cyan, 1, 0.4);
        break;

      case Move::Reverse:
        LED.setHSV(_index, V2Colour::Orange, 1, 0.4);
        break;

      case Move::Stop:
        LED.setHSV(_index, V2Colour::Green, 1, 0.2);
        break;
    }
  }
} Steppers[nSteppers]{
  Stepper(
    {
      .ampere{0.8},
      .microstepsShift{3},
      .home{.speed{250}, .stall{0.06}},
      .speed{.min{100}, .max{2000}, .accel{2500}},
    },
    Stepper::Rail),
  Stepper(
    {
      .ampere{0.6},
      .inverse{true},
      .speed{.min{25}, .max{600}, .accel{600}},
    },
    Stepper::Wheel),
  Stepper(
    {
      .ampere{0.6},
      .microstepsShift{3},
      .inverse{true},
      .home{.speed{250}, .stall{0.06}},
      .speed{.min{100}, .max{250}, .accel{250}},
    },
    Stepper::Pressure),
  Stepper(
    {
      .ampere{0.4},
      .microstepsShift{3},
      .home{.speed{200}, .stall{0.02}},
      .speed{.min{25}, .max{1500}, .accel{3000}},
    },
    Stepper::Pluck),
};

static class Power : public V2PowerSupply {
public:
  Power() : V2PowerSupply({.min{6}, .max{26}}) {}

  void begin() {
    pinMode(PIN_DRIVER_ENABLE, OUTPUT);
    digitalWrite(PIN_DRIVER_ENABLE, HIGH);
  }

private:
  float handleMeasurement() override {
    // A voltage 10/100k divider.
    return 36.f * ADC.readChannel(V2Base::Analog::ADC::getChannel(PIN_VOLTAGE_SENSE));
  }

  void handleOn() override {
    digitalWrite(PIN_DRIVER_ENABLE, LOW);
  }

  void handleOff() override {
    digitalWrite(PIN_DRIVER_ENABLE, HIGH);
  }

  void handleNotify(float voltage) override {
    // Power interruption, or commands without a power connection show yellow LEDs.
    if (voltage < config.min) {
      LED.splashHSV(0.5, V2Colour::Yellow, 1, 0.5);
      return;
    }

    // Over-voltage shows red LEDs.
    if (voltage > config.max) {
      LED.splashHSV(0.5, V2Colour::Red, 1, 1);
      return;
    }

    // The number of green LEDs shows the voltage.
    float   fraction = voltage / (float)config.max;
    uint8_t n        = ceil((float)nSteppers * fraction);
    LED.splashHSV(0.5, 0, n, V2Colour::Green, 1, 0.5);
  }
} Power;

// Config, written to EEPROM.
static constexpr struct Configuration {
  struct {
    // The middle C, MIDI note 60, in this mapping is C3.
    uint8_t start{V2MIDI::E(0)};
    uint8_t count{29};
  } notes;

  struct {
    // Offsets in millimeters.
    float home{3.5};
    float min{2};
    float max{4};
  } bow;

  struct {
    // Overall string length in millimeters.
    float length{1165};

    // Offset in millimeters from the home position to the first note.
    float home{5};
  } string;
} ConfigurationDefault;

static struct Configuration Config{ConfigurationDefault};

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "V2 kontra-2-string";
    metadata.description = "2 String Double Bass - String Controller";
    metadata.home        = "https://versioduo.com/#kontra-2";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
    usb.pid = 0xe960;

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
  };

private:
  uint32_t            _timeoutUsec{};
  bool                _ready{};
  V2Music::ForcedStop _force;
  const struct {
    uint8_t     number;
    const char* name;
  } _programs[(uint8_t)Program::_count]{
    [(uint8_t)Program::Bow]   = {.number{V2MIDI::GM::Program::Contrabass}, .name{"Bow"}},
    [(uint8_t)Program::Pluck] = {.number{V2MIDI::GM::Program::PizzicatoStrings}, .name{"Pizzicato"}},
  };
  Program                    _program{};
  V2Music::Playing<notesMax> _playing;

  void handleLoop() override {
    if (_timeoutUsec > 0 && V2Base::getUsecSince(_timeoutUsec) > 900 * 1000 * 1000) {
      _timeoutUsec = 0;
      reset();
    }

    _rail.loop();
  }

  void handleReset() override {
    _program     = Program::Bow;
    _timeoutUsec = 0;
    _ready       = false;
    _force.reset();
    _program = Program::Bow;
    _playing.reset();
    _rail.reset();
    _wheel.reset();
    _pluck.reset();

    for (uint8_t i = 0; i < nSteppers; i++)
      Steppers[i].reset();

    LED.reset();
    Power.off();
  }

  void allNotesOff() {
    _playing.reset();

    if (!power())
      return;

    _playing.reset();
    _rail.stop();
    _wheel.stop();
    _pluck.stop();

    if (!_ready || _force.trigger()) {
      _program = Program::Bow;
      _rail.home();
      _wheel.home();
      _pluck.home();
      _ready = true;
    }
  }

  class {
  public:
    float speedMax{100.f / 127.f};
    float pitchbend{};
    struct {
      float rate{};
      float depth{0.5};
    } vibrato;

    void home() {
      Steppers[Stepper::Rail].ready = false;

      static const auto railReady = []() {
        Steppers[Stepper::Rail].ready = true;
      };

      static const auto railHome = []() {
        Steppers[Stepper::Rail].hold(0.2);

        // ~1.1m rail, GT2 40 teeth wheel == 80 mm, 200 steps per rotation.
        const float max  = (1500.f / 80.f) * 200.f;
        const float home = 8 + (Config.string.home / 80.f) * 200.f;
        Steppers[Stepper::Rail].home(max, home, railReady);
      };

      // Move a few steps back to be able to move towards the home postiion and detect the stalling.
      const float position = Steppers[Stepper::Rail].getPosition();
      Steppers[Stepper::Rail].setPosition(position + 16, 1, railHome);
      _note = 0;
    }

    void position(uint8_t note) {
      _note = note;
      update();
    }

    void update() {
      if (_note == 0)
        return;

      if (!Steppers[Stepper::Rail].ready)
        return;

      float steps         = getNotePosition(_note - Config.notes.start);
      float stepsTwoNotes = 0;

      if (pitchbend < 0) {
        const uint8_t twoNotes = max((int8_t)(_note - Config.notes.start) - 2, 0);
        stepsTwoNotes          = steps - getNotePosition(twoNotes);

      } else {
        const uint8_t twoNotes = min(_note - Config.notes.start + 2, Config.notes.count - 1);
        stepsTwoNotes          = getNotePosition(twoNotes) - steps;
      }
      steps += stepsTwoNotes * pitchbend;

      // A typical string vibrato is 5-8 Hz, 0.2-0.4 semitones.
      if (vibrato.rate > 0) {
        const uint8_t note     = _note - Config.notes.start;
        const float   oneNote  = getNotePosition(note + 1) - getNotePosition(note);
        const float   fraction = 0.01f + (0.2f * powf(vibrato.depth, 1.5));
        steps += oneNote * fraction * (_vibrato.high ? 1.f : -1.f);
      }

      Steppers[Stepper::Rail].setPosition(steps, speedMax);
    }

    void loop() {
      if (_note == 0)
        return;

      if (vibrato.rate <= 0)
        return;

      // A typical string vibrato is 5-8 Hz, 0.2-0.4 semitones.
      const float hz = 5 + (3 * vibrato.rate);
      if (V2Base::getUsecSince(_vibrato.usec) < (1000.f * 1000.f) / hz)
        return;

      _vibrato.high = !_vibrato.high;
      _vibrato.usec = V2Base::getUsec();
      update();
    }

    void stop() {
      speedMax      = 100.f / 127.f;
      pitchbend     = 0;
      vibrato.rate  = 0;
      vibrato.depth = 0.5;
      _vibrato      = {};
      _note         = 0;

      if (Steppers[Stepper::Rail].ready)
        Steppers[Stepper::Rail].stop();
    }

    void reset() {
      stop();
      Steppers[Stepper::Rail].ready = false;
    }

  private:
    uint8_t _note{};
    struct {
      uint32_t usec{};
      bool     high{};
    } _vibrato{};

    // Get the number of steps to shorten the string by, to play the n-th note above the base
    // note, 'length' is the vibrating string length, ~1.1m for a double bass.
    float getNotePosition(uint8_t n) {
      return (V2Music::String::getNoteDistance(n, Config.string.length) / 80.f) * 200.f;
    }
  } _rail;

  class {
  public:
    uint8_t volume{100};
    float   rotationMax{1};
    float   pressureMax{1};
    bool    reverse{};
    uint8_t aftertouch{0};

    void home() {
      aftertouch = 0;
      _note      = 0;
      _velocity  = 0;

      Steppers[Stepper::Wheel].stop();
      Steppers[Stepper::Pressure].hold(0.2);
      Steppers[Stepper::Pressure].ready = false;

      Steppers[Stepper::Pressure].home(200, 8 + Config.bow.home * 10.f, []() { Steppers[Stepper::Pressure].ready = true; });
    }

    void play(uint8_t note, uint8_t velocity) {
      if (velocity == 0) {
        _velocity = 0;
        stop();
        return;
      }

      aftertouch = 0;
      _note      = note;
      _velocity  = velocity;
      update();
    }

    float adjustVolume(float fraction) {
      if (volume < 100) {
        const float range = (float)volume / 100.f;
        return fraction * range;
      }

      const float range = (float)(volume - 100) / 27.f;
      return powf(fraction, 1 - (0.5f * range));
    }

    void update() {
      if (_velocity == 0)
        return;

      if (!Steppers[Stepper::Pressure].ready)
        return;

      uint8_t v = _velocity;

      if (aftertouch > 0)
        v = aftertouch;

      float fraction = (float)v / 127.f;
      fraction       = adjustVolume(fraction);

      // Reduce the pressure for higher pitched notes.
      const float reduction = 1 - (0.5f * ((float)(_note - Config.notes.start) / (float)Config.notes.count));

      rotate(fraction * rotationMax * (reverse ? -1.f : 1.f));
      pressure(fraction * reduction * pressureMax);
    }

    void stop() {
      volume      = 100;
      rotationMax = 1;
      pressureMax = 1;
      reverse     = false;
      aftertouch  = 0;
      _note       = 0;
      _velocity   = 0;

      if (Steppers[Stepper::Pressure].ready)
        Steppers[Stepper::Pressure].setPosition(0);

      rotate(0);
    }

    void reset() {
      stop();
      Steppers[Stepper::Pressure].ready = false;
    }

  private:
    uint8_t _note     = 0;
    uint8_t _velocity = 0;

    void rotate(float speed) {
      Steppers[Stepper::Wheel].rotate(speed);
    }

    void pressure(float pressure) {
      const float min = Config.bow.min * 10.f;
      const float max = Config.bow.max * 10.f;
      const float p   = min + ((max - min) * pressure);
      Steppers[Stepper::Pressure].setPosition(p, 0.5);
    }
  } _wheel;

  class {
  public:
    void home() {
      Steppers[Stepper::Pluck].hold(0.2);
      Steppers[Stepper::Pluck].ready = false;
      Steppers[Stepper::Pluck].home(250, 55, []() { Steppers[Stepper::Pluck].ready = true; });
      _back = false;
    }

    void play(uint8_t velocity) {
      if (velocity == 0)
        return;

      const float speed = (float)velocity / 127.f;
      if (Steppers[Stepper::Pluck].ready)
        Steppers[Stepper::Pluck].setPosition(_back ? 10 : 90, speed);

      _back = !_back;
    }

    void stop() {
      _back = false;

      if (Steppers[Stepper::Pluck].ready)
        Steppers[Stepper::Pluck].setPosition(_back ? 90 : 10);
    }

    void reset() {
      stop();
      Steppers[Stepper::Pluck].ready = false;
    }

  private:
    bool _back = false;
  } _pluck;

  bool power() {
    bool continuous;
    if (!Power.on(continuous))
      return false;

    if (!continuous) {
      for (uint8_t i = 0; i < nSteppers; i++)
        Steppers[i].reset();

      _ready = false;
    }

    return true;
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    _timeoutUsec = V2Base::getUsec();

    if (note < Config.notes.start)
      return;

    if (note >= Config.notes.start + Config.notes.count)
      return;

    if (!power())
      return;

    if (!_ready)
      allNotesOff();

    switch (_program) {
      case Program::Bow:
        _pluck.stop();
        _playing.update(note, velocity);

        // Restore previous note.
        if (velocity == 0) {
          uint8_t n;
          uint8_t v;
          if (_playing.getLast(n, v)) {
            _rail.position(n);
            _wheel.play(n, v);
            return;
          }

          // Stop playing note.
          _wheel.stop();
          return;
        }

        _rail.position(note);
        _wheel.play(note, velocity);
        break;

      case Program::Pluck:
        _wheel.stop();
        _playing.reset();

        if (velocity > 0)
          _rail.position(note);

        _pluck.play(velocity);
        break;
    }
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    _timeoutUsec = V2Base::getUsec();
    handleNote(channel, note, 0);
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    if (channel != 0)
      return;

    _timeoutUsec = V2Base::getUsec();

    switch (controller) {
      case (uint8_t)CC::Volume:
        _wheel.volume = value;
        _wheel.update();
        break;

      case (uint8_t)CC::FingerSpeed:
        _rail.speedMax = (float)(value + 1) / 128.f;
        _rail.update();
        break;

      case (uint8_t)CC::VibratoRate:
        _rail.vibrato.rate = (float)value / 127.f;
        _rail.update();
        break;

      case (uint8_t)CC::VibratoDepth:
        _rail.vibrato.depth = (float)value / 127.f;
        _rail.update();
        break;

      case (uint8_t)CC::BowPressure:
        _wheel.pressureMax = (float)(value + 1) / 128.f;
        _wheel.update();
        break;

      case (uint8_t)CC::BowSpeed:
        _wheel.rotationMax = (float)(value + 1) / 128.f;
        _wheel.update();
        break;

      case (uint8_t)CC::BowReverse:
        _wheel.reverse = value > 63;
        _wheel.update();
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

      _program = (Program)i;
      break;
    }
  }

  void handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure) override {
    _timeoutUsec = V2Base::getUsec();

    uint8_t n;
    uint8_t v;
    if (!_playing.getLast(n, v))
      return;

    if (n != note)
      return;

    _wheel.aftertouch = pressure;
    _wheel.update();
  }

  void handleAftertouchChannel(uint8_t channel, uint8_t pressure) override {
    if (channel != 0)
      return;

    _timeoutUsec = V2Base::getUsec();

    _wheel.aftertouch = pressure;
    _wheel.update();
  }

  void handlePitchBend(uint8_t channel, int16_t value) override {
    if (channel != 0)
      return;

    _timeoutUsec = V2Base::getUsec();

    _rail.pitchbend = (float)value / (value < 0 ? 8192.f : 8191.f);
    _rail.update();
  }

  void handleSystemReset() override {
    reset();
  }

  void exportSystem(JsonObject json) override {
    JsonObject jsonPower       = json["power"].to<JsonObject>();
    jsonPower["voltage"]       = serialized(String(Power.getVoltage(), 1));
    jsonPower["interruptions"] = Power.getInterruptions();
  }

  void exportInput(JsonObject json) override {
    {
      JsonObject jsonAftertouch = json["aftertouch"].to<JsonObject>();
      jsonAftertouch["value"]   = _wheel.aftertouch;
    }

    {
      JsonObject jsonPitchbend = json["pitchbend"].to<JsonObject>();
      jsonPitchbend["value"]   = (int16_t)(_rail.pitchbend * (_rail.pitchbend < 0 ? 8192.f : 8191.f));
    }

    JsonArray jsonControllers = json["controllers"].to<JsonArray>();
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Volume";
      jsonController["number"]  = (uint8_t)CC::Volume;
      jsonController["value"]   = _wheel.volume;
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Finger Speed";
      jsonController["number"]  = (uint8_t)CC::FingerSpeed;
      jsonController["value"]   = (uint8_t)(_rail.speedMax * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Vibrato Rate";
      jsonController["number"]  = (uint8_t)CC::VibratoRate;
      jsonController["value"]   = (uint8_t)(_rail.vibrato.rate * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Vibrato Depth";
      jsonController["number"]  = (uint8_t)CC::VibratoDepth;
      jsonController["value"]   = (uint8_t)(_rail.vibrato.depth * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Bow Pressure";
      jsonController["number"]  = (uint8_t)CC::BowPressure;
      jsonController["value"]   = (uint8_t)(_wheel.pressureMax * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Bow Speed";
      jsonController["number"]  = (uint8_t)CC::BowSpeed;
      jsonController["value"]   = (uint8_t)(_wheel.rotationMax * 127.f);
    }
    {
      JsonObject jsonController = jsonControllers.add<JsonObject>();
      jsonController["name"]    = "Bow Reverse";
      jsonController["type"]    = "toggle";
      jsonController["number"]  = (uint8_t)CC::BowReverse;
      jsonController["value"]   = (uint8_t)(_wheel.reverse ? 127 : 0);
    }

    {
      JsonArray jsonPrograms = json["programs"].to<JsonArray>();
      for (uint8_t i = 0; i < (uint8_t)Program::_count; i++) {
        JsonObject jsonProgram = jsonPrograms.add<JsonObject>();
        jsonProgram["name"]    = _programs[i].name;
        jsonProgram["number"]  = _programs[i].number;
        if (i == (uint8_t)_program)
          jsonProgram["selected"] = true;
      }
    }

    {
      JsonObject jsonChromatic = json["chromatic"].to<JsonObject>();
      jsonChromatic["start"]   = Config.notes.start;
      jsonChromatic["count"]   = Config.notes.count;
    }
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "note";
      setting["title"]   = "Notes";
      setting["label"]   = "Start";
      setting["default"] = ConfigurationDefault.notes.start;
      setting["path"]    = "notes/start";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";
      setting["label"]   = "Count";
      setting["min"]     = 1;
      setting["max"]     = notesMax;
      setting["default"] = ConfigurationDefault.notes.count;
      setting["path"]    = "notes/count";
    }

    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";

      setting["title"]   = "Bow";
      setting["label"]   = "Home";
      setting["min"]     = 0;
      setting["max"]     = 10;
      setting["step"]    = 0.1;
      setting["default"] = ConfigurationDefault.bow.home;
      setting["path"]    = "bow/home";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";

      setting["label"]   = "Minimum";
      setting["min"]     = 0;
      setting["max"]     = 10;
      setting["step"]    = 0.1;
      setting["default"] = ConfigurationDefault.bow.min;
      setting["path"]    = "bow/min";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";

      setting["label"]   = "Maximum";
      setting["min"]     = 0;
      setting["max"]     = 10;
      setting["step"]    = 0.1;
      setting["default"] = ConfigurationDefault.bow.max;
      setting["path"]    = "bow/max";
    }

    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";

      setting["title"]   = "String";
      setting["label"]   = "Length";
      setting["min"]     = 1;
      setting["max"]     = 1200;
      setting["step"]    = 0.1;
      setting["default"] = ConfigurationDefault.string.length;
      setting["path"]    = "string/length";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";

      setting["label"]   = "Home";
      setting["min"]     = 0;
      setting["max"]     = 50;
      setting["step"]    = 0.1;
      setting["default"] = ConfigurationDefault.string.home;
      setting["path"]    = "string/home";
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

    JsonObject jsonBow = json["bow"];
    if (jsonBow) {
      if (!jsonBow["home"].isNull()) {
        float home = jsonBow["home"];
        if (home < 0.f)
          home = 0;
        else if (home > 10.f)
          home = 10;

        Config.bow.home = home;
      }

      if (!jsonBow["min"].isNull()) {
        float min = jsonBow["min"];
        if (min < 0.f)
          min = 0;
        else if (min > 10.f)
          min = 10;

        Config.bow.min = min;
      }

      if (!jsonBow["max"].isNull()) {
        float max = jsonBow["max"];
        if (max < 0.f)
          max = 0;
        else if (max > 10.f)
          max = 10;

        Config.bow.max = max;
      }
    }

    JsonObject jsonString = json["string"];
    if (jsonString) {
      if (!jsonString["length"].isNull()) {
        float length = jsonString["length"];
        if (length < 1.f)
          length = 1;
        else if (length > 1200.f)
          length = 1200;

        Config.string.length = length;
      }

      if (!jsonString["home"].isNull()) {
        float home = jsonString["home"];
        if (home < 0.f)
          home = 0;
        else if (home > 50.f)
          home = 50;

        Config.string.home = home;
      }
    }
  }

  void exportConfiguration(JsonObject json) override {
    JsonObject jsonNotes = json["notes"].to<JsonObject>();
    jsonNotes["#start"]  = "First note";
    jsonNotes["start"]   = Config.notes.start;
    jsonNotes["#count"]  = "Total number of notes ";
    jsonNotes["count"]   = Config.notes.count;

    JsonObject jsonBow = json["bow"].to<JsonObject>();
    jsonBow["#home"]   = "Offset of home position in millimeters";
    jsonBow["home"]    = serialized(String(Config.bow.home, 1));
    jsonBow["#min"]    = "Offset for velocity 1 in millimeters";
    jsonBow["min"]     = serialized(String(Config.bow.min, 1));
    jsonBow["#max"]    = "Offset for velocity 127 in millimeters";
    jsonBow["max"]     = serialized(String(Config.bow.max, 1));

    JsonObject jsonString = json["string"].to<JsonObject>();
    jsonString["#length"] = "Total string length in millimeters";
    jsonString["length"]  = serialized(String(Config.string.length, 1));
    jsonString["#home"]   = "Offset of first note in millimeters";
    jsonString["home"]    = serialized(String(Config.string.home, 1));
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
  void receivePlug(V2Link::Packet* packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      packet->receive(&_midi);
      Device.dispatch(&Plug, &_midi);
    }
  }

  // Forward children device events to the host
  void receiveSocket(V2Link::Packet* packet) override {
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
  SPI.begin();

  LED.begin();
  LED.setMaxBrightness(0.5);

  Plug.begin();
  Socket.begin();
  Device.link = &Link;

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialPlug, 2);
  setSerialPriority(&SerialSocket, 2);

  Power.begin();
  for (uint8_t i = 0; i < nSteppers; i++)
    Steppers[i].begin();

  // The priority needs to be lower than the SERCOM priorities.
  Timer.begin([]() {
    for (uint8_t i = 0; i < nSteppers; i++)
      Steppers[i].tick();
  });
  Timer.setPriority(3);

  ADC.begin();
  ADC.addChannel(V2Base::Analog::ADC::getChannel(PIN_VOLTAGE_SENSE));

  Device.begin();
  Device.reset();
}

void loop() {
  for (uint8_t i = 0; i < nSteppers; i++)
    Steppers[i].loop();

  LED.loop();
  MIDI.loop();
  Link.loop();
  Power.loop();
  Device.loop();

  if (Link.idle() && Device.idle())
    Device.sleep();
}
