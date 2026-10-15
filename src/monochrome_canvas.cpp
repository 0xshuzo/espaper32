#include "monochrome_canvas.h"

#include <cctype>
#include <cstring>

namespace {

// Five-pixel-wide glyphs, one byte per row. Keeping this here avoids a desktop
// graphics dependency and makes the exact same drawing code usable on ESP32.
constexpr unsigned char kBlank[7] = {0, 0, 0, 0, 0, 0, 0};
constexpr unsigned char kA[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
constexpr unsigned char kB[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
constexpr unsigned char kC[7] = {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
constexpr unsigned char kD[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
constexpr unsigned char kE[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
constexpr unsigned char kF[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
constexpr unsigned char kG[7] = {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F};
constexpr unsigned char kH[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
constexpr unsigned char kI[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
constexpr unsigned char kJ[7] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
constexpr unsigned char kK[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
constexpr unsigned char kL[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
constexpr unsigned char kM[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
constexpr unsigned char kN[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
constexpr unsigned char kO[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
constexpr unsigned char kP[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
constexpr unsigned char kQ[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
constexpr unsigned char kR[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
constexpr unsigned char kS[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
constexpr unsigned char kT[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
constexpr unsigned char kU[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
constexpr unsigned char kV[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
constexpr unsigned char kW[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
constexpr unsigned char kX[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
constexpr unsigned char kY[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
constexpr unsigned char kZ[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
// Numeric glyphs use the same five-by-seven cell as letters.
constexpr unsigned char k0[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
constexpr unsigned char k1[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
constexpr unsigned char k2[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
constexpr unsigned char k3[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
constexpr unsigned char k4[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
constexpr unsigned char k5[7] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
constexpr unsigned char k6[7] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
constexpr unsigned char k7[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
constexpr unsigned char k8[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
constexpr unsigned char k9[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
// Punctuation needed by status labels, times and route numbers.
constexpr unsigned char kDash[7] = {0, 0, 0, 0x1F, 0, 0, 0};
constexpr unsigned char kDot[7] = {0, 0, 0, 0, 0, 0x06, 0x06};
constexpr unsigned char kColon[7] = {0, 0x06, 0x06, 0, 0x06, 0x06, 0};
constexpr unsigned char kSlash[7] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
constexpr unsigned char kPlus[7] = {0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0};

// Unsupported characters render as an empty cell.
const unsigned char* Glyph(char character) {
    const unsigned char upper =
        static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(character)));
    switch (upper) {
    case 'A':
        return kA;
    case 'B':
        return kB;
    case 'C':
        return kC;
    case 'D':
        return kD;
    case 'E':
        return kE;
    case 'F':
        return kF;
    case 'G':
        return kG;
    case 'H':
        return kH;
    case 'I':
        return kI;
    case 'J':
        return kJ;
    case 'K':
        return kK;
    case 'L':
        return kL;
    case 'M':
        return kM;
    case 'N':
        return kN;
    case 'O':
        return kO;
    case 'P':
        return kP;
    case 'Q':
        return kQ;
    case 'R':
        return kR;
    case 'S':
        return kS;
    case 'T':
        return kT;
    case 'U':
        return kU;
    case 'V':
        return kV;
    case 'W':
        return kW;
    case 'X':
        return kX;
    case 'Y':
        return kY;
    case 'Z':
        return kZ;
    case '0':
        return k0;
    case '1':
        return k1;
    case '2':
        return k2;
    case '3':
        return k3;
    case '4':
        return k4;
    case '5':
        return k5;
    case '6':
        return k6;
    case '7':
        return k7;
    case '8':
        return k8;
    case '9':
        return k9;
    case '-':
        return kDash;
    case '.':
        return kDot;
    case ':':
        return kColon;
    case '/':
        return kSlash;
    case '+':
        return kPlus;
    default:
        return kBlank;
    }
}

} // namespace

MonochromeCanvas::MonochromeCanvas(unsigned char* frame_buffer) : frame_buffer_(frame_buffer) {
}

void MonochromeCanvas::Clear(bool black) {
    std::memset(frame_buffer_, black ? 0x00 : 0xFF, EPD_FRAME_BYTES);
}

void MonochromeCanvas::SetClipCircle(int center_x, int center_y, int radius) {
    circle_clip_ = true;
    clip_x_ = center_x;
    clip_y_ = center_y;
    clip_radius_ = radius > 0 ? radius : 0;
}

void MonochromeCanvas::SetClipRect(int x, int y, int width, int height) {
    clip_left_ = x;
    clip_top_ = y;
    clip_right_ = x + (width > 0 ? width : 0);
    clip_bottom_ = y + (height > 0 ? height : 0);
}

void MonochromeCanvas::ResetClip() {
    circle_clip_ = false;
    clip_left_ = 0;
    clip_top_ = 0;
    clip_right_ = EPD_WIDTH;
    clip_bottom_ = EPD_HEIGHT;
}

void MonochromeCanvas::SetPixel(int x, int y, bool black) {
    if (x < 0 || y < 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) {
        return;
    }
    if (x < clip_left_ || x >= clip_right_ || y < clip_top_ || y >= clip_bottom_) {
        return;
    }

    // Rectangle and circle clipping combine to protect the map boundary.
    if (circle_clip_) {
        const long long dx = x - clip_x_;
        const long long dy = y - clip_y_;
        if (dx * dx + dy * dy > 1LL * clip_radius_ * clip_radius_) {
            return;
        }
    }

    // The leftmost pixel is the high bit; clear that bit to draw black.
    unsigned char& cell = frame_buffer_[x / 8 + y * (EPD_WIDTH / 8)];
    const unsigned char mask = static_cast<unsigned char>(0x80U >> (x % 8));
    if (black) {
        cell &= static_cast<unsigned char>(~mask);
    } else {
        cell |= mask;
    }
}

void MonochromeCanvas::DrawLine(int x0, int y0, int x1, int y1, bool black) {
    // Bresenham stepping keeps the raster path integral on both targets.
    const int delta_x = x0 < x1 ? x1 - x0 : x0 - x1;
    const int step_x = x0 < x1 ? 1 : -1;
    const int delta_y = y0 < y1 ? y0 - y1 : y1 - y0;
    const int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x + delta_y;

    while (true) {
        SetPixel(x0, y0, black);
        if (x0 == x1 && y0 == y1) {
            return;
        }

        const int doubled_error = 2 * error;
        if (doubled_error >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (doubled_error <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

void MonochromeCanvas::DrawThickLine(int x0, int y0, int x1, int y1, int thickness, bool black) {
    if (thickness <= 1) {
        DrawLine(x0, y0, x1, y1, black);
        return;
    }

    const int delta_x = x0 < x1 ? x1 - x0 : x0 - x1;
    const int step_x = x0 < x1 ? 1 : -1;
    const int delta_y = y0 < y1 ? y0 - y1 : y1 - y0;
    const int step_y = y0 < y1 ? 1 : -1;
    // Stamp a square brush along the same path used by a one-pixel line.
    const int half_thickness = thickness / 2;
    int error = delta_x + delta_y;

    while (true) {
        FillRect(x0 - half_thickness, y0 - half_thickness, thickness, thickness, black);
        if (x0 == x1 && y0 == y1) {
            return;
        }

        const int doubled_error = 2 * error;
        if (doubled_error >= delta_y) {
            error += delta_y;
            x0 += step_x;
        }
        if (doubled_error <= delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

void MonochromeCanvas::DrawRect(int x, int y, int width, int height, bool black) {
    if (width <= 0 || height <= 0) {
        return;
    }

    DrawLine(x, y, x + width - 1, y, black);
    DrawLine(x, y, x, y + height - 1, black);
    DrawLine(x + width - 1, y, x + width - 1, y + height - 1, black);
    DrawLine(x, y + height - 1, x + width - 1, y + height - 1, black);
}

void MonochromeCanvas::FillRect(int x, int y, int width, int height, bool black) {
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            SetPixel(x + column, y + row, black);
        }
    }
}

void MonochromeCanvas::FillCircle(int center_x, int center_y, int radius, bool black) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                SetPixel(center_x + x, center_y + y, black);
            }
        }
    }
}

void MonochromeCanvas::DrawText(int x, int y, const std::string& text, int scale, bool black) {
    if (scale <= 0) {
        return;
    }

    // Scale each glyph pixel to a square while retaining one-column spacing.
    int cursor_x = x;
    for (const char character : text) {
        const unsigned char* glyph = Glyph(character);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((glyph[row] & (1U << (4 - column))) == 0) {
                    continue;
                }
                FillRect(cursor_x + column * scale, y + row * scale, scale, scale, black);
            }
        }

        cursor_x += 6 * scale;
    }
}

int MonochromeCanvas::TextWidth(const std::string& text, int scale) {
    return static_cast<int>(text.size()) * 6 * scale;
}
