#define USER_SETUP_ID 101

#define ILI9341_DRIVER

#define TFT_WIDTH 240
#define TFT_HEIGHT 320

#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS 5
#define TFT_DC 27
#define TFT_RST 14
#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY 27000000
#define SPI_READ_FREQUENCY 16000000

// If red and blue are swapped on your module, change TFT_BGR to TFT_RGB.
// #define TFT_RGB_ORDER TFT_BGR
