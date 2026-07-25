#pragma once
// A hand-built seven-segment LED clock rendered with LVGL rectangles, so the
// digit size and the lit/unlit LED look are fully under our control (no built-in
// font tops out large enough, and real LED clocks hide their unlit segments).

#include <lvgl.h>
#include "HuckTheme.h"

class SevenSegClock {
public:
  // Digit + layout geometry (landscape 320x240).
  static constexpr int DW = 52;   // digit width
  static constexpr int DH = 104;  // digit height
  static constexpr int T  = 11;   // segment thickness
  static constexpr int G  = 6;    // gap between digits
  static constexpr int CW = 22;   // colon column width

  // The hour-tens digit is only ever blank or "1" (12h clock), so it gets a
  // slim cell. The whole clock is shifted left to leave room for AM/PM on the
  // right of the minutes.
  static constexpr int W1 = 18;   // narrow width for the "1" digit

  void create(lv_obj_t* parent, int top_y) {
    _parent = parent;
    const int startX = 20;
    _digitW[0] = W1; _digitW[1] = DW; _digitW[2] = DW; _digitW[3] = DW;
    int x = startX;
    _digitX[0] = x;             x += _digitW[0] + G;
    _digitX[1] = x;             x += _digitW[1] + G;
    _colonX    = x;             x += CW + G;
    _digitX[2] = x;             x += _digitW[2] + G;
    _digitX[3] = x;
    _topY = top_y;

    for (int d = 0; d < 4; d++)
      buildDigit(d, _digitX[d], top_y, _digitW[d]);
    buildColon(_colonX, top_y);
  }

  int rightEdgeX() const { return _digitX[3] + _digitW[3]; }
  int centerY()    const { return _topY + DH / 2; }

  void applyTheme(const HuckTheme& t) {
    _segOn = t.seg_on;
    _segOff = t.seg_off;
    _segOffOpa = t.seg_off_opa;
    // Re-render whatever is currently shown with the new colors.
    for (int d = 0; d < 4; d++) paintDigit(d, _val[d], _blank[d]);
    setColon(_colonOn);
  }

  // hour is already in the desired 12/24h form; blankTens hides a leading zero.
  void setTime(int hour, int minute, bool blankTens) {
    paintDigit(0, hour / 10, blankTens && (hour / 10) == 0);
    paintDigit(1, hour % 10, false);
    paintDigit(2, minute / 10, false);
    paintDigit(3, minute % 10, false);
  }

  void setColon(bool on) {
    _colonOn = on;
    for (int i = 0; i < 2; i++) {
      lv_obj_set_style_bg_color(_colon[i], on ? _segOn : _segOff, 0);
      lv_obj_set_style_bg_opa(_colon[i], on ? LV_OPA_COVER : _segOffOpa, 0);
    }
  }

  int bottomY() const { return _topY + DH; }

private:
  lv_obj_t* _parent = nullptr;
  lv_obj_t* _seg[4][7];
  lv_obj_t* _colon[2];
  int _digitX[4], _digitW[4] = {DW, DW, DW, DW}, _colonX = 0, _topY = 0;
  int _val[4] = {8, 8, 8, 8};
  bool _blank[4] = {false, false, false, false};
  bool _colonOn = true;
  lv_color_t _segOn = lv_color_hex(0xE0611A);
  lv_color_t _segOff = lv_color_hex(0x140a03);
  lv_opa_t _segOffOpa = LV_OPA_0;

  lv_obj_t* mkSeg(int x, int y, int w, int h) {
    lv_obj_t* s = lv_obj_create(_parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, h);
    lv_obj_set_style_radius(s, T / 2, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return s;
  }

  void buildDigit(int d, int ox, int oy, int w) {
    int hw = w - (T * 6 / 5);          // horizontal segment length
    if (hw < 1) hw = 1;                // slim "1" cell: horiz segs vestigial (always off)
    const int hx = ox + (T * 3 / 5);
    const int vh = DH / 2 - (T * 7 / 10);
    const int vy1 = oy + (T * 3 / 5);
    const int vy2 = oy + DH / 2 + (T / 10);
    // a, b, c, d, e, f, g
    _seg[d][0] = mkSeg(hx, oy, hw, T);                    // a  top
    _seg[d][1] = mkSeg(ox + w - T, vy1, T, vh);           // b  top-right
    _seg[d][2] = mkSeg(ox + w - T, vy2, T, vh);           // c  bottom-right
    _seg[d][3] = mkSeg(hx, oy + DH - T, hw, T);           // d  bottom
    _seg[d][4] = mkSeg(ox, vy2, T, vh);                   // e  bottom-left
    _seg[d][5] = mkSeg(ox, vy1, T, vh);                   // f  top-left
    _seg[d][6] = mkSeg(hx, oy + DH / 2 - T / 2, hw, T);   // g  middle
  }

  void buildColon(int ox, int oy) {
    int cx = ox + (CW - T) / 2;
    _colon[0] = mkSeg(cx, oy + DH / 3 - T / 2, T, T);
    _colon[1] = mkSeg(cx, oy + 2 * DH / 3 - T / 2, T, T);
    for (int i = 0; i < 2; i++) lv_obj_set_style_radius(_colon[i], T / 2, 0);
  }

  static const uint8_t* mask(int v) {
    static const uint8_t M[10][7] = {
      {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1},
      {0,1,1,0,0,1,1}, {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0},
      {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1},
    };
    return M[v];
  }

  void paintDigit(int d, int v, bool blank) {
    _val[d] = v; _blank[d] = blank;
    const uint8_t* m = (v >= 0 && v <= 9) ? mask(v) : mask(8);
    for (int s = 0; s < 7; s++) {
      bool on = !blank && m[s];
      lv_obj_set_style_bg_color(_seg[d][s], on ? _segOn : _segOff, 0);
      lv_obj_set_style_bg_opa(_seg[d][s], on ? LV_OPA_COVER : _segOffOpa, 0);
    }
  }
};
