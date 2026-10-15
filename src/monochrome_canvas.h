#pragma once

#include <string>

#include <epd7in5_V2.h>

// Small, dependency-free drawing helper for the display's native 1-bit buffer.
// It deliberately uses the same convention as Waveshare's examples: a zero bit
// is black and a one bit is white.
class MonochromeCanvas {
public:
    explicit MonochromeCanvas(unsigned char* frame_buffer);

    void Clear(bool black = false);

    // Clip every drawing primitive, including thick strokes at the map edge.
    void SetClipCircle(int center_x, int center_y, int radius);
    void SetClipRect(int x, int y, int width, int height);
    void ResetClip();

    // All drawing operations eventually use SetPixel and share its clipping.
    void SetPixel(int x, int y, bool black = true);
    void DrawLine(int x0, int y0, int x1, int y1, bool black = true);
    void DrawThickLine(int x0, int y0, int x1, int y1, int thickness, bool black = true);
    void DrawRect(int x, int y, int width, int height, bool black = true);
    void FillRect(int x, int y, int width, int height, bool black = true);
    void FillCircle(int center_x, int center_y, int radius, bool black = true);

    // Text uses the small built-in font at an integer pixel scale.
    void DrawText(int x, int y, const std::string& text, int scale = 1, bool black = true);

    static int TextWidth(const std::string& text, int scale = 1);

private:
    unsigned char* frame_buffer_;
    bool circle_clip_ = false;
    int clip_x_ = 0;
    int clip_y_ = 0;
    int clip_radius_ = 0;
    int clip_left_ = 0;
    int clip_top_ = 0;
    int clip_right_ = EPD_WIDTH;
    int clip_bottom_ = EPD_HEIGHT;
};
