#include <Arduino.h>

#include <exception>

#include <epd7in5_V2.h>
#include "monochrome_canvas.h"

// The display's native C++ API expects 1 bit per pixel: 0 = black, 1 = white.
static unsigned char frame_buffer[EPD_FRAME_BYTES];
static Epd epd;

static void DrawHardwarePlaceholder(MonochromeCanvas& canvas) {
    canvas.Clear();
    canvas.DrawText(30, 30, "TRANSIT", 2);
    canvas.DrawText(30, 62, "LIVE MAP", 2);
    canvas.DrawLine(30, 94, 770, 94);

    // Explain the current firmware state independently of the desktop demo.
    canvas.DrawText(30, 118, "THE DESKTOP BUILD FETCHES LIVE KVV DATA", 1);
    canvas.DrawText(30, 138, "ESP32 WIFI FETCHING IS THE NEXT HARDWARE STEP", 1);

    canvas.DrawText(30, 445, "WAVESHARE EPD API: READY", 1);
}

static void InitialiseAndDraw() {
    Serial.begin(115200);

    if (epd.Init() != 0) {
        Serial.println("Failed to initialise the E-Paper driver");
        while (true) {
            delay(1000);
        }
    }

    MonochromeCanvas canvas(frame_buffer);
    DrawHardwarePlaceholder(canvas);

    // The panel retains this frame after power-down.
    epd.DisplayFrame(frame_buffer);
    epd.Sleep();
}

void setup() {
    InitialiseAndDraw();
}

void loop() {
    // Real E-Ink hardware holds this last frame after Sleep().
    delay(1000);
}
