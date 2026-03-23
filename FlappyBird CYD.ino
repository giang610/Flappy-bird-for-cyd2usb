/*
  Flappy Bird - CYD 2USB
  Board  : ESP32 (CYD = Cheap Yellow Display)
  Screen : ILI9341 320x240 (landscape)
  Touch  : XPT2046 SPI
  Lib    : TFT_eSPI  +  XPT2046_Touchscreen
*/

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>

// ─── Touch pins (CYD 2USB) ───────────────────────────────────────────────────
#define TOUCH_CS   33
#define TOUCH_IRQ  36

// ─── Display ─────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);   // full-screen sprite (double-buffer)

// ─── Touch ───────────────────────────────────────────────────────────────────
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// ─── Screen ──────────────────────────────────────────────────────────────────
#define W  320
#define H  240

// ─── Colors (RGB565) ─────────────────────────────────────────────────────────
#define C_SKY        0x2739   // #4EC0CA
#define C_GROUND     0xDF51   // #DED895
#define C_GROUND_TOP 0x75C5   // #74BF2E
#define C_PIPE       0x75C5   // #73BF2E
#define C_PIPE_DARK  0x2C45   // #558B2F
#define C_PIPE_CAP   0x75C5
#define C_PIPE_CAPDK 0x23C4   // #4A7A25
#define C_BIRD_YEL   0xFD00   // #F5D60A  (approx)
#define C_BIRD_ORG   0xF3C1   // #F07B0F
#define C_WHITE      0xFFFF
#define C_BLACK      0x0000
#define C_GOLD       0xFEA0   // #FFD700
#define C_RED        0xF800

// ─── Game constants ──────────────────────────────────────────────────────────
#define GROUND_H      30
#define BIRD_X        60
#define BIRD_R        7      // collision radius
#define PIPE_W        28
#define PIPE_GAP      65
#define PIPE_CAP_H    10
#define PIPE_CAP_W    (PIPE_W + 6)
#define PIPE_SPEED_F  200    // pixels per second  (2 px @ 100fps)
#define PIPE_INTERVAL 1500   // ms between pipes  (~90 frames @ ~60fps)
#define GRAVITY_F     35     // units/s²  (0.35 px/frame² @ 100fps)
#define JUMP_VEL_F   -367    // units/s   (-3.67 px/frame @ 100fps)
                             // (all velocities * 100 to avoid float)

// ─── Game state ──────────────────────────────────────────────────────────────
enum GameState { IDLE, PLAYING, DEAD };
GameState state = IDLE;

struct Bird {
  int   y;        // * 100
  int   vy;       // * 100
  int   flapTick;
};

struct Pipe {
  int  x;        // pixel
  int  gapY;     // pixel  (top of gap)
  bool scored;
};

Bird  bird;
Pipe  pipes[3];
int   pipeCount = 0;
int   score     = 0;
int   bestScore = 0;

// Scroll counters
int  groundScroll = 0;   // pixel offset for ground dashes
int  bgScroll     = 0;   // pixel offset for clouds * 10

// Timing
unsigned long lastFrame    = 0;
unsigned long lastPipeTime = 0;
unsigned long deadTime     = 0;

// Touch debounce
bool     lastTouched  = false;
unsigned long touchDebounce = 0;

// Cloud positions (simple, fixed pattern)
int cloudX[3] = { 30, 130, 230 };
int cloudY[3] = { 40,  28,  50 };
int cloudW[3] = { 46,  38,  52 };

// ─── Helpers ─────────────────────────────────────────────────────────────────
// pseudo-random (no stdlib rand needed)
uint32_t rngState = 12345;
int myRand(int lo, int hi) {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return lo + (int)(rngState % (uint32_t)(hi - lo + 1));
}

// ─── Draw helpers ────────────────────────────────────────────────────────────
void drawSky() {
  spr.fillRect(0, 0, W, H - GROUND_H, C_SKY);
}

void drawCloud(int cx, int cy, int cw, int ch) {
  int rx = cw / 2, ry = ch / 2;
  spr.fillEllipse(cx,              cy,           rx,          ry,          C_WHITE);
  spr.fillEllipse(cx - cw*22/100, cy + ch/10,   cw*35/100,  ch*42/100,   C_WHITE);
  spr.fillEllipse(cx + cw*22/100, cy + ch/10,   cw*35/100,  ch*42/100,   C_WHITE);
}

void drawClouds() {
  int scroll = bgScroll / 10;   // slow parallax
  for (int i = 0; i < 3; i++) {
    int cx = ((cloudX[i] - scroll % (W + 60)) + W + 60) % (W + 60) - 10;
    drawCloud(cx, cloudY[i], cloudW[i], cloudW[i] / 3);
  }
}

void drawGround() {
  spr.fillRect(0, H - GROUND_H, W, GROUND_H,      C_GROUND);
  spr.fillRect(0, H - GROUND_H, W, 8,              C_GROUND_TOP);
  // Dashes
  int gs = groundScroll % 18;
  for (int x = -gs; x < W; x += 18) {
    spr.drawFastHLine(x, H - GROUND_H + 10, 10, 0xC60B); // tan
  }
}

void drawPipe(int px, int gapY) {
  int topH  = gapY;
  int botY  = gapY + PIPE_GAP;
  int botH  = H - GROUND_H - botY;
  int capX  = px - 3;

  // ── Top pipe ──
  if (topH - PIPE_CAP_H > 0)
    spr.fillRect(px, 0, PIPE_W, topH - PIPE_CAP_H, C_PIPE);
  // right shadow
  if (topH - PIPE_CAP_H > 0)
    spr.fillRect(px + PIPE_W - 5, 0, 5, topH - PIPE_CAP_H, C_PIPE_DARK);
  // cap
  spr.fillRect(capX, topH - PIPE_CAP_H, PIPE_CAP_W, PIPE_CAP_H, C_PIPE_CAP);
  spr.fillRect(capX + PIPE_CAP_W - 5, topH - PIPE_CAP_H, 5, PIPE_CAP_H, C_PIPE_CAPDK);

  // ── Bottom pipe ──
  spr.fillRect(capX, botY, PIPE_CAP_W, PIPE_CAP_H, C_PIPE_CAP);
  spr.fillRect(capX + PIPE_CAP_W - 5, botY, 5, PIPE_CAP_H, C_PIPE_CAPDK);
  if (botH - PIPE_CAP_H > 0) {
    spr.fillRect(px, botY + PIPE_CAP_H, PIPE_W, botH - PIPE_CAP_H, C_PIPE);
    spr.fillRect(px + PIPE_W - 5, botY + PIPE_CAP_H, 5, botH - PIPE_CAP_H, C_PIPE_DARK);
  }
}

void drawBird(int by, bool flap) {
  int bx = BIRD_X;
  // Body
  spr.fillEllipse(bx, by, 10, 8, C_BIRD_YEL);
  // Wing
  int wingOff = flap ? -2 : 2;
  spr.fillEllipse(bx - 2, by + 2 + wingOff, 7, 4, C_BIRD_YEL);
  // Belly
  spr.fillEllipse(bx + 3, by + 3, 6, 5, C_BIRD_ORG);
  // Eye white
  spr.fillCircle(bx + 5, by - 3, 4, C_WHITE);
  // Pupil
  spr.fillCircle(bx + 6, by - 3, 2, C_BLACK);
  // Beak
  spr.fillTriangle(bx + 8, by - 1,  bx + 14, by + 1,  bx + 8, by + 3, C_BIRD_ORG);
}

void drawScoreText(int s, int x, int y, uint16_t col) {
  spr.setTextColor(col);
  spr.setTextSize(3);
  spr.setCursor(x, y);
  spr.print(s);
}

void drawCenteredText(const char* txt, int y, int sz, uint16_t col) {
  spr.setTextSize(sz);
  spr.setTextColor(col);
  int tw = strlen(txt) * 6 * sz;
  spr.setCursor((W - tw) / 2, y);
  spr.print(txt);
}

// ─── Game logic ──────────────────────────────────────────────────────────────
void resetGame() {
  bird.y  = (H / 2 - 20) * 100;
  bird.vy = 0;
  bird.flapTick = 0;
  pipeCount = 0;
  score     = 0;
  groundScroll = 0;
  bgScroll     = 0;
  lastPipeTime = millis();
}

void doJump() {
  bird.vy = JUMP_VEL_F;
}

void spawnPipe() {
  if (pipeCount >= 3) return;
  int gapY = myRand(40, H - GROUND_H - PIPE_GAP - 40);
  pipes[pipeCount] = { W + 10, gapY, false };
  pipeCount++;
}

bool checkCollision() {
  int by = bird.y / 100;
  if (by + BIRD_R >= H - GROUND_H) return true;
  if (by - BIRD_R <= 0)            return true;
  for (int i = 0; i < pipeCount; i++) {
    int px = pipes[i].x;
    if (BIRD_X + BIRD_R > px + 3 && BIRD_X - BIRD_R < px + PIPE_W - 3) {
      if (by - BIRD_R < pipes[i].gapY || by + BIRD_R > pipes[i].gapY + PIPE_GAP)
        return true;
    }
  }
  return false;
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  tft.init();
  tft.setRotation(1);   // landscape
  tft.fillScreen(C_SKY);

  spr.createSprite(W, H);
  spr.setTextFont(1);

  touch.begin();
  touch.setRotation(1);

  resetGame();
  state = IDLE;
  lastFrame = millis();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  unsigned long dt  = now - lastFrame;
  if (dt < 10) return;   // ~100 fps cap
  lastFrame = now;

  // ── Touch input ────────────────────────────────────────────────────────────
  bool touched = touch.tirqTouched() && touch.touched();
  if (touched && !lastTouched && (now - touchDebounce > 150)) {
    touchDebounce = now;
    if (state == IDLE) {
      state = PLAYING;
      doJump();
    } else if (state == PLAYING) {
      doJump();
    } else if (state == DEAD && (now - deadTime > 800)) {
      resetGame();
      state = PLAYING;
      doJump();
    }
  }
  lastTouched = touched;

  // ── Physics / update ───────────────────────────────────────────────────────
  if (state == PLAYING) {
    // Gravity
    bird.vy += (GRAVITY_F * (int)dt) / 100;
    bird.y  += (bird.vy   * (int)dt) / 100;

    // Scroll
    groundScroll += (PIPE_SPEED_F * (int)dt) / 1000;
    bgScroll     += (int)dt / 2;

    // Flap anim
    bird.flapTick += dt;

    // Spawn pipes
    if (now - lastPipeTime >= (unsigned long)PIPE_INTERVAL) {
      spawnPipe();
      lastPipeTime = now;
    }

    // Move pipes & score
    for (int i = 0; i < pipeCount; i++) {
      pipes[i].x -= (PIPE_SPEED_F * (int)dt) / 1000;
      if (!pipes[i].scored && pipes[i].x + PIPE_W < BIRD_X) {
        pipes[i].scored = true;
        score++;
        if (score > bestScore) bestScore = score;
      }
    }

    // Remove off-screen pipes (shift array)
    for (int i = 0; i < pipeCount; ) {
      if (pipes[i].x < -PIPE_W - 10) {
        for (int j = i; j < pipeCount - 1; j++) pipes[j] = pipes[j+1];
        pipeCount--;
      } else i++;
    }

    // Collision
    if (checkCollision()) {
      state    = DEAD;
      deadTime = now;
      bird.vy  = 0;
    }
  } else if (state == IDLE) {
    // Gentle bob
    bird.y = (H / 2 - 20 + (int)(sin(now / 400.0) * 5)) * 100;
    bgScroll     += dt / 3;
    groundScroll += dt * 8 / 100;
    bird.flapTick += dt;
  }

  // ── Draw frame ─────────────────────────────────────────────────────────────
  drawSky();
  drawClouds();

  // Pipes
  for (int i = 0; i < pipeCount; i++) drawPipe(pipes[i].x, pipes[i].gapY);

  drawGround();

  // Bird
  int by = bird.y / 100;
  bool flap = (bird.flapTick / 200) % 2 == 0;
  if (state == DEAD && by >= H - GROUND_H - 12) by = H - GROUND_H - 12;
  drawBird(by, flap);

  // HUD
  if (state == PLAYING) {
    // Score (top center)
    spr.setTextColor(C_BLACK);
    spr.setTextSize(3);
    char buf[8]; itoa(score, buf, 10);
    int tw = strlen(buf) * 18;
    spr.setCursor(W/2 - tw/2 + 2, 8);
    spr.print(buf);
    spr.setTextColor(C_WHITE);
    spr.setCursor(W/2 - tw/2, 6);
    spr.print(buf);

  } else if (state == IDLE) {
    drawCenteredText("FLAPPY BIRD", H/2 - 30, 2, C_GOLD);
    drawCenteredText("CHAM DE BAT DAU", H/2 + 2, 1, C_WHITE);

  } else if (state == DEAD) {
    // Dark panel
    spr.fillRect(W/2-72, H/2-40, 144, 84, 0x0000);
    spr.fillRect(W/2-70, H/2-38, 140, 80, 0x2104); // dark grey
    drawCenteredText("GAME OVER",   H/2 - 28, 2, C_RED);
    // Score lines
    spr.setTextSize(1);
    spr.setTextColor(C_WHITE);
    char s1[20]; sprintf(s1, "Score: %d", score);
    char s2[20]; sprintf(s2, "Best:  %d", bestScore);
    int w1 = strlen(s1)*6; spr.setCursor((W-w1)/2, H/2+2);  spr.print(s1);
    spr.setTextColor(C_GOLD);
    int w2 = strlen(s2)*6; spr.setCursor((W-w2)/2, H/2+14); spr.print(s2);
    // Blinking
    if ((now / 500) % 2 == 0) {
      drawCenteredText("CHAM DE CHOI LAI", H/2+28, 1, C_WHITE);
    }
  }

  spr.pushSprite(0, 0);
}
