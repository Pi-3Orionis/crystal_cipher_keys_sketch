// SPI
#include <SPI.h>

// Adafruit library for controlling MCP23S17_SOs GPIO extenders.
#include <Adafruit_MCP23X17.h>

// NeoPixel control library
#include <Adafruit_NeoPixel.h>

// SPI pins
#define PIN_SCK 11
#define PIN_MISO 10
#define PIN_MOSI 12
#define PIN_GPIO_U1_CS 16
#define PIN_GPIO_U2_CS 15
#define PIN_LED_CS 4

// Glyph count, 26 letters in the alphabet.
#define GLYPH_COUNT 26

// GPIO pin counts.
#define U1_PIN_COUNT 16
#define U2_PIN_COUNT 10

/*
The states for the program's main state machine execution loop.
*/
enum ProgramState {
  WAITING_FOR_CRYSTAL,
  ANNOUNCE_RIDDLE,
  WAITING_FOR_ANSWER,
  CRYSTAL_PULLED,
  RIDDLE_ANSWERED
};

/*
The states for glyphs. This largely influences how each glyph responds to input
as well as how it is lit up.
*/
enum GlyphState {
  IDLE_DARK,
  IDLE_FLICKER_ON,
  IDLE_FLICKER_OFF,
  WAITING_FOR_PUSH,
  INCORRECT_PUSH,
  CORRECT_PUSH,
  LOCK_SUBSEQUENT,
  WAITING_FOR_SUBSEQUENT,
  LOCK_COMPLETE,
  TRANSFER_TO_CRYSTAL,
  RESET_TO_DARK
};

/*
LED color.
*/
typedef struct {
  byte red;
  byte green;
  byte blue;
} Color;

// Answer strings.
// Must be all uppercase, alphabetical characters only.
const int answerCount = 1;
const char* answers[answerCount] = {
  "PARK"
};

Adafruit_MCP23X17 gpioU1;
Adafruit_MCP23X17 gpioU2;
Adafruit_NeoPixel leds(GLYPH_COUNT, PIN_LED_CS);

ProgramState currentState;
ProgramState toState;
char* currentAnswer;
int currentLetter;

Color dark;
Color wrong_red;
Color good_green;
Color subsequent_cyan;
const byte slowSpeed = 8;
const byte fastSpeed = 32;

/*
Glyph is a struct that encapsulates an entire glyph button.
*/
typedef struct {
  GlyphState state;
  Color currentColor;
  Color toColor;
  byte speed;     // The delta of the largest color component change per pulse.
  byte keyframe;  // 255 signals that the animation is complete.
  byte remainingLetterCount;
} Glyph;

Glyph glyphs[GLYPH_COUNT];

void setup() {
  // Dark.
  dark.red = 0;
  dark.green = 0;
  dark.blue = 0;

  // Wrong Red.
  wrong_red.red = 255;
  wrong_red.green = 8;
  wrong_red.blue = 16;

  // Good Green.
  good_green.red = 8;
  good_green.green = 255;
  good_green.blue = 16;

  // Subsequent Cyan.
  subsequent_cyan.red = 8;
  subsequent_cyan.green = 192;
  subsequent_cyan.blue = 255;

  // Initialize program state variables.
  currentState = WAITING_FOR_CRYSTAL;
  toState = WAITING_FOR_CRYSTAL;

  // Initialize serial output for debugging
  Serial.begin(115200);

  // Initialize local pin modes.
  pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_MISO, OUTPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_LED_CS, OUTPUT);

  // Intialize SPI.
  SPI.setFrequency(100000);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

  // Initialize the GPIO extenders. Manages their own SPI init.
  // Looking into the library, to some extent these GPIO libraries
  // reset settings from earlier, namely SPI frequency, but I'm
  // retaining the full SPI init above for clarity.
  if (!gpioU1.begin_SPI(PIN_GPIO_U1_CS)) {
    Serial.println("Error: Unable to initialize SPI for GPIO U1.");
    while (true);
  }
  for (int i = 0; i < U1_PIN_COUNT; i++) {
    gpioU1.pinMode(i, INPUT);
  }

  if (!gpioU2.begin_SPI(PIN_GPIO_U2_CS)) {
    Serial.println("Error: Unable to initialize SPI for GPIO U2.");
    while (true);
  }
  for (int i = 0; i < U2_PIN_COUNT; i++) {
    gpioU2.pinMode(i, INPUT);
  }

  // Initialize LED control.
  leds.begin();
}

void loop() {
  // Check for crystal presence, which is the key to making the puzzle active.
  if (!isCrystalPresent()) {
    // No crystal is detected as present.
    if (currentState != WAITING_FOR_CRYSTAL) {
      // Pulling the crystal causes the puzzle to go to the "crystal pulled"
      // state, as an LED animation, which quickly reverts to "waiting for
      // crystal."
      // Essentially, it resets the puzzle completely.
      moveProgramState(CRYSTAL_PULLED);
    }
    // Otherwise, linger in the idle "waiting for crystal" state.
  } else {
    // Crystal is in the IR socket.
    if (currentState == WAITING_FOR_CRYSTAL) {
      // We are detecting the crystal's insertion, so move to riddle delivery.
      moveProgramState(ANNOUNCE_RIDDLE);
    }
    // Otherwise it's safe to assume we've already cycled a crystal insertion.
    // Whatever the current state is should persist beyond this check.
  }

  // Cycle through glyphs
  for (int i = 0; i < GLYPH_COUNT; i++) {
    checkGlyph(i);
  }
  leds.show();

  // Check for riddle answered.
  if (currentState == WAITING_FOR_ANSWER) {
    if (currentAnswer[currentLetter] == '\0') {
      // We've reached the end of the answer string, so the answer is successfully
      // completed.
      moveProgramState(RIDDLE_ANSWERED);
    } else {
      // We're still in the midst of solving the riddle.
      // If the next letter to guess is a subsequent lock, we need to change
      // it to "waiting for subsequent" which visually cues the user that a
      // previously locked letter has now unlocked and needs to be pressed.
      int letter = currentAnswer[currentLetter] - 65;
      Glyph glyph = glyphs[letter];
      if (glyph.state == LOCK_SUBSEQUENT) {
        glyph.state = WAITING_FOR_SUBSEQUENT;
        setColor(glyph, subsequent_cyan, fastSpeed);
      }
    }
  }
}

/*
Change the current state of the program's state machine.
*/
void moveProgramState(ProgramState newState) {
  if (currentState == newState) {
    return;
  }

  toState = newState;
}

/*
Run all glyph-specific checks that are iterated over the full set of glyphs.
*/
void checkGlyph(int index) {
  // Fetch the Glyph instance.
  Glyph glyph = glyphs[index];
  GlyphState state = glyph.state;

  bool checkForPress = (currentState == WAITING_FOR_ANSWER)
                       && (currentAnswer[currentLetter] != '\0')  // Riddle is now solved.
                       && ((state == WAITING_FOR_PUSH)
                           || (state == WAITING_FOR_SUBSEQUENT))
                       && isGlyphPressed(index);

  if (checkForPress) {
    // Get the index for the current letter of the riddle.
    int letter = currentAnswer[currentLetter] - 65;

    if (letter == index) {
      // Correct letter press!
      if (--glyph.remainingLetterCount < 1) {
        glyph.state = LOCK_COMPLETE;
      } else {
        glyph.state = LOCK_SUBSEQUENT;
      }
      setColor(glyph, good_green, fastSpeed);
      currentLetter++;
    } else {
      // Incorrect letter press.
      glyph.state = INCORRECT_PUSH;
      setColor(glyph, wrong_red, fastSpeed);
    }
  }

  if (glyph.keyframe < 255) {
    // Animate the glyph's LED.
    byte speed = glyph.speed;

    bool finished = true;
    finished = finished && adjustColorComponent(glyph.currentColor.red, glyph.toColor.red, speed);
    finished = finished && adjustColorComponent(glyph.currentColor.green, glyph.toColor.green, speed);
    finished = finished && adjustColorComponent(glyph.currentColor.blue, glyph.toColor.blue, speed);

    // Check for finished animation.
    if (finished || (255 - glyph.keyframe) < speed) {
      glyph.keyframe = 255;
    } else {
      glyph.keyframe += speed;
    }
    syncColor(index, glyph);
  } else {
    // We're at the end of an animation cycle.
    // Check for transitions to follow-up or "resting" glyph states.
    switch (state) {
      case IDLE_FLICKER_ON:
        glyph.state = IDLE_FLICKER_OFF;
        setColor(glyph, dark, slowSpeed);
        break;
      case IDLE_FLICKER_OFF:
        glyph.state = IDLE_DARK;
        break;
      case INCORRECT_PUSH:
        glyph.state = WAITING_FOR_PUSH;
        setColor(glyph, dark, slowSpeed);
        break;
      case CORRECT_PUSH:
        if (glyph.remainingLetterCount < 1) {
          glyph.state = LOCK_COMPLETE;
        } else {
          glyph.state = LOCK_SUBSEQUENT;
        }
        break;
      case TRANSFER_TO_CRYSTAL:
        glyph.state = RESET_TO_DARK;
        setColor(glyph, dark, fastSpeed);
        break;
      case RESET_TO_DARK:
        glyph.state = IDLE_DARK;
        break;
    }
  }
}

/*
Trigger the playback of a sound.
*/
void playSound(int index) {
  // TODO
}

/*
Returns true only if a crystal is detected in the IR slot.
*/
bool isCrystalPresent() {
  // TODO
  return false;
}

/*
Returns true if the glyph is currently being pressed down.
*/
bool isGlyphPressed(int index) {
  Adafruit_MCP23X17 *gpio;
  int offset = 0;
  if (index >= U1_PIN_COUNT) {
    gpio = &gpioU2;
    offset += U1_PIN_COUNT;
  } else {
    gpio = &gpioU1;
  }
  return gpio->digitalRead(index - offset);  
}

/*
Sync the color, i.e. push it via the NeoPixel library. All of the necessary
data is carried on the Glyph state object.
*/
void syncColor(int index, Glyph glyph) {
  Color color = glyph.currentColor;
  leds.setPixelColor(index, color.red, color.blue, color.green);
}

/*
Adjust one of the color components based on speed value. Returns true if the
value to end at has been or will be reached.
*/
bool adjustColorComponent(byte& currentValue, byte toValue, byte speed) {
  if (currentValue == toValue) {
    // No change.
    return true;
  }
  if (currentValue > toValue) {
    // Subraction.
    byte delta = currentValue - toValue;
    if (delta > speed) {
      currentValue -= speed;
    } else {
      currentValue = toValue;
    }
  } else {
    // Addition.
    byte delta = toValue - currentValue;
    if (delta > speed) {
      currentValue += speed;
    } else {
      currentValue = toValue;
    }
  }
  return currentValue == toValue;
}

/*
Set a new color for the glyph, which sets all the values for the animation.
*/
void setColor(Glyph glyph, Color toColor, byte speed) {
  glyph.toColor = toColor;
  if (speed < 1) {
    glyph.speed = 1;
  } else {
    glyph.speed = speed;
  }
  glyph.keyframe = 0;
}
