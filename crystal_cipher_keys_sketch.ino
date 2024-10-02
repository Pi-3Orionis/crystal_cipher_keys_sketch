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

// Glyph count, equivalent letters A through F.
#define GLYPH_COUNT 6

// GPIO pin counts.
#define U1_PIN_COUNT 16
#define U2_PIN_COUNT 10

/*
The states for the program's main state machine execution loop.
*/
enum ProgramState {
  WAITING_FOR_CRYSTAL,
  BEGIN_PUZZLE,
  SHOW_SEQUENCE,
  WAITING_FOR_PRESS,
  CRYSTAL_PULLED,
  PUZZLE_SOLVED
};

/*
The states for glyphs. This largely influences how each glyph responds to input
as well as how it is lit up.
*/
enum GlyphState {
  IDLE_DARK,
  IDLE_FLICKER_ON,
  IDLE_FLICKER_OFF,
  SIGNAL_FLASH,
  INCORRECT_PUSH,
  CORRECT_PUSH,
  RESET_TO_DARK
};

/*
LED color.
*/
struct Color {
  byte red;
  byte green;
  byte blue;
};

/*
Glyph is a struct that encapsulates an entire glyph button.
*/
struct Glyph {
  GlyphState state;
  Color currentColor;
  Color toColor;
  byte speed;     // The delta of the largest color component change per pulse.
  byte keyframe;  // 255 signals that the animation is complete.
};

// Answer strings.
// Must be all uppercase, alphabetical characters only.
const int answerCount = 1;
const char* answers[answerCount] = {
  "ABCDEF"
};

Adafruit_MCP23X17 gpioU1;
Adafruit_MCP23X17 gpioU2;
Adafruit_NeoPixel leds(GLYPH_COUNT, PIN_LED_CS);

ProgramState currentState;
ProgramState toState;

const char* answerSequence;
int revealedIndex;
int showIndex;
int pressIndex;

Color dark;
Color wrong_red;
Color good_green;
Color signal_cyan;
const byte slowSpeed = 8;
const byte fastSpeed = 32;

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

  // Signal Cyan.
  signal_cyan.red = 8;
  signal_cyan.green = 192;
  signal_cyan.blue = 255;

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

  // Initialize glyphs.
  clearGlyphs();
}

void loop() {
  // Check for crystal presence, which is the key to making the puzzle active.
  if (!isCrystalPresent()) {
    // No crystal is detected as present.
    if (currentState != WAITING_FOR_CRYSTAL && currentState != CRYSTAL_PULLED) {
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
      moveProgramState(BEGIN_PUZZLE);
    }
    // Otherwise it's safe to assume we've already cycled a crystal insertion.
    // Whatever the current state is should persist beyond this check.
  }

  // Process program state transitions.
  int randomSequence = millis() % answerCount;
  int glyphId = answerSequence[0] - 65;
  if (currentState != toState) {
    switch (toState) {
      case BEGIN_PUZZLE:
        // More complex intro? For now just select a random sequence and
        // show it to players.
        answerSequence = answers[randomSequence];
        revealedIndex = 0;
        moveProgramState(SHOW_SEQUENCE);
        break;
      case SHOW_SEQUENCE:
        // Reset indices for tracking progress across sequence.
        showIndex = 0;
        pressIndex = 0;
        // Start animation on the first glyph.
        setColor(glyphs[glyphId], signal_cyan, fastSpeed);
        break;
      case CRYSTAL_PULLED:
        // Crystal was removed prematurely.
        // TODO: More than just turn off glyph lights?
        clearGlyphs();
        moveProgramState(WAITING_FOR_CRYSTAL);
        break;
      case PUZZLE_SOLVED:
        // TODO: More detail? For now just flash the glyphs all green.
        // Also, how to 'send' success to crystal?
        flashSuccessful();
        // The program will linger in this state until the crystal is
        // pulled.
    }
    currentState = toState;
  }

  // Cycle through glyphs, and track if we're awaiting animations to finish.
  bool animationFinished = true;
  for (int i = 0; i < GLYPH_COUNT; i++) {
    animationFinished = animationFinished && checkGlyph(i);
  }
  leds.show();

  // If animations are still running, suspend the rest of the program logic.
  if (!animationFinished) {
    return;
  }

  // If we're showing the sequence and animations finished, either advance to
  // the next revealed glyph or return to waiting for input.
  if (currentState == SHOW_SEQUENCE) {
    if (++showIndex > revealedIndex) {
      // We've shown everything to reveal for now.
      moveProgramState(WAITING_FOR_PRESS);
    } else {
      // Trigger the next glyph to show.
      int glyphId = answerSequence[showIndex] - 65;
      setColor(glyphs[glyphId], signal_cyan, fastSpeed);
    }
    return;
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
Returns false if the glyph is in the midst if animating.
*/
bool checkGlyph(int glyphId) {
  // Fetch the Glyph instance.
  Glyph &glyph = glyphs[glyphId];
  GlyphState state = glyph.state;

  // If the glyph is still animating, that takes precedence.
  if (glyph.keyframe < 255) {
    bool finished = true;
    finished = finished && adjustColorComponent(glyph.currentColor.red, glyph.toColor.red, glyph.speed);
    finished = finished && adjustColorComponent(glyph.currentColor.green, glyph.toColor.green, glyph.speed);
    finished = finished && adjustColorComponent(glyph.currentColor.blue, glyph.toColor.blue, glyph.speed);

    if (finished || ((255 - glyph.keyframe) < glyph.speed)) {
      // Mark the keyframe as finished.
      glyph.keyframe = 255;
    } else {
      // If not finished, advance keyframe and return false.
      glyph.keyframe += glyph.speed;
      return false;
    }

    // Check end of animation for follow up animation and state changes.
    switch (glyph.state) {
      case IDLE_FLICKER_ON:
        setColor(glyph, dark, fastSpeed);
        glyph.state = IDLE_FLICKER_OFF;
        return false;
      case SIGNAL_FLASH:
        setColor(glyph, dark, fastSpeed);
        glyph.state = RESET_TO_DARK;
        return false;
      case CORRECT_PUSH:
      case INCORRECT_PUSH:
        setColor(glyph, dark, slowSpeed);
        glyph.state = RESET_TO_DARK;
        return false;
      default:
        glyph.state = IDLE_DARK;
        return true;
    }
  }

  // Check for glyph press.
  if ((currentState == WAITING_FOR_PRESS) && isGlyphPressed(glyphId)) {
    // Get the index for the current currect push.
    int letter = answerSequence[revealedIndex] - 65;

    if (letter == glyphId) {
      // Correct letter press!
      glyph.state = CORRECT_PUSH;
      setColor(glyph, good_green, fastSpeed);
      char nextCharacter = answerSequence[++pressIndex];
      if (nextCharacter == '\0') {
        // The players have entered the full sequence.
        moveProgramState(PUZZLE_SOLVED);
      } else if (pressIndex >= revealedIndex) {
        // The players have pressed everything in the revealed sequence.
        // Show the next part of the sequence.
        revealedIndex++;
        moveProgramState(SHOW_SEQUENCE);
      }
      return false;
    } else {
      // Incorrect letter press.
      glyph.state = INCORRECT_PUSH;
      setColor(glyph, wrong_red, fastSpeed);
      moveProgramState(SHOW_SEQUENCE);
      return false;
    }
  }

  // Nothing to do on this glyph.
  return true;
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
Clear the colors on all LEDs.
*/
void clearGlyphs() {
  for (int i = 0; i < GLYPH_COUNT; i++) {
    Glyph &glyph = glyphs[i];
    setColor(glyph, dark, fastSpeed);
  }
}

/*
Flash all the glyphs green, to signal puzzle solved.
*/
void flashSuccessful() {
  for (int i = 0; i < GLYPH_COUNT; i++) {
    Glyph &glyph = glyphs[i];
    setColor(glyph, good_green, fastSpeed);
    glyph.state = CORRECT_PUSH;
  }
}

/*
Queue up an animation on a glyph, to the destination color at the given speed.
*/
void setColor(Glyph &glyph, Color color, byte speed) {
  glyph.toColor.red = color.red;
  glyph.toColor.green = color.green;
  glyph.toColor.blue = color.blue;
  if (speed < 1) {
    glyph.speed = 1;
  } else {
    glyph.speed = speed;
  }
  glyph.keyframe = 0;
}

/*
Sync the color, i.e. push it via the NeoPixel library. All of the necessary
data is carried on the Glyph state object.
*/
void syncColor(int index, Glyph &glyph) {
  Color color = glyph.currentColor;
  leds.setPixelColor(index, color.red, color.blue, color.green);
}

/*
Adjust one of the color components based on speed value. Returns true if the
value to end at has been or will be reached.
*/
bool adjustColorComponent(byte &currentValue, byte toValue, byte speed) {
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
