#include <gba.h>
#include <stdbool.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// GBA HARDWARE CONSTANTS & MEMORY MAPS
// -----------------------------------------------------------------------------
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 160

#define MEM_VRAM_OBJ ((volatile uint16_t *)(0x06010000))
#define MEM_OAM ((volatile OBJATTR *)(0x07000000))
#define PALETTE_BG ((volatile uint16_t *)(0x05000000))  // Background Palette RAM
#define PALETTE_OBJ ((volatile uint16_t *)(0x05000200)) // Sprite Palette RAM

static OBJATTR oam_buffer[128];

// -----------------------------------------------------------------------------
// EXTERNAL BINARY ASSET SYMBOLS (from gfx/ & audio/)
// -----------------------------------------------------------------------------
extern const uint16_t ingame_pal_bin[];
extern const uint32_t ingame_pal_bin_size;

extern const uint8_t ingame_img_bin[];
extern const uint32_t ingame_img_bin_size;

// Sound files
extern const int16_t jump_wav[];
extern const uint32_t jump_wav_size;

extern const int16_t Hat_wav[];
extern const uint32_t Hat_wav_size;

extern const int16_t death_wav[];
extern const uint32_t death_wav_size;

// -----------------------------------------------------------------------------
// PHYSICS TUNING
// -----------------------------------------------------------------------------
#define GRAVITY             0x00001000
#define JUMP_VELOCITY      -0x00068000

// -----------------------------------------------------------------------------
// SPRITE TILE INDEX MAP
// -----------------------------------------------------------------------------
typedef enum {
  TILE_IDLE = 0,
  TILE_JUMP_RIGHT = 4,
  TILE_JUMP_LEFT = 8,
  TILE_HAT_1 = 12,
  TILE_HAT_2 = 16,
  TILE_HAT_3 = 20,
  TILE_FALL_1 = 24,
  TILE_FALL_2 = 28,
  TILE_PLATFORM = 32,
  TILE_PLATFORM_SPIKE = 36,
  TILE_ENEMY_1 = 40,
  TILE_ENEMY_2 = 44,
  TILE_ENEMY_3 = 48
} TileSpriteOffset;

static int8_t sfx_pcm8_buffer[2048];

// -----------------------------------------------------------------------------
// PALETTE UTILITIES
// -----------------------------------------------------------------------------
// Safely loads palette data into Sprite Palette RAM using 16-bit word transfers
void load_sprite_palette(const uint16_t *pal_data, uint32_t size_bytes) {
  uint32_t color_count = size_bytes / sizeof(uint16_t);
  if (color_count > 256) color_count = 256;

  for (uint32_t i = 0; i < color_count; i++) {
    PALETTE_OBJ[i] = pal_data[i];
  }
}

// -----------------------------------------------------------------------------
// DIRECT SOUND ENGINE
// -----------------------------------------------------------------------------
void init_sound() {
  REG_SOUNDCNT_X = 0x80;    // Master Sound Enable
  REG_SOUNDCNT_H = 0x0B0F;  // Direct Sound A, full volume
  REG_SOUNDCNT_L = 0x0000;
}

void play_sfx_wav16(const int16_t *wav16_data, uint32_t bytes_size) {
  if (!wav16_data || bytes_size == 0) return;

  uint32_t sample_count = bytes_size / sizeof(int16_t);
  uint32_t count = sample_count > 2048 ? 2048 : sample_count;

  for (uint32_t i = 0; i < count; i++) {
    sfx_pcm8_buffer[i] = (int8_t)(wav16_data[i] >> 8);
  }

  REG_DMA1CNT = 0;
  REG_TM0CNT_H = 0;
  REG_TM0CNT_L = 65536 - 1048; // ~16kHz

  REG_DMA1SAD = (uint32_t)sfx_pcm8_buffer;
  REG_DMA1DAD = (uint32_t)0x040000A0; // REG_FIFO_A
  REG_DMA1CNT = 0xB6400000;
  REG_TM0CNT_H = 0x0080;
}

// -----------------------------------------------------------------------------
// GAMEPLAY STRUCTURES AND STATES
// -----------------------------------------------------------------------------
typedef struct {
  int32_t x, y;
  int32_t vx, vy;
  uint16_t tile_id;
  bool facing_right;
  bool hat_active;
  uint16_t hat_timer;
} Player;

typedef struct {
  int16_t x, y;
  bool is_spiked;
  bool active;
} Platform;

typedef struct {
  int16_t x, y;
  uint8_t type;
  bool active;
} Enemy;

#define MAX_PLATFORMS 8
#define MAX_ENEMIES 2

static Player player;
static Platform platforms[MAX_PLATFORMS];
static Enemy enemies[MAX_ENEMIES];
static uint32_t score = 0;
static uint32_t anim_frame = 0;
static bool game_over = false;

// -----------------------------------------------------------------------------
// OAM GRAPHICS UPDATE
// -----------------------------------------------------------------------------
void update_oam() {
  for (int i = 0; i < 128; i++) {
    oam_buffer[i].attr0 = 160;
    oam_buffer[i].attr1 = 240;
  }

  // 1. Draw Player
  oam_buffer[0].attr0 = (player.y >> 16) & 0xFF;
  oam_buffer[0].attr0 |= (0 << 14); // Square shape
  oam_buffer[0].attr1 = (player.x >> 16) & 0x1FF;
  oam_buffer[0].attr1 |= (1 << 14); // 16x16 size

  if (!player.facing_right) {
    oam_buffer[0].attr1 |= (1 << 12);
  }
  oam_buffer[0].attr2 = player.tile_id;

  // 2. Draw Platforms
  for (int i = 0; i < MAX_PLATFORMS; i++) {
    if (!platforms[i].active) continue;
    int idx = 1 + i;
    oam_buffer[idx].attr0 = (platforms[i].y & 0xFF);
    oam_buffer[idx].attr1 = (platforms[i].x & 0x1FF) | (1 << 14);
    oam_buffer[idx].attr2 = platforms[i].is_spiked ? TILE_PLATFORM_SPIKE : TILE_PLATFORM;
  }

  // 3. Draw Enemies
  for (int i = 0; i < MAX_ENEMIES; i++) {
    if (!enemies[i].active) continue;
    int idx = 1 + MAX_PLATFORMS + i;
    oam_buffer[idx].attr0 = (enemies[i].y & 0xFF);
    oam_buffer[idx].attr1 = (enemies[i].x & 0x1FF) | (1 << 14);

    uint16_t etile = TILE_ENEMY_1;
    if (enemies[i].type == 1) etile = TILE_ENEMY_2;
    if (enemies[i].type == 2) etile = TILE_ENEMY_3;

    oam_buffer[idx].attr2 = etile;
  }

  dmaCopy((void *)oam_buffer, (void *)MEM_OAM, sizeof(oam_buffer));
}

// -----------------------------------------------------------------------------
// GAME LOOP FUNCTIONS
// -----------------------------------------------------------------------------
void init_game() {
  score = 0;
  game_over = false;

  player.x = (120 - 8) << 16;
  player.y = (100) << 16;
  player.vx = 0;
  player.vy = JUMP_VELOCITY;
  player.facing_right = true;
  player.hat_active = false;
  player.hat_timer = 0;
  player.tile_id = TILE_JUMP_RIGHT;

  for (int i = 0; i < MAX_PLATFORMS; i++) {
    platforms[i].x = (i * 30 + 10) % (SCREEN_WIDTH - 16);
    platforms[i].y = 150 - (i * 32);
    platforms[i].is_spiked = (i > 3 && (i % 3 == 0));
    platforms[i].active = true;
  }

  for (int i = 0; i < MAX_ENEMIES; i++) {
    enemies[i].active = false;
  }
}

void process_input() {
  scanKeys();
  uint16_t keys = keysHeld();

  if (keys & KEY_LEFT) {
    player.vx = -0x00018000;
    player.facing_right = false;
  } else if (keys & KEY_RIGHT) {
    player.vx = 0x00018000;
    player.facing_right = true;
  } else {
    player.vx = 0;
  }

  uint16_t keys_down = keysDown();
  if ((keys_down & KEY_A) && !player.hat_active) {
    player.hat_active = true;
    player.hat_timer = 180;
    play_sfx_wav16(Hat_wav, (uint32_t)&Hat_wav_size);
  }
}

void update_game() {
  if (game_over) {
    if (keysDown() & KEY_START) {
      init_game();
    }
    return;
  }

  anim_frame++;

  if (player.hat_active) {
    player.vy = -0x00030000;
    player.hat_timer--;

    uint8_t h_frame = (anim_frame / 4) % 3;
    player.tile_id = TILE_HAT_1 + (h_frame * 4);

    if (player.hat_timer == 0) {
      player.hat_active = false;
    }
  } else {
    player.vy += GRAVITY;

    if (player.vy < 0) {
      player.tile_id = player.facing_right ? TILE_JUMP_RIGHT : TILE_JUMP_LEFT;
    } else {
      player.tile_id = ((anim_frame / 8) % 2 == 0) ? TILE_FALL_1 : TILE_FALL_2;
    }
  }

  player.x += player.vx;
  player.y += player.vy;

  int32_t px = player.x >> 16;
  if (px < -8) player.x = (SCREEN_WIDTH - 8) << 16;
  if (px > SCREEN_WIDTH - 8) player.x = (-8) << 16;

  int32_t py = player.y >> 16;

  if (py < 70) {
    int32_t diff = 70 - py;
    player.y = 70 << 16;
    score += diff;

    for (int i = 0; i < MAX_PLATFORMS; i++) {
      platforms[i].y += diff;
      if (platforms[i].y > SCREEN_HEIGHT) {
        platforms[i].y = 0;
        platforms[i].x = (uint16_t)(py + i * 47) % (SCREEN_WIDTH - 16);
        platforms[i].is_spiked = ((score > 500) && (i % 2 == 0));
      }
    }
  }

  if (player.vy > 0 && !player.hat_active) {
    for (int i = 0; i < MAX_PLATFORMS; i++) {
      if (!platforms[i].active) continue;

      int16_t plat_x = platforms[i].x;
      int16_t plat_y = platforms[i].y;

      if ((px + 12 >= plat_x) && (px <= plat_x + 16) &&
          (py + 16 >= plat_y) && (py + 16 <= plat_y + 6)) {

        if (platforms[i].is_spiked) {
          game_over = true;
          play_sfx_wav16(death_wav, (uint32_t)&death_wav_size);
          return;
        } else {
          player.vy = JUMP_VELOCITY;
          play_sfx_wav16(jump_wav, (uint32_t)&jump_wav_size);
        }
      }
    }
  }

  if (py > SCREEN_HEIGHT) {
    game_over = true;
    play_sfx_wav16(death_wav, (uint32_t)&death_wav_size);
  }
}

// -----------------------------------------------------------------------------
// MAIN ENTRY POINT
// -----------------------------------------------------------------------------
int main(void) {
  irqInit();
  irqEnable(IRQ_VBLANK);

  // Set Display Mode 0 (Tilemap / Sprites), Enable Sprites, 1D Mapping
  REG_DISPCNT = MODE_0 | OBJ_ENABLE | OBJ_1D_MAP;

  // Background color set to white
  PALETTE_BG[0] = RGB5(31, 31, 31);

  // 1. Copy sprite tile graphic data to VRAM
  dmaCopy((void *)ingame_img_bin, (void *)MEM_VRAM_OBJ, (uint32_t)&ingame_img_bin_size);

  // 2. Safely load Palette binary into Sprite Palette RAM
  load_sprite_palette(ingame_pal_bin, (uint32_t)&ingame_pal_bin_size);

  init_sound();
  init_game();

  while (1) {
    VBlankIntrWait();

    process_input();
    update_game();
    update_oam();
  }

  return 0;
}