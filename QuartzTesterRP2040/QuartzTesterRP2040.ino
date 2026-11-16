#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_ST7735.h>
#include <hardware/irq.h>
#include <hardware/pwm.h>
#include <hardware/sync.h>
#include <math.h>

constexpr char projectVersion[] = "v1.0.0";

constexpr uint8_t sckPin = 11;
constexpr uint8_t mosiPin = 12;
constexpr uint8_t csPin = 10;
constexpr uint8_t dcPin = 9;
constexpr uint8_t resetPin = 15;
constexpr uint8_t rgbPin = 16;
constexpr uint8_t buttonPin = 13;
constexpr uint32_t debounceMs = 10;
constexpr uint32_t holdMs = 650;
constexpr uint8_t counterPin = 7;
constexpr uint32_t windowUs = 250000;
constexpr uint32_t startTimeoutUs = 3000000;
constexpr uint32_t minSignalHz = 1000;
constexpr uint8_t frequencyWindows = 4;
constexpr uint8_t stabilityWindows = 12;
constexpr uint8_t graphWindows = 32;
constexpr uint8_t graphHistory = graphWindows + 1;
constexpr uint8_t graphWidth = 108;
constexpr uint8_t graphHeight = 36;
constexpr uint8_t graphStride = (graphWidth + 7) / 8;
constexpr uint32_t graphFrameMs = 35;
constexpr int16_t graphX = 10;
constexpr int16_t graphY = 47;
constexpr uint32_t stabilityPpm = 100;
constexpr uint32_t minSpreadLimitHz = 20;

Adafruit_ST7735 tft(csPin, dcPin, mosiPin, sckPin, resetPin);
Adafruit_NeoPixel rgb(1, rgbPin, NEO_GRB + NEO_KHZ800);

constexpr uint16_t background = 0x0863;
constexpr uint16_t panel = 0x1127;
constexpr uint16_t gridColor = 0x19AC;
constexpr uint16_t white = 0xEFFF;
constexpr uint16_t cyan = 0x4FDF;
constexpr uint16_t amber = 0xFE09;
constexpr uint16_t green = 0x6F8C;

enum class Status : uint8_t { Starting, Checking, Stable, Unstable, NoStart };
enum class Page : uint8_t { Frequency, Stability, Graph };

uint counterSlice;
volatile uint32_t wraps = 0;
uint32_t previousTime;
uint32_t previousCount;
uint32_t testStarted;
uint32_t firstSignalUs;
uint32_t dropoutCount;
bool signalPending;
bool signalSeen;
Page page = Page::Frequency;
Status status = Status::Starting;

struct Window {
  uint32_t edges;
  uint32_t elapsed;
  uint32_t hz;
};

Window windows[stabilityWindows];
uint8_t nextWindow;
uint8_t windowCount;
uint32_t graphSamples[graphHistory];
uint32_t graphFilteredHz;
uint32_t graphLatestHz;
uint8_t graphNext;
uint8_t graphCount;

void onCounterWrap() {
  pwm_clear_irq(counterSlice);
  ++wraps;
}

uint32_t readCounter(uint32_t &sampleTime) {
  const uint32_t irqState = save_and_disable_interrupts();
  uint32_t upper = wraps;
  uint16_t lower = pwm_get_counter(counterSlice);
  // If the wrap interrupt is pending, fold it into this sample only.
  if (pwm_get_irq_status_mask() & (1u << counterSlice)) {
    ++upper;
    lower = pwm_get_counter(counterSlice);
  }
  sampleTime = micros();
  restore_interrupts(irqState);
  return (upper << 16) | lower;
}

void startCounter() {
  counterSlice = pwm_gpio_to_slice_num(counterPin);
  gpio_disable_pulls(counterPin);
  gpio_set_function(counterPin, GPIO_FUNC_PWM);

  pwm_config config = pwm_get_default_config();
  pwm_config_set_clkdiv_mode(&config, PWM_DIV_B_RISING);
  pwm_init(counterSlice, &config, false);
  pwm_clear_irq(counterSlice);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, onCounterWrap);
  pwm_set_irq_enabled(counterSlice, true);
  irq_set_enabled(PWM_IRQ_WRAP, true);
  pwm_set_enabled(counterSlice, true);

}

void updateRgb() {
  uint32_t color;
  switch (status) {
    case Status::Stable: color = rgb.Color(0, 16, 0); break;
    case Status::Unstable: color = rgb.Color(24, 9, 0); break;
    case Status::NoStart: color = rgb.Color(20, 0, 0); break;
    default: color = rgb.Color(0, 4, 14); break;
  }
  rgb.setPixelColor(0, color);
  rgb.show();
}

void drawCenteredLine(uint8_t row, int16_t y, const char *text, uint8_t size,
                      uint8_t columns, uint16_t color, bool redraw = false,
                      uint16_t fill = panel) {
  static char previous[5][20] = {};
  static uint16_t previousColor[5] = {};
  if (!redraw && previousColor[row] == color && strcmp(previous[row], text) == 0) {
    return;
  }
  snprintf(previous[row], sizeof(previous[row]), "%s", text);
  previousColor[row] = color;
  const int length = min(static_cast<int>(strlen(text)), static_cast<int>(columns));
  const int leftPadding = (columns - length) / 2;
  tft.setTextSize(size);
  tft.setTextColor(color, fill);
  tft.setCursor((128 - columns * 6 * size) / 2, y);
  for (int i = 0; i < columns; ++i) {
    const int index = i - leftPadding;
    tft.write(index >= 0 && index < length ? text[index] : ' ');
  }
}

const char *formatFrequency(uint32_t hz, char (&digits)[20]) {
  const bool mhz = hz >= 999950;
  const bool khz = hz >= 1000 && !mhz;
  const uint32_t scale = mhz ? 10000 : khz ? 1000 : 1;
  const uint32_t value = mhz ? hz / 100 + (hz % 100 >= 50) : hz;
  const char *unit = mhz ? "MHz" : khz ? "kHz" : "Hz";
  int len = snprintf(digits, sizeof(digits), "%lu",
                     static_cast<unsigned long>(value / scale));
  uint32_t fraction = value % scale;
  if (fraction) {
    digits[len++] = ',';
    for (uint32_t place = scale / 10; place && fraction; place /= 10) {
      digits[len++] = '0' + fraction / place;
      fraction %= place;
    }
    digits[len] = '\0';
  }

  return unit;
}

const char *statusLabel() {
  switch (status) {
    case Status::Starting: return "STARTING...";
    case Status::Checking: return "CHECKING...";
    case Status::Stable: return "STABLE";
    case Status::Unstable: return "UNSTABLE";
    case Status::NoStart: return "NO START";
  }
  return "";
}

uint32_t toHz(uint32_t edges, uint32_t elapsed) {
  return (static_cast<uint64_t>(edges) * 1000000ULL + elapsed / 2) / elapsed;
}

uint32_t frequency() {
  uint32_t edges = 0;
  uint32_t elapsed = 0;
  const uint8_t count = min(windowCount, frequencyWindows);
  for (uint8_t i = 0; i < count; ++i) {
    const Window &sample = windows[(nextWindow + stabilityWindows - 1 - i) % stabilityWindows];
    edges += sample.edges;
    elapsed += sample.elapsed;
  }
  return elapsed ? toHz(edges, elapsed) : 0;
}

uint32_t spreadHz() {
  uint32_t low = UINT32_MAX;
  uint32_t high = 0;
  for (uint8_t i = 0; i < windowCount; ++i) {
    low = min(low, windows[i].hz);
    high = max(high, windows[i].hz);
  }
  return windowCount ? high - low : 0;
}

uint32_t jitterTenthsPpm() {
  // Work with offsets to preserve small variations at MHz frequencies.
  const int32_t reference = windows[0].hz;
  double sum = 0;
  double squares = 0;
  for (const Window &sample : windows) {
    const double offset = static_cast<int32_t>(sample.hz) - reference;
    sum += offset;
    squares += offset * offset;
  }
  const double meanOffset = sum / stabilityWindows;
  const double variance = squares / stabilityWindows - meanOffset * meanOffset;
  const double rmsHz = sqrt(max(0.0, variance));
  return static_cast<uint32_t>(rmsHz * 10000000.0 / (reference + meanOffset) + 0.5);
}

void paintGraph(GFXcanvas1 &trace, bool redraw) {
  static uint8_t previous[graphHeight * graphStride];
  const uint8_t *pixels = trace.getBuffer();
  if (redraw) {
    tft.fillRect(graphX, graphY, graphWidth, graphHeight, background);
    tft.drawRect(graphX, graphY, graphWidth, graphHeight, gridColor);
    tft.drawFastHLine(graphX + 1, graphY + graphHeight / 2, graphWidth - 2, gridColor);
    memset(previous, 0, sizeof(previous));
  }
  if (!pixels || memcmp(pixels, previous, sizeof(previous)) == 0) {
    return;
  }

  // Update only changed pixels, keeping the trace visible during each refresh.
  tft.startWrite();
  for (uint8_t y = 0; y < graphHeight; ++y) {
    for (uint8_t byte = 0; byte < graphStride; ++byte) {
      const uint16_t index = y * graphStride + byte;
      const uint8_t changed = pixels[index] ^ previous[index];
      if (!changed) {
        continue;
      }
      for (uint8_t bit = 0; bit < 8; ++bit) {
        const uint8_t mask = 0x80 >> bit;
        const uint8_t x = byte * 8 + bit;
        if ((changed & mask) && x > 0 && x < graphWidth - 1) {
          const uint16_t color = pixels[index] & mask ? cyan :
                                 y == graphHeight / 2 ? gridColor : background;
          tft.writePixel(graphX + x, graphY + y, color);
        }
      }
      previous[index] = pixels[index];
    }
  }
  tft.endWrite();
}

void drawGraph(bool redraw) {
  const uint8_t first = (graphNext + graphHistory - graphCount) % graphHistory;
  const uint8_t visibleStart = graphCount > graphWindows ? 1 : 0;
  uint64_t sum = 0;
  uint8_t valid = 0;
  for (uint8_t i = visibleStart; i < graphCount; ++i) {
    const uint32_t hz = graphSamples[(first + i) % graphHistory];
    if (hz) {
      sum += hz;
      ++valid;
    }
  }
  const int32_t mean = valid ? (sum + valid / 2) / valid : 0;
  const int16_t middle = graphHeight / 2;
  const int16_t amplitude = middle - 2;
  int32_t neededScale = minSpreadLimitHz;
  for (uint8_t i = visibleStart; i < graphCount; ++i) {
    const uint32_t hz = graphSamples[(first + i) % graphHistory];
    if (hz) {
      neededScale = max(neededScale, abs(static_cast<int32_t>(hz) - mean));
    }
  }
  static int32_t scale = minSpreadLimitHz;
  static uint32_t lastScaleChangeMs;
  const uint32_t now = millis();
  if (redraw || !graphCount || neededScale > scale ||
      (neededScale <= scale / 2 && now - lastScaleChangeMs >= 2000)) {
    scale = minSpreadLimitHz;
    while (scale < neededScale) {
      scale *= 2;
    }
    lastScaleChangeMs = now;
  }

  static GFXcanvas1 trace(graphWidth, graphHeight);
  trace.fillScreen(0);
  const uint32_t phaseUs = min(micros() - previousTime, windowUs);
  const int32_t scroll = (graphWidth - 3) * phaseUs / windowUs;
  int16_t previousX = -1;
  int16_t previousY = 0;
  bool previousValid = false;
  for (uint8_t i = 0; i < graphCount; ++i) {
    const uint32_t hz = graphSamples[(first + i) % graphHistory];
    if (!hz) {
      previousValid = false;
      continue;
    }
    const int32_t position = (graphWindows - graphCount + i) * (graphWidth - 3) - scroll;
    // Round towards negative infinity so the first segment scrolls off the left edge.
    const int16_t x = 1 + (position >= 0 ? position : position - (graphWindows - 2)) /
                              (graphWindows - 1);
    const int32_t deviation = static_cast<int32_t>(hz) - mean;
    const int16_t y = middle - constrain(deviation, -scale, scale) * amplitude / scale;
    if (previousValid) {
      trace.drawLine(previousX, previousY, x, y, 1);
    } else {
      trace.drawPixel(x, y, 1);
    }
    previousX = x;
    previousY = y;
    previousValid = true;
  }
  if (previousValid) {
    trace.drawFastHLine(previousX, previousY, graphWidth - 1 - previousX, 1);
  }
  paintGraph(trace, redraw);

  char line[20];
  if (graphLatestHz) {
    char digits[20];
    const char *unit = formatFrequency(graphLatestHz, digits);
    snprintf(line, sizeof(line), "%.13s %s", digits, unit);
    drawCenteredLine(3, 37, line, 1, 18, cyan, redraw);
    snprintf(line, sizeof(line), "Dev %+ld Hz", static_cast<long>(graphFilteredHz) - mean);
  } else {
    drawCenteredLine(3, 37, "--", 1, 18, cyan, redraw);
    snprintf(line, sizeof(line), "Dev --");
  }
  drawCenteredLine(1, 86, line, 1, 18, white, redraw);
  snprintf(line, sizeof(line), "+/-%ld Hz/8s", static_cast<long>(scale));
  drawCenteredLine(2, 97, line, 1, 18, white, redraw);
}

void drawPage(bool redraw = false) {
  const bool graphPage = page == Page::Graph;
  if (redraw) {
    tft.fillScreen(background);
    tft.setTextSize(1);
    tft.setTextColor(white);
    const char *title = page == Page::Frequency ? "QUARTZ TESTER" :
                        page == Page::Stability ? "STABILITY" : "FREQ GRAPH";
    tft.setCursor((128 - strlen(title) * 6) / 2, graphPage ? 10 : 17);
    tft.print(title);
    if (page == Page::Frequency) {
      tft.setCursor((128 - (sizeof(projectVersion) - 1) * 6) / 2, 5);
      tft.print(projectVersion);
    }
    const uint8_t panelHeight = graphPage ? 77 : 66;
    tft.fillRoundRect(5, 33, 118, panelHeight, 5, panel);
    tft.drawRoundRect(5, 33, 118, panelHeight, 5, gridColor);
    tft.setCursor(7, graphPage ? 116 : 111);
    tft.print(page == Page::Frequency ? "Tap:stats Hold:test" :
              page == Page::Stability ? "Tap:graph Hold:test" : "Tap:freq Hold:test");
  }

  const uint16_t ink = status == Status::Stable ? green :
      status == Status::Unstable || status == Status::NoStart ? amber : cyan;
  drawCenteredLine(0, graphPage ? 21 : 87, statusLabel(), 1, 18, ink, redraw,
                   graphPage ? background : panel);
  if (graphPage) {
    drawGraph(redraw);
    return;
  }
  char line[20];
  const uint32_t hz = frequency();
  if (page == Page::Frequency) {
    const char *unit = formatFrequency(hz, line);
    drawCenteredLine(1, 46, hz ? line : "--", 2, 9, cyan, redraw);
    drawCenteredLine(2, 69, hz ? unit : "", 1, 18, white, redraw);
    return;
  }

  if (signalSeen) {
    snprintf(line, sizeof(line), "Start ~%lu ms", static_cast<unsigned long>(firstSignalUs / 1000));
  } else {
    snprintf(line, sizeof(line), "Start --");
  }
  drawCenteredLine(1, 39, line, 1, 18, white, redraw);
  if (windowCount >= 2) {
    snprintf(line, sizeof(line), "Span %lu Hz", static_cast<unsigned long>(spreadHz()));
  } else {
    snprintf(line, sizeof(line), "Span --");
  }
  drawCenteredLine(2, 50, line, 1, 18, white, redraw);
  if (windowCount == stabilityWindows) {
    const uint32_t jitter = jitterTenthsPpm();
    snprintf(line, sizeof(line), "Jitter %lu.%lu ppm",
             static_cast<unsigned long>(jitter / 10),
             static_cast<unsigned long>(jitter % 10));
  } else {
    snprintf(line, sizeof(line), "Jitter --");
  }
  drawCenteredLine(3, 61, line, 1, 19, white, redraw);
  snprintf(line, sizeof(line), "Drops %lu", static_cast<unsigned long>(dropoutCount));
  drawCenteredLine(4, 72, line, 1, 18, white, redraw);
}

void resetTest() {
  previousCount = readCounter(previousTime);
  testStarted = previousTime;
  firstSignalUs = 0;
  dropoutCount = 0;
  signalPending = false;
  signalSeen = false;
  windowCount = 0;
  nextWindow = 0;
  graphCount = 0;
  graphNext = 0;
  graphFilteredHz = 0;
  graphLatestHz = 0;
  status = Status::Starting;
  updateRgb();
  drawPage();
}

void recordWindow(uint32_t edges, uint32_t elapsed, uint32_t now) {
  const uint32_t hz = toHz(edges, elapsed);
  // Filter each sample once so scrolling never changes the oldest plotted points.
  graphLatestHz = hz >= minSignalHz && signalPending ? hz : 0;
  if (!graphLatestHz || !graphFilteredHz) {
    graphFilteredHz = graphLatestHz;
  } else {
    graphFilteredHz = (graphFilteredHz * 3 + graphLatestHz + 2) / 4;
  }
  graphSamples[graphNext] = graphFilteredHz;
  graphNext = (graphNext + 1) % graphHistory;
  if (graphCount < graphHistory) {
    ++graphCount;
  }
  if (hz < minSignalHz) {
    if (windowCount) {
      ++dropoutCount;
    }
    windowCount = 0;
    nextWindow = 0;
    signalPending = false;
    if (signalSeen) {
      status = Status::Unstable;
    } else if (now - testStarted >= startTimeoutUs) {
      status = Status::NoStart;
    }
    return;
  }

  if (!signalPending) {
    // The first window can contain part silence and part signal.
    signalPending = true;
    if (!signalSeen) {
      firstSignalUs = now - testStarted;
    }
    return;
  }
  signalSeen = true;
  windows[nextWindow] = {edges, elapsed, hz};
  nextWindow = (nextWindow + 1) % stabilityWindows;
  if (windowCount < stabilityWindows) {
    ++windowCount;
  }
  const uint32_t limit = max(minSpreadLimitHz, frequency() / (1000000 / stabilityPpm));
  if (spreadHz() > limit) {
    status = Status::Unstable;
  } else if (windowCount == stabilityWindows) {
    status = Status::Stable;
  } else if (status != Status::Unstable) {
    status = Status::Checking;
  }
}

void updateFrequency() {
  if (micros() - previousTime < windowUs) {
    return;
  }
  uint32_t now;
  const uint32_t count = readCounter(now);
  const Status oldStatus = status;
  recordWindow(count - previousCount, now - previousTime, now);
  previousTime = now;
  previousCount = count;
  drawPage();
  if (status != oldStatus) {
    updateRgb();
  }

  static uint32_t lastReport;
  if ((now - lastReport >= 1000000 || status != oldStatus) && Serial) {
    lastReport = now;
    Serial.print(F("GP7: "));
    Serial.print(frequency());
    Serial.print(F(" Hz | "));
    Serial.print(statusLabel());
    Serial.print(F(" | span "));
    Serial.print(spreadHz());
    Serial.print(F(" Hz | drops "));
    Serial.println(dropoutCount);
  }
}

void pollButton() {
  static bool raw;
  static bool pressed;
  static bool longHandled;
  static uint32_t changedAt;
  static uint32_t pressedAt;
  const uint32_t now = millis();
  const bool reading = digitalRead(buttonPin) == LOW;
  if (reading != raw) {
    raw = reading;
    changedAt = now;
  }
  if (raw != pressed && now - changedAt >= debounceMs) {
    pressed = raw;
    if (pressed) {
      pressedAt = changedAt;
      longHandled = false;
    } else if (!longHandled) {
      if (changedAt - pressedAt >= holdMs) {
        resetTest();
      } else {
        page = page == Page::Frequency ? Page::Stability :
               page == Page::Stability ? Page::Graph : Page::Frequency;
        drawPage(true);
      }
    }
  }
  if (pressed && raw && !longHandled && now - pressedAt >= holdMs) {
    longHandled = true;
    resetTest();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(buttonPin, INPUT_PULLUP);
  rgb.begin();

  tft.initR(INITR_144GREENTAB);
  tft.setRotation(2);
  tft.setTextWrap(false);
  startCounter();
  drawPage(true);
  resetTest();
}

void loop() {
  pollButton();
  updateFrequency();
  static uint32_t lastGraphFrameMs;
  const uint32_t now = millis();
  if (page == Page::Graph && now - lastGraphFrameMs >= graphFrameMs) {
    lastGraphFrameMs = now;
    drawGraph(false);
  }
}

