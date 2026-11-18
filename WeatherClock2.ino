#define LV_CONF_INCLUDE_SIMPLE

#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TAMC_GT911.h>

// --- NEW for Pictures ---
#include <SPI.h>
#include <SD.h>
#include <JPEGDEC.h>      // Library Manager: "JPEGDEC" by Larry Bank
#include <vector>
#include <algorithm>

// --- NEW for WiFi setup ---
#include <Preferences.h>  // built into the ESP32 core


// =====================================================
// WIFI
// =====================================================
// Used only until a network has been saved from the touch screen
const char* DEFAULT_WIFI_SSID = "SSID";
const char* DEFAULT_WIFI_PASS = "password";

String savedSSID;
String savedPass;

Preferences prefs;

bool wifiConnected = false;

// ---- scan results ----
struct WifiNet
{
    String ssid;
    int32_t rssi;
    bool open;
};

std::vector<WifiNet> scannedNets;
bool wifiScanRunning = false;

// ---- connect attempt (non-blocking) ----
bool wifiConnecting = false;
unsigned long wifiConnectStart = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 15000;
String pendingSSID;
String pendingPass;

// ---- startup connection ----
bool wifiBootAttempt = false;      // true while the power-on connect is running
String bootFailedSSID;             // shown on the setup screen if it failed

// ---- jump to the weather screen after connecting ----
const unsigned long WEATHER_SWITCH_DELAY = 10000;   // 10 s
bool weatherSwitchPending = false;
unsigned long wifiConnectedAt = 0;

// countdown shown on the Dashboard while waiting to switch
void *countdownLabelPtr = nullptr;      // (lv_obj_t*, declared before LVGL types)

// ---- "throbbing" text: fades between 30% and 100% opacity ----
static void throbExec(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void startThrob(lv_obj_t *obj)
{
    lv_anim_t a;
    lv_anim_init(&a);

    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, throbExec);
    lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

    lv_anim_set_duration(&a, 700);              // fade in...
#if LV_VERSION_CHECK(9, 3, 0)
    lv_anim_set_reverse_duration(&a, 700);      // ...and back out
#else
    lv_anim_set_playback_duration(&a, 700);
#endif
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);

    lv_anim_start(&a);
}

void stopThrob(lv_obj_t *obj)
{
    lv_anim_delete(obj, throbExec);
    lv_obj_set_style_text_opa(obj, LV_OPA_COVER, 0);    // back to solid
}

// ---- "Weather data provided by OpenWeather" credit ----
// Small text that slides in from the right edge each time its screen
// opens, stops centred, and stays there while the screen is shown.

#define CREDIT_TEXT        "Weather data provided by OpenWeather"
#define CREDIT_SPEED_PX_S  60          // slide-in speed, pixels per second

static void creditExec(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

// screen starting to slide in: park the text just off the right edge
static void creditReset(lv_event_t *e)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);

    lv_anim_delete(lbl, creditExec);
    lv_obj_set_x(lbl, 320);
}

// screen fully shown: slide the text in and stop when centred
static void creditStart(lv_event_t *e)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);

    int32_t textW = lv_obj_get_width(lbl);
    int32_t centreX = (320 - textW) / 2;

    lv_anim_delete(lbl, creditExec);

    lv_anim_t a;
    lv_anim_init(&a);

    lv_anim_set_var(&a, lbl);
    lv_anim_set_exec_cb(&a, creditExec);
    lv_anim_set_values(&a, 320, centreX);
    lv_anim_set_duration(&a, (uint32_t)((320 - centreX) * 1000 / CREDIT_SPEED_PX_S));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);    // eases to a gentle stop

    lv_anim_start(&a);      // plays once, then the text stays centred
}

lv_obj_t *addCreditTicker(lv_obj_t *parent, int y, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_label_set_text(lbl, CREDIT_TEXT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_70, 0);      // "small print"
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    // The text starts off-screen. Without this, LVGL would make the
    // screen scrollable and show a moving scrollbar.
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_FLOATING);             // don't count it in the screen's size
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);     // screen never scrolls
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_pos(lbl, 320, y);           // waiting off the right edge
    lv_obj_update_layout(lbl);             // so its width is known

    // replay the slide-in every time this screen is opened
    lv_obj_add_event_cb(parent, creditReset, LV_EVENT_SCREEN_LOAD_START, lbl);
    lv_obj_add_event_cb(parent, creditStart, LV_EVENT_SCREEN_LOADED, lbl);

    return lbl;
}

// =====================================================
// TIME
// =====================================================

unsigned long lastClockUpdate = 0;

bool timeSynchronized = false;

unsigned long lastTimeCheck = 0;

// UTC offset (seconds) of the weather city, e.g. -21600 = CST, -18000 = CDT.
// OpenWeatherMap sends the CURRENT offset (daylight saving included) with
// every weather update, so the clock follows the city and its DST changes.
long weatherTzOffset = -6 * 3600;

// Set the ESP32's local time zone from a UTC offset in seconds
void applyTimeZone(long offsetSec)
{
    // POSIX TZ strings use the opposite sign: UTC-6 is written "UTC+6"
    long o = -offsetSec;
    long a = labs(o);

    int h = a / 3600;
    int m = (a % 3600) / 60;

    char tz[20];

    if (m)
        snprintf(tz, sizeof(tz), "UTC%c%d:%02d", o < 0 ? '-' : '+', h, m);
    else
        snprintf(tz, sizeof(tz), "UTC%c%d", o < 0 ? '-' : '+', h);

    setenv("TZ", tz, 1);
    tzset();

    Serial.printf("Time zone set: UTC%+.2f h (TZ=%s)\n",
                  offsetSec / 3600.0f, tz);
}

// =====================================================
// UPDATE WEATHER
// =====================================================
const char* OWM_API_KEY = "insert your key here";

// Defaults - used until a city is saved from the City screen
const char* DEFAULT_CITY     = "Belton";
const char* DEFAULT_STATE    = "MO";
const char* DEFAULT_COUNTRY  = "US";

String weatherCity;
String weatherState;
String weatherCountry;

String weatherPlaceName;        // name OpenWeatherMap matched, e.g. "Belton"
int lastWeatherHttpCode = 0;    // 200 ok, 404 city not found, <0 network

float currentTemp = 0;
float currentHumidity = 0;
float currentFeelsLike = 0;

// Weather screen colours (0xRRGGBB) - changed on the Weather Page Color screen
#define DEFAULT_WEATHER_BG    0x2e2d37
#define DEFAULT_WEATHER_TEXT  0xa0a0a0

uint32_t weatherBgColor   = DEFAULT_WEATHER_BG;
uint32_t weatherTextColor = DEFAULT_WEATHER_TEXT;

float currentWindMph  = 0;     // wind speed
float currentWindGust = 0;     // 0 = no gust reported
int   currentWindDeg  = -1;    // direction wind blows FROM (0 = N); -1 = unknown

long currentSunrise = 0;        // UTC unix time from OpenWeatherMap
long currentSunset  = 0;

String currentCondition = "Loading...";
String currentIconCode = "";        // OpenWeatherMap icon id, e.g. "01d"
unsigned long lastWeatherUpdate = 0;

const unsigned long
    weatherInterval =
    600000;         // 10 minutes between OpenWeather updates



// =====================================================
// DISPLAY
// =====================================================

#define GFX_BL 27

// =====================================================
// BACKLIGHT (PWM brightness)
// =====================================================

#define BL_PWM_FREQ     5000
#define BL_PWM_BITS     8          // duty 0-255
#define BL_PWM_CHANNEL  0          // only used on ESP32 core 2.x
#define BL_MIN_PERCENT  1          // never fully black - you'd lose the slider

int brightnessPercent = 100;

// Auto Dim: 5% at sunset, 100% at sunrise (weather city's sun times)
bool autoDimEnabled = false;
int  autoDimPhase = -1;             // -1 unknown, 0 night, 1 day
#define AUTO_DIM_NIGHT_PERCENT  2      // default low limit (after sunset)
#define AUTO_DIM_DAY_PERCENT    80     // default high limit (after sunrise)

// user-adjustable limits, set with the range slider on Settings
int autoDimLow  = AUTO_DIM_NIGHT_PERCENT;
int autoDimHigh = AUTO_DIM_DAY_PERCENT;

void setBacklight(int percent)
{
    percent = constrain(percent, BL_MIN_PERCENT, 100);
    brightnessPercent = percent;

    uint32_t duty = (percent * 255) / 100;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(GFX_BL, duty);
#else
    ledcWrite(BL_PWM_CHANNEL, duty);
#endif
}

void initBacklight()
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(GFX_BL, BL_PWM_FREQ, BL_PWM_BITS);
#else
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_BITS);
    ledcAttachPin(GFX_BL, BL_PWM_CHANNEL);
#endif

    prefs.begin("display", true);
    int saved = prefs.getInt("bright", 100);
    prefs.end();

    setBacklight(saved);
}

void saveBrightness()
{
    prefs.begin("display", false);
    prefs.putInt("bright", brightnessPercent);
    prefs.end();
}

// Display is pinned explicitly to the VSPI peripheral so the
// SD card can use HSPI without the two fighting over one bus.
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    2,      // DC
    15,     // CS
    14,     // SCK
    13,     // MOSI
    12,     // MISO
    VSPI);

Arduino_GFX *gfx =
    new Arduino_ST7796(
        bus,
        GFX_NOT_DEFINED,
        0);

#define SCREEN_W 320
#define SCREEN_H 480

// =====================================================
// TF / SD CARD  (CYD 3.5" pinout)
// =====================================================

#define SD_CS    5
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  23

SPIClass sdSPI(HSPI);
bool sdReady = false;

// Folder on the card to read pictures from ("/" = root).
// Change to e.g. "/pictures" if you keep them in a folder.
const char* PICTURE_DIR = "/";

std::vector<String> pictureFiles;
int pictureIndex = 0;

bool pictureDrawPending = false;
unsigned long pictureRequestAt = 0;

// =====================================================
// TOUCH
// =====================================================

TAMC_GT911 touch(
    33,
    32,
    -1,
    -1,
    320,
    480);

// =====================================================
// LVGL
// =====================================================

// Draw buffer is taken from the heap in setup() instead of being a
// static array - static RAM (.bss) on the ESP32 is limited and was full.
#define DRAW_BUF_PIXELS  (320 * 10)
#define DRAW_BUF_BYTES   (DRAW_BUF_PIXELS * sizeof(uint16_t))

static uint16_t *drawBuf = nullptr;

lv_display_t *display;
lv_indev_t *touch_indev;


// =====================================================
// SCREENS
// =====================================================

lv_obj_t *homeScreen;
lv_obj_t *weatherScreen;
lv_obj_t *settingsScreen;
lv_obj_t *picturesScreen;
lv_obj_t *wifiScreen;
lv_obj_t *cityScreen;
lv_obj_t *colorScreen;
lv_obj_t *windScreen = nullptr;
lv_obj_t *forecastScreen = nullptr;


// =====================================================
// PAGE STATE
// =====================================================

enum ScreenID
{
    SCREEN_HOME,
    SCREEN_WEATHER,
    SCREEN_SETTINGS,
    SCREEN_PICTURES,
    SCREEN_WIFI,
    SCREEN_CITY,
    SCREEN_COLORS,
    SCREEN_WIND,
    SCREEN_FORECAST
};

ScreenID currentScreen = SCREEN_HOME;

// =====================================================
// WEATHER OBJECTS
// =====================================================

lv_obj_t *feelsLikeLabel = nullptr;
lv_obj_t *timeLabel = nullptr;
lv_obj_t *dateLabel = nullptr;

lv_obj_t *tempLabel = nullptr;
lv_obj_t *cityNameLabel = nullptr;
lv_obj_t *humidityLabel = nullptr;
lv_obj_t *sunriseLabel = nullptr;
lv_obj_t *sunsetLabel = nullptr;

lv_obj_t *weatherIcon = nullptr;
lv_obj_t *weatherCredit = nullptr;      // scrolling OpenWeather credit

// =====================================================
// PICTURES OBJECTS
// =====================================================

lv_obj_t *picturesMsgLabel = nullptr;

// =====================================================
// WIFI SETUP OBJECTS
// =====================================================

lv_obj_t *settingsStatusLabel = nullptr;

lv_obj_t *brightSliderObj = nullptr;    // Settings brightness slider
lv_obj_t *brightLabelObj = nullptr;     // "Brightness 85%" label
lv_obj_t *autoDimBtn = nullptr;
lv_obj_t *dimRangeSlider = nullptr;     // two-handle low/high slider
lv_obj_t *dimRangeLabel = nullptr;

lv_obj_t *wifiStatusLabel = nullptr;
lv_obj_t *wifiList = nullptr;
lv_obj_t *wifiScanBtn = nullptr;

lv_obj_t *wifiPassPanel = nullptr;     // full-screen overlay
lv_obj_t *wifiPassTitle = nullptr;
lv_obj_t *wifiPassTA = nullptr;        // text area
lv_obj_t *wifiKeyboard = nullptr;

// =====================================================
// FORWARD DECLARATIONS
// =====================================================
void updateWeatherDisplay();

// UTC unix time -> "6:52 AM" in the weather city's own time zone
String formatCityTime(long utc)
{
    if (utc <= 0)
        return "--:--";

    time_t t = utc + weatherTzOffset;

    struct tm tmv;
    gmtime_r(&t, &tmv);

    int h = tmv.tm_hour % 12;
    if (h == 0)
        h = 12;

    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d %s",
             h, tmv.tm_min, tmv.tm_hour >= 12 ? "PM" : "AM");

    return String(buf);
}

// "Belton, MO" - city plus 2-letter state (state left off if blank)
String cityWithState()
{
    if (weatherState.length() > 0)
        return weatherCity + ", " + weatherState;

    return weatherCity;
}
void createHomeScreen();
void createWeatherScreen();
void createSettingsScreen();
void createPicturesScreen();
void createWifiScreen();
void createCityScreen();
void createColorScreen();
void createWindScreen();
void createForecastScreen();

void goToHome();
void goToWeather();
void goToSettings();
void goToPictures();
void goToWifiSetup();
void goToCity();
void goToWeatherColors();
void goToWind();
void goToForecast();
void weatherIconClicked(lv_event_t *e);
void applyForecastColors();
void pollForecastFetch();
void goBackToWeather();
void updateWindScreen();
void applyWindColors();

void setAutoDim(bool on);
void updateDimRangeLabel();
void saveDimLimits();
void showBrightness(int percent);

void applyWeatherColors();
void loadWeatherColors();

void updateSettingsStatus();
void startNTP();

void requestPictureDraw();

// =====================================================
// DISPLAY FLUSH
// =====================================================

void my_flush_cb(
    lv_display_t *disp,
    const lv_area_t *area,
    uint8_t *px_map)
{
    gfx->draw16bitRGBBitmap(
        area->x1,
        area->y1,
        (uint16_t *)px_map,
        area->x2 - area->x1 + 1,
        area->y2 - area->y1 + 1);

    lv_display_flush_ready(disp);
}

// =====================================================
// TOUCH READ
// =====================================================

void my_touch_read(
    lv_indev_t *indev,
    lv_indev_data_t *data)
{
    touch.read();

    if (touch.isTouched)
    {
        data->state =
            LV_INDEV_STATE_PRESSED;

        data->point.x =
            320 - touch.points[0].x;

        data->point.y =
            480 - touch.points[0].y;
    }
    else
    {
        data->state =
            LV_INDEV_STATE_RELEASED;
    }
}

// =====================================================
// JPEG DECODER CALLBACKS (JPEGDEC -> SD -> display)
// =====================================================

static File jpgFile;

void *jpgOpen(const char *filename, int32_t *size)
{
    jpgFile = SD.open(filename, FILE_READ);

    if (!jpgFile)
        return nullptr;

    *size = jpgFile.size();
    return &jpgFile;
}

void jpgClose(void *handle)
{
    if (jpgFile)
        jpgFile.close();
}

int32_t jpgRead(JPEGFILE *handle, uint8_t *buffer, int32_t length)
{
    if (!jpgFile)
        return 0;

    return jpgFile.read(buffer, length);
}

int32_t jpgSeek(JPEGFILE *handle, int32_t position)
{
    if (!jpgFile)
        return 0;

    return jpgFile.seek(position);
}

// Each decoded block is pushed straight to the panel.
// (A full 320x480 frame buffer won't fit in the CYD's RAM,
//  so the picture is drawn directly, bypassing LVGL.)
int jpgDraw(JPEGDRAW *pDraw)
{
    gfx->draw16bitRGBBitmap(
        pDraw->x,
        pDraw->y,
        pDraw->pPixels,
        pDraw->iWidth,
        pDraw->iHeight);

    return 1;
}

// =====================================================
// PICTURE FILE HANDLING
// =====================================================

bool isJpegName(String name)
{
    // skip hidden files, incl. macOS "._photo.jpg" junk files
    if (name.startsWith("."))
        return false;

    name.toLowerCase();

    return name.endsWith(".jpg") ||
           name.endsWith(".jpeg");
}

void scanPictures()
{
    pictureFiles.clear();

    if (!sdReady)
        return;

    File dir = SD.open(PICTURE_DIR);

    if (!dir || !dir.isDirectory())
    {
        Serial.printf("Pictures: cannot open %s\n", PICTURE_DIR);
        return;
    }

    String base = String(PICTURE_DIR);
    if (!base.endsWith("/"))
        base += "/";

    File f = dir.openNextFile();

    while (f)
    {
        if (!f.isDirectory())
        {
            String name = f.name();

            // older ESP32 cores return the full path
            int slash = name.lastIndexOf('/');
            if (slash >= 0)
                name = name.substring(slash + 1);

            if (isJpegName(name))
                pictureFiles.push_back(base + name);
        }

        f.close();
        f = dir.openNextFile();
    }

    dir.close();

    std::sort(pictureFiles.begin(), pictureFiles.end());

    if (pictureIndex >= (int)pictureFiles.size())
        pictureIndex = 0;

    Serial.printf("Pictures: found %d JPEG(s)\n", (int)pictureFiles.size());
}

void drawPictureMessage(const char *msg)
{
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setCursor(10, SCREEN_H / 2 - 8);
    gfx->print(msg);
}

void showPicture(int index)
{
    if (pictureFiles.empty())
        return;

    const String &path = pictureFiles[index];

    Serial.printf("Pictures: showing %s\n", path.c_str());

    gfx->fillScreen(0x0000);

    // Allocate the decoder only while drawing (~17 KB)
    JPEGDEC *jpeg = new JPEGDEC();

    if (!jpeg)
    {
        drawPictureMessage("Out of memory");
        return;
    }

    if (!jpeg->open(path.c_str(),
                    jpgOpen,
                    jpgClose,
                    jpgRead,
                    jpgSeek,
                    jpgDraw))
    {
        drawPictureMessage("Can't open image");
        delete jpeg;
        return;
    }

    int w = jpeg->getWidth();
    int h = jpeg->getHeight();

    // Pick the smallest built-in downscale that fits the screen
    int options = 0;
    int shift = 0;

    while (((w >> shift) > SCREEN_W || (h >> shift) > SCREEN_H) && shift < 3)
        shift++;

    if (shift == 1) options = JPEG_SCALE_HALF;
    if (shift == 2) options = JPEG_SCALE_QUARTER;
    if (shift == 3) options = JPEG_SCALE_EIGHTH;

    int sw = w >> shift;
    int sh = h >> shift;

    int x = max(0, (SCREEN_W - sw) / 2);
    int y = max(0, (SCREEN_H - sh) / 2);

    if (!jpeg->decode(x, y, options))
    {
        // usually a progressive JPEG (JPEGDEC supports baseline only)
        drawPictureMessage("Unsupported JPEG");
    }

    jpeg->close();
    delete jpeg;

    // small "3/12" counter in the bottom-left corner
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setCursor(4, SCREEN_H - 10);
    gfx->printf("%d/%d", index + 1, (int)pictureFiles.size());
}

void requestPictureDraw()
{
    pictureDrawPending = true;
    pictureRequestAt = millis();
}

void nextPicture()
{
    if (pictureFiles.empty())
        return;

    pictureIndex = (pictureIndex + 1) % pictureFiles.size();
    requestPictureDraw();
}

void prevPicture()
{
    if (pictureFiles.empty())
        return;

    pictureIndex--;

    if (pictureIndex < 0)
        pictureIndex = pictureFiles.size() - 1;

    requestPictureDraw();
}

// =====================================================
// NAVIGATION
// =====================================================

void goToHome()
{
    currentScreen = SCREEN_HOME;
    pictureDrawPending = false;

    lv_screen_load_anim(
        homeScreen,
        LV_SCR_LOAD_ANIM_OVER_RIGHT,
        250,
        0,
        false);
}

void goToWeather()
{
    currentScreen = SCREEN_WEATHER;

    lv_screen_load_anim(
        weatherScreen,
        LV_SCR_LOAD_ANIM_OVER_LEFT,
        250,
        0,
        false);
}

void goToSettings()
{
    // coming back from WiFi setup slides the other way
    lv_screen_load_anim_t anim =
        (currentScreen == SCREEN_WIFI ||
         currentScreen == SCREEN_CITY ||
         currentScreen == SCREEN_COLORS)
            ? LV_SCR_LOAD_ANIM_OVER_RIGHT
            : LV_SCR_LOAD_ANIM_OVER_LEFT;

    // delete the City / Color screen after the slide finishes
    bool freeOld =
        (currentScreen == SCREEN_CITY ||
         currentScreen == SCREEN_COLORS);

    currentScreen = SCREEN_SETTINGS;

    updateSettingsStatus();

    lv_screen_load_anim(
        settingsScreen,
        anim,
        250,
        0,
        freeOld);
}

void goToPictures()
{
    currentScreen = SCREEN_PICTURES;
    pictureDrawPending = false;

    scanPictures();

    if (!sdReady)
    {
        lv_label_set_text(picturesMsgLabel, "No TF card found");
        lv_obj_remove_flag(picturesMsgLabel, LV_OBJ_FLAG_HIDDEN);
    }
    else if (pictureFiles.empty())
    {
        lv_label_set_text(picturesMsgLabel, "No .jpg files on card");
        lv_obj_remove_flag(picturesMsgLabel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(picturesMsgLabel, LV_OBJ_FLAG_HIDDEN);
    }

    lv_screen_load_anim(
        picturesScreen,
        LV_SCR_LOAD_ANIM_OVER_LEFT,
        250,
        0,
        false);
}

// =====================================================
// BUTTON EVENTS
// =====================================================

void homeEvent(lv_event_t *e)
{
    goToHome();
}

void weatherEvent(lv_event_t *e)
{
    goToWeather();
}

void settingsEvent(lv_event_t *e)
{
    goToSettings();
}

void picturesEvent(lv_event_t *e)
{
    goToPictures();
}

// Fires once the slide-in animation has finished
void picturesLoadedEvent(lv_event_t *e)
{
    if (!pictureFiles.empty())
        requestPictureDraw();
}

// =====================================================
// SWIPE GESTURES
// =====================================================

void gestureEvent(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();

    lv_dir_t dir =
        lv_indev_get_gesture_dir(
            indev);

    // A swipe that started on a button must not also "click" it.
    // Gestures are detected while the finger is still down, so:
    //  - drop the button's pressed look, and
    //  - tell LVGL to ignore this touch until the finger lifts,
    //    which stops the CLICKED event that would fire on release.
    if (dir != LV_DIR_NONE)
    {
        lv_obj_t *touched = lv_indev_get_active_obj();

        if (touched)
            lv_obj_remove_state(touched, LV_STATE_PRESSED);

        lv_indev_wait_release(indev);
    }

    switch(currentScreen)
    {
        case SCREEN_HOME:

            if(dir == LV_DIR_LEFT)
                goToWeather();

            break;

        case SCREEN_WEATHER:

            if(dir == LV_DIR_LEFT)
                goToSettings();

            if(dir == LV_DIR_RIGHT)
                goToHome();

            break;

        case SCREEN_SETTINGS:

            if(dir == LV_DIR_RIGHT)
                goToWeather();

            break;

        case SCREEN_WIND:
        case SCREEN_FORECAST:

            if(dir == LV_DIR_RIGHT)
                goBackToWeather();

            break;

        case SCREEN_CITY:

            if(dir == LV_DIR_RIGHT)
                goToSettings();

            break;

        case SCREEN_WIFI:

            // ignore swipes while the password keyboard is open
            if(dir == LV_DIR_RIGHT &&
               lv_obj_has_flag(wifiPassPanel, LV_OBJ_FLAG_HIDDEN))
                goToSettings();

            break;

        case SCREEN_PICTURES:

            if(dir == LV_DIR_TOP)          // swipe up   -> next
                nextPicture();

            if(dir == LV_DIR_BOTTOM)       // swipe down -> previous
                prevPicture();

            if(dir == LV_DIR_LEFT ||
               dir == LV_DIR_RIGHT)        // swipe sideways -> home
                goToHome();

            break;
    }
}

// =====================================================
// PAGE DOTS
// =====================================================

void createPageDots(lv_obj_t *parent)
{
    lv_obj_t *home =
        lv_button_create(parent);

    lv_obj_t *weather =
        lv_button_create(parent);

    lv_obj_t *settings =
        lv_button_create(parent);

    lv_obj_set_size(home,20,20);
    lv_obj_set_size(weather,20,20);
    lv_obj_set_size(settings,20,20);

    // Bigger invisible touch area: each dot now responds 12 px beyond
    // its edge (44 x 44 total). Dots are 45 px apart so the areas
    // don't overlap.
    lv_obj_set_ext_click_area(home, 12);
    lv_obj_set_ext_click_area(weather, 12);
    lv_obj_set_ext_click_area(settings, 12);

    lv_obj_align(
        home,
        LV_ALIGN_BOTTOM_MID,
        -45,
        -8);

    lv_obj_align(
        weather,
        LV_ALIGN_BOTTOM_MID,
        0,
        -8);

    lv_obj_align(
        settings,
        LV_ALIGN_BOTTOM_MID,
        45,
        -8);

    lv_obj_add_event_cb(
        home,
        homeEvent,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_add_event_cb(
        weather,
        weatherEvent,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_add_event_cb(
        settings,
        settingsEvent,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *d1 =
        lv_obj_create(home);

    lv_obj_t *d2 =
        lv_obj_create(weather);

    lv_obj_t *d3 =
        lv_obj_create(settings);

    lv_obj_set_size(d1,12,12);
    lv_obj_set_size(d2,12,12);
    lv_obj_set_size(d3,12,12);

    lv_obj_set_style_radius(
        d1,
        LV_RADIUS_CIRCLE,
        0);

    lv_obj_set_style_radius(
        d2,
        LV_RADIUS_CIRCLE,
        0);

    lv_obj_set_style_radius(
        d3,
        LV_RADIUS_CIRCLE,
        0);

    lv_obj_center(d1);
    lv_obj_center(d2);
    lv_obj_center(d3);
}

// =====================================================
// HOME SCREEN
// =====================================================

void createHomeScreen()
{
    homeScreen =
        lv_obj_create(NULL);

	lv_obj_set_style_bg_color(
		homeScreen,
		lv_color_hex(0x2e2d37),
		0);

    lv_obj_add_event_cb(
        homeScreen,
        gestureEvent,
        LV_EVENT_GESTURE,
        NULL);

    lv_obj_t *title =
        lv_label_create(homeScreen);

    lv_label_set_text(
        title,
        "Dashboard");

	lv_obj_set_style_text_color(
		title,
		lv_color_white(),
		0);

    lv_obj_align(
        title,
        LV_ALIGN_TOP_MID,
        0,
        20);

    lv_obj_t *btn =
        lv_button_create(homeScreen);

    lv_obj_set_size(
        btn,
        180,
        60);

    lv_obj_align(
        btn,
        LV_ALIGN_CENTER,
        0,
        -100);

    lv_obj_add_event_cb(
        btn,
        weatherEvent,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *label =
        lv_label_create(btn);

    lv_label_set_text(
        label,
        "Weather");

    lv_obj_center(label);

    lv_obj_t *btn2 =
        lv_button_create(homeScreen);

    lv_obj_set_size(
        btn2,
        180,
        60);

    lv_obj_align(
        btn2,
        LV_ALIGN_CENTER,
        0,
        0);

    // CHANGED: was weatherEvent
    lv_obj_add_event_cb(
        btn2,
        picturesEvent,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *label2 =
        lv_label_create(btn2);

    lv_label_set_text(
        label2,
        "Pictures");

    lv_obj_center(label2);

    // -----------------------------------
    // SETTINGS BUTTON
    // -----------------------------------

    lv_obj_t *btn3 =
        lv_button_create(homeScreen);

    lv_obj_set_size(
        btn3,
        180,
        60);

    lv_obj_align(
        btn3,
        LV_ALIGN_CENTER,
        0,
        100);

    lv_obj_add_event_cb(
        btn3,
        settingsEvent,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *label3 =
        lv_label_create(btn3);

    lv_label_set_text(
        label3,
        "Settings");

    lv_obj_center(label3);

    // -----------------------------------
    // COUNTDOWN (shown for 10 s after WiFi connects)
    // -----------------------------------

    lv_obj_t *countdown =
        lv_label_create(homeScreen);

    lv_obj_set_style_text_color(
        countdown,
        lv_color_white(),
        0);

    lv_obj_set_style_text_font(
        countdown,
        &lv_font_montserrat_28,
        0);

    lv_obj_align(
        countdown,
        LV_ALIGN_TOP_MID,
        0,
        390);       // between the Settings button and the page dots

    lv_obj_add_flag(countdown, LV_OBJ_FLAG_HIDDEN);

    countdownLabelPtr = countdown;

    createPageDots(homeScreen);
}
    // -----------------------------------
    // WEATHER DOWNLOAD FUNCTION
    // -----------------------------------
bool getWeather()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Weather: WiFi not connected");
        return false;
    }

    HTTPClient http;

    // "City,State,Country" - state is optional (only used for US)
    String place = weatherCity;

    if (weatherState.length() > 0)
        place += "," + weatherState;

    if (weatherCountry.length() > 0)
        place += "," + weatherCountry;

    place.replace(" ", "%20");      // e.g. "Kansas City"

    String url =
        "https://api.openweathermap.org/data/2.5/weather?q=" +
        place +
        "&units=imperial&appid=" +
        String(OWM_API_KEY);

    Serial.println("Downloading weather...");
    Serial.println(url);

    http.begin(url);

    // FIX: don't let a dead internet connection hang the UI for long
    http.setConnectTimeout(3000);
    http.setTimeout(3000);

    int httpCode = http.GET();

    lastWeatherHttpCode = httpCode;

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf(
            "Weather HTTP Error: %d\n",
            httpCode);

        http.end();
        return false;
    }

    String payload =
        http.getString();

    http.end();

    JsonDocument doc;

    DeserializationError error =
        deserializeJson(
            doc,
            payload);

    if (error)
    {
        Serial.println(
            "Weather JSON Error");

        return false;
    }

    currentTemp =
        doc["main"]["temp"]
            .as<float>();

    currentHumidity =
        doc["main"]["humidity"]
            .as<float>();

    // -----------------------------------
    // FEELS LIKE = NWS wind chill
    // (units=imperial, so wind speed is already in mph)
    // -----------------------------------

    float windMph =
        doc["wind"]["speed"]
            .as<float>();

    // for the Wind screen
    currentWindMph = windMph;

    currentWindDeg =
        doc["wind"]["deg"].isNull()
            ? -1
            : doc["wind"]["deg"].as<int>();

    currentWindGust =
        doc["wind"]["gust"].isNull()
            ? 0
            : doc["wind"]["gust"].as<float>();

    // The NWS formula is only valid at or below 50F with wind above 3 mph.
    // Outside that range there is no wind chill, so it equals the air temp.
    if (currentTemp <= 50.0f && windMph > 3.0f)
    {
        float v16 = powf(windMph, 0.16f);

        currentFeelsLike =
            35.74f +
            0.6215f * currentTemp -
            35.75f * v16 +
            0.4275f * currentTemp * v16;
    }
    // -----------------------------------
    // HEAT INDEX (NWS Rothfusz method), 80F and above
    // -----------------------------------
    else if (currentTemp >= 80.0f)
    {
        float T  = currentTemp;
        float RH = currentHumidity;

        // NWS simple formula first
        float hi = 0.5f * (T + 61.0f + (T - 68.0f) * 1.2f + RH * 0.094f);

        // if the result is 80F or more, use the full regression
        if ((hi + T) / 2.0f >= 80.0f)
        {
            hi = -42.379f
                 + 2.04901523f  * T
                 + 10.14333127f * RH
                 - 0.22475541f  * T * RH
                 - 0.00683783f  * T * T
                 - 0.05481717f  * RH * RH
                 + 0.00122874f  * T * T * RH
                 + 0.00085282f  * T * RH * RH
                 - 0.00000199f  * T * T * RH * RH;

            // low-humidity adjustment
            if (RH < 13.0f && T >= 80.0f && T <= 112.0f)
                hi -= ((13.0f - RH) / 4.0f) *
                      sqrtf((17.0f - fabsf(T - 95.0f)) / 17.0f);

            // high-humidity adjustment
            if (RH > 85.0f && T >= 80.0f && T <= 87.0f)
                hi += ((RH - 85.0f) / 10.0f) *
                      ((87.0f - T) / 5.0f);
        }

        // heat index never reads cooler than the actual temperature
        currentFeelsLike = max(hi, T);
    }
    else
    {
        currentFeelsLike = currentTemp;
    }

    Serial.printf(
        "Wind: %.1f mph  Humidity: %.0f%%  Feels like: %.1fF\n",
        windMph,
        currentHumidity,
        currentFeelsLike);

    currentCondition =
        doc["weather"][0]["main"]
            .as<String>();

    weatherPlaceName =
        doc["name"]
            .as<String>();

    // city's current UTC offset -> clock time zone
    if (!doc["timezone"].isNull())
    {
        long tzOffset =
            doc["timezone"]
                .as<long>();

        if (tzOffset != weatherTzOffset)
        {
            weatherTzOffset = tzOffset;
            applyTimeZone(tzOffset);

            // remember it so the clock is right straight after a reboot
            prefs.begin("weather", false);
            prefs.putLong("tz", tzOffset);
            prefs.end();
        }
    }

    // e.g. "01d" = clear day, "10n" = rain at night
    currentSunrise =
        doc["sys"]["sunrise"]
            .as<long>();

    currentSunset =
        doc["sys"]["sunset"]
            .as<long>();

    currentIconCode =
        doc["weather"][0]["icon"]
            .as<String>();

    Serial.printf(
        "Temp: %.1fF  Humidity: %.0f%%  Condition: %s\n",
        currentTemp,
        currentHumidity,
        currentCondition.c_str());

    return true;
}
// =====================================================
// WEATHER ICONS (drawn from simple LVGL shapes)
// =====================================================
// Uses the OpenWeatherMap icon id (e.g. "01d", "10n"):
//   01 clear      02 few clouds   03/04 clouds
//   09/10 rain    11 thunderstorm 13 snow   50 mist/fog
// "d" = day (sun), "n" = night (moon)

#define ICON_W 90
#define ICON_H 70

#define COL_SUN     0xFFC83D
#define COL_MOON    0xE8E4C8
#define COL_CLOUD   0xC8CCD4
#define COL_DARK    0x8A8E9A      // storm cloud
#define COL_RAIN    0x4FA3FF
#define COL_SNOW    0xFFFFFF
#define COL_BOLT    0xFFD23D
#define COL_FOG     0xA0A4AC

// The icon is designed on a 90 x 70 grid. It can be drawn into any
// container at any scale (1.0 = weather screen, 0.6 = forecast rows).
static lv_obj_t *iconParent = nullptr;
static float iconK = 1.0f;
static int iconSlot = 0;          // which bolt-point array to use (0-5)

static lv_obj_t *iconRect(int x, int y, int w, int h,
                          uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(iconParent);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_pos(o, (int)(x * iconK + 0.5f), (int)(y * iconK + 0.5f));
    lv_obj_set_size(o, max(1, (int)(w * iconK + 0.5f)),
                       max(1, (int)(h * iconK + 0.5f)));

    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(
        o,
        radius == LV_RADIUS_CIRCLE ? LV_RADIUS_CIRCLE : (int)(radius * iconK),
        0);

    return o;
}

static lv_obj_t *iconCircle(int cx, int cy, int d, uint32_t color)
{
    return iconRect(cx - d / 2, cy - d / 2, d, d, color, LV_RADIUS_CIRCLE);
}

// sun with 8 dot "rays" around it
static void drawSun(int cx, int cy, int d)
{
    int r = d / 2 + d / 4 + 2;          // ray distance from centre
    int dot = max(4, d / 6);

    for (int i = 0; i < 8; i++)
    {
        float a = i * PI / 4.0f;
        iconCircle(cx + (int)(r * cosf(a)),
                   cy + (int)(r * sinf(a)),
                   dot, COL_SUN);
    }

    iconCircle(cx, cy, d, COL_SUN);
}

// crescent moon: light disc with a background-coloured disc over it
static void drawMoon(int cx, int cy, int d)
{
    iconCircle(cx, cy, d, COL_MOON);
    iconCircle(cx + d / 3, cy - d / 4, d * 4 / 5, weatherBgColor);
}

// cloud is 60 x 42, top-left at (x, y)
static void drawCloud(int x, int y, uint32_t color)
{
    iconRect(x, y + 20, 60, 22, color, 11);     // flat base
    iconCircle(x + 19, y + 23, 26, color);      // left puff
    iconCircle(x + 37, y + 17, 34, color);      // big puff
}

// lightning bolt on the 90 x 70 grid
static const lv_point_precise_t boltBase[4] =
{
    {47, 44}, {39, 55}, {48, 55}, {41, 67}
};

// scaled copies; points must stay in memory, one array per icon slot
static lv_point_precise_t boltPts[6][4];

// draw an icon into any container at scale k
void drawIconInto(lv_obj_t *parent, const String &code, float k, int slot)
{
    if (!parent)
        return;

    iconParent = parent;
    iconK = k;
    iconSlot = constrain(slot, 0, 5);

    lv_obj_clean(parent);

    String id = code.length() >= 2 ? code.substring(0, 2) : "";
    bool night = code.endsWith("n");

    const int cx = ICON_W / 2;
    const int cy = ICON_H / 2;

    if (id == "01")                                 // clear
    {
        if (night) drawMoon(cx, cy, 40);
        else       drawSun(cx, cy, 36);
    }
    else if (id == "02")                            // few clouds
    {
        if (night) drawMoon(30, 24, 30);
        else       drawSun(30, 24, 26);

        drawCloud(26, 26, COL_CLOUD);
    }
    else if (id == "03" || id == "04")              // cloudy
    {
        drawCloud(15, 14, COL_CLOUD);
    }
    else if (id == "09" || id == "10")              // rain
    {
        drawCloud(15, 4, COL_CLOUD);

        for (int i = 0; i < 3; i++)
            iconRect(26 + i * 16, 52 + (i % 2) * 5, 4, 12, COL_RAIN, 2);
    }
    else if (id == "11")                            // thunderstorm
    {
        drawCloud(15, 4, COL_DARK);

        lv_obj_t *bolt = lv_line_create(iconParent);

        for (int i = 0; i < 4; i++)
        {
            boltPts[iconSlot][i].x = boltBase[i].x * iconK;
            boltPts[iconSlot][i].y = boltBase[i].y * iconK;
        }

        lv_line_set_points(bolt, boltPts[iconSlot], 4);

        lv_obj_set_style_line_color(bolt, lv_color_hex(COL_BOLT), 0);
        lv_obj_set_style_line_width(bolt, max(2, (int)(4 * iconK)), 0);
        lv_obj_set_style_line_rounded(bolt, true, 0);
        lv_obj_remove_flag(bolt, LV_OBJ_FLAG_CLICKABLE);
    }
    else if (id == "13")                            // snow
    {
        drawCloud(15, 4, COL_CLOUD);

        iconCircle(28, 56, 7, COL_SNOW);
        iconCircle(45, 63, 7, COL_SNOW);
        iconCircle(62, 56, 7, COL_SNOW);
    }
    else if (id == "50")                            // mist / fog / haze
    {
        for (int i = 0; i < 4; i++)
            iconRect(15 + (i % 2) * 8, 14 + i * 12, 52, 6, COL_FOG, 3);
    }
    else                                            // not loaded yet
    {
        drawCloud(15, 14, COL_FOG);
    }
}

// the big icon on the weather screen
void drawWeatherIcon(const String &code)
{
    drawIconInto(weatherIcon, code, 1.0f, 0);
}

// =====================================================
// WEATHER SCREEN
// =====================================================

void createWeatherScreen()
{
    weatherScreen =
        lv_obj_create(NULL);

	lv_obj_set_style_bg_color(
		weatherScreen,
		lv_color_hex(0x2e2d37),
		0);

	lv_obj_set_style_bg_opa(
		weatherScreen,
		LV_OPA_COVER,
		0);

    lv_obj_add_event_cb(
        weatherScreen,
        gestureEvent,
        LV_EVENT_GESTURE,
        NULL);

    // -----------------------------------
    // WEATHER ICON (drawn shapes, 90 x 70)
    // -----------------------------------

    weatherIcon =
        lv_obj_create(weatherScreen);

    lv_obj_remove_style_all(weatherIcon);       // transparent, no border/padding

    lv_obj_set_size(
        weatherIcon,
        ICON_W,
        ICON_H);

    lv_obj_remove_flag(weatherIcon, LV_OBJ_FLAG_SCROLLABLE);
    // NEW: the icon is a button that opens the Wind screen.
    // (Swipes that start on it still change page - gestureEvent()
    //  cancels the click when a swipe is detected.)
    lv_obj_add_flag(weatherIcon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(weatherIcon, 10);

    // single tap -> Wind screen, double tap -> 5-day forecast
    lv_obj_add_event_cb(
        weatherIcon,
        weatherIconClicked,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_align(
        weatherIcon,
        LV_ALIGN_TOP_MID,
        0,
        260);

    drawWeatherIcon(currentIconCode);

    // (greeting removed)

    // -----------------------------------
    // TIME
    // -----------------------------------

    timeLabel =
        lv_label_create(weatherScreen);

    lv_label_set_text(
        timeLabel,
        "10:42 PM");

	lv_obj_set_style_text_color(
		timeLabel,
		lv_color_hex(0xa0a0a0),
		0);

    lv_obj_set_style_text_font(
        timeLabel,
        &lv_font_montserrat_48,
        0);

    lv_obj_align(
        timeLabel,
        LV_ALIGN_TOP_MID,
        0,
        15);

    // -----------------------------------
    // DATE
    // -----------------------------------

    dateLabel =
        lv_label_create(weatherScreen);

    lv_label_set_text(
        dateLabel,
        "November 2, 2026");

	lv_obj_set_style_text_color(
		dateLabel,
		lv_color_hex(0xa0a0a0),
		0);

    lv_obj_set_style_text_font(
        dateLabel,
        &lv_font_montserrat_28,
        0);
    lv_obj_align(
        dateLabel,
        LV_ALIGN_TOP_MID,
        0,
        72);


    // -----------------------------------
    // TEMPERATURE
    // -----------------------------------

    tempLabel =
        lv_label_create(weatherScreen);

    lv_obj_set_style_text_font(
        tempLabel,
        &lv_font_montserrat_48,
        0);

	lv_obj_set_style_text_color(
		tempLabel,
		lv_color_hex(0xa0a0a0),
		0);

    lv_label_set_text(
        tempLabel,
        "--F");

    lv_obj_align(
        tempLabel,
        LV_ALIGN_TOP_MID,
        0,
        160);

    // -----------------------------------
    // FEELS LIKE (below temperature)
    // -----------------------------------

    feelsLikeLabel =
        lv_label_create(weatherScreen);

    lv_obj_set_style_text_font(
        feelsLikeLabel,
        &lv_font_montserrat_28,
        0);

    lv_obj_set_style_text_color(
        feelsLikeLabel,
        lv_color_hex(0xa0a0a0),     // same grey as the other labels
        0);

    lv_label_set_text(
        feelsLikeLabel,
        "Feels like --F");

    lv_obj_align(
        feelsLikeLabel,
        LV_ALIGN_TOP_MID,
        0,
        214);

    // -----------------------------------
    // CITY NAME (was condition)
    // -----------------------------------

    cityNameLabel =
        lv_label_create(weatherScreen);

	lv_obj_set_style_text_color(
		cityNameLabel,
		lv_color_hex(0xa0a0a0),
		0);

    lv_label_set_text(
        cityNameLabel,
        cityWithState().c_str());  // e.g. "Belton, MO"

    // long names get "..." instead of wrapping onto the humidity line
    lv_obj_set_width(cityNameLabel, 300);
    lv_label_set_long_mode(cityNameLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(cityNameLabel, LV_TEXT_ALIGN_CENTER, 0);

	lv_obj_set_style_text_font(
        cityNameLabel,
        &lv_font_montserrat_28,
        0);

    lv_obj_align(
        cityNameLabel,
        LV_ALIGN_TOP_MID,
        0,
        115);

    // -----------------------------------
    // HUMIDITY
    // -----------------------------------

    humidityLabel =
        lv_label_create(weatherScreen);

	lv_obj_set_style_text_color(
		humidityLabel,
		lv_color_hex(0xa0a0a0),
		0);

    lv_label_set_text(
        humidityLabel,
        "Humidity --%");

    lv_obj_set_style_text_font(
        humidityLabel,
        &lv_font_montserrat_28,
        0);

    lv_obj_align(
        humidityLabel,
        LV_ALIGN_TOP_MID,
        0,
        340);

    // -----------------------------------
    // SUNRISE / SUNSET (14 pt, below humidity)
    // -----------------------------------

    sunriseLabel =
        lv_label_create(weatherScreen);

    lv_obj_set_style_text_font(
        sunriseLabel,
        &lv_font_montserrat_14,
        0);

    lv_obj_set_style_text_color(
        sunriseLabel,
        lv_color_hex(0xa0a0a0),
        0);

    lv_label_set_text(
        sunriseLabel,
        "Sunrise --:--");

    lv_obj_align(
        sunriseLabel,
        LV_ALIGN_TOP_MID,
        0,
        385);

    sunsetLabel =
        lv_label_create(weatherScreen);

    lv_obj_set_style_text_font(
        sunsetLabel,
        &lv_font_montserrat_14,
        0);

    lv_obj_set_style_text_color(
        sunsetLabel,
        lv_color_hex(0xa0a0a0),
        0);

    lv_label_set_text(
        sunsetLabel,
        "Sunset --:--");

    lv_obj_align(
        sunsetLabel,
        LV_ALIGN_TOP_MID,
        0,
        407);

    // OpenWeather credit, scrolling between the sunset line and the dots
    weatherCredit = addCreditTicker(weatherScreen, 430, weatherTextColor);

    createPageDots(weatherScreen);
}
// =====================================================
// SETTINGS SCREEN
// =====================================================

void createSettingsScreen()
{
    settingsScreen =
        lv_obj_create(NULL);

	lv_obj_set_style_bg_color(
		settingsScreen,
		lv_color_hex(0x2e2d37),
		0);

    lv_obj_add_event_cb(
        settingsScreen,
        gestureEvent,
        LV_EVENT_GESTURE,
        NULL);

    lv_obj_t *title =
        lv_label_create(settingsScreen);

    lv_label_set_text(
        title,
        "Settings");

	lv_obj_set_style_text_color(
		title,
		lv_color_white(),
		0);


    lv_obj_align(
        title,
        LV_ALIGN_TOP_MID,
        0,
        20);

    lv_obj_t *wifiBtn =
        lv_button_create(settingsScreen);

    lv_obj_set_size(
        wifiBtn,
        200,
        50);

    lv_obj_align(
        wifiBtn,
        LV_ALIGN_TOP_MID,
        0,
        45);       // was 80

    lv_obj_t *wifiLbl =
        lv_label_create(wifiBtn);

    lv_label_set_text(
        wifiLbl,
        LV_SYMBOL_WIFI "  WiFi Setup");

    lv_obj_center(wifiLbl);

    // NEW: open the WiFi setup screen
    lv_obj_add_event_cb(
        wifiBtn,
        [](lv_event_t *e) { goToWifiSetup(); },
        LV_EVENT_CLICKED,
        NULL);

    // -----------------------------------
    // CITY BUTTON (below WiFi Setup)
    // -----------------------------------

    lv_obj_t *cityBtn =
        lv_button_create(settingsScreen);

    lv_obj_set_size(
        cityBtn,
        200,
        50);

    lv_obj_align(
        cityBtn,
        LV_ALIGN_TOP_MID,
        0,
        103);       // was 140

    lv_obj_t *cityLbl =
        lv_label_create(cityBtn);

    lv_label_set_text(
        cityLbl,
        LV_SYMBOL_GPS "  City");

    lv_obj_center(cityLbl);

    lv_obj_add_event_cb(
        cityBtn,
        [](lv_event_t *e) { goToCity(); },
        LV_EVENT_CLICKED,
        NULL);

    // -----------------------------------
    // WEATHER PAGE COLOR BUTTON (below City)
    // -----------------------------------

    lv_obj_t *colorBtn =
        lv_button_create(settingsScreen);

    lv_obj_set_size(
        colorBtn,
        200,
        50);

    lv_obj_align(
        colorBtn,
        LV_ALIGN_TOP_MID,
        0,
        161);       // was 200

    lv_obj_t *colorLbl =
        lv_label_create(colorBtn);

    lv_label_set_text(
        colorLbl,
        LV_SYMBOL_EDIT "  Weather Page Color");

    lv_obj_center(colorLbl);

    lv_obj_add_event_cb(
        colorBtn,
        [](lv_event_t *e) { goToWeatherColors(); },
        LV_EVENT_CLICKED,
        NULL);

    // NEW: shows the current connection
    settingsStatusLabel =
        lv_label_create(settingsScreen);

    lv_obj_set_width(settingsStatusLabel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(settingsStatusLabel, 190, 0);

    lv_obj_set_style_text_align(
        settingsStatusLabel,
        LV_TEXT_ALIGN_LEFT,
        0);

    lv_obj_set_style_text_color(
        settingsStatusLabel,
        lv_color_hex(0xa0a0a0),
        0);

    lv_obj_align(
        settingsStatusLabel,
        LV_ALIGN_TOP_LEFT,
        140,        // 25 px gap right of the Auto Dim button (ends at x=115)
        352);

    updateSettingsStatus();

    // -----------------------------------
    // BRIGHTNESS SLIDER (below Weather Page Color)
    // -----------------------------------

    static lv_obj_t *brightLabel;

    brightLabel =
        lv_label_create(settingsScreen);

    lv_obj_set_style_text_color(
        brightLabel,
        lv_color_white(),
        0);

    lv_label_set_text_fmt(
        brightLabel,
        LV_SYMBOL_IMAGE "  Brightness  %d%%",
        brightnessPercent);

    lv_obj_align(
        brightLabel,
        LV_ALIGN_TOP_MID,
        0,
        225);       // was 270

    lv_obj_t *brightSlider =
        lv_slider_create(settingsScreen);

    lv_obj_set_width(brightSlider, 240);

    lv_slider_set_range(
        brightSlider,
        BL_MIN_PERCENT,
        100);

    lv_slider_set_value(
        brightSlider,
        brightnessPercent,
        LV_ANIM_OFF);

    lv_obj_align(
        brightSlider,
        LV_ALIGN_TOP_MID,
        0,
        255);       // was 310

    brightSliderObj = brightSlider;
    brightLabelObj = brightLabel;

    // a little extra touch area around the thin slider
    lv_obj_set_ext_click_area(brightSlider, 15);

    // dragging the slider sideways shouldn't be mistaken for a page swipe
    lv_obj_remove_flag(brightSlider, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // live dimming while dragging
    lv_obj_add_event_cb(
        brightSlider,
        [](lv_event_t *e)
        {
            lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
            int v = lv_slider_get_value(s);

            setBacklight(v);

            lv_label_set_text_fmt(
                brightLabel,
                LV_SYMBOL_IMAGE "  Brightness  %d%%",
                v);
        },
        LV_EVENT_VALUE_CHANGED,
        NULL);

    // save to flash only when the finger lifts
    lv_obj_add_event_cb(
        brightSlider,
        [](lv_event_t *e) { saveBrightness(); },
        LV_EVENT_RELEASED,
        NULL);

    // -----------------------------------
    // AUTO DIM TOGGLE (below slider, left of WiFi status)
    // -----------------------------------

    autoDimBtn =
        lv_button_create(settingsScreen);

    lv_obj_set_size(
        autoDimBtn,
        95,
        40);

    lv_obj_align(
        autoDimBtn,
        LV_ALIGN_TOP_LEFT,
        20,
        352);

    lv_obj_add_flag(autoDimBtn, LV_OBJ_FLAG_CHECKABLE);    // on/off toggle

    lv_obj_t *autoDimLbl =
        lv_label_create(autoDimBtn);

    lv_label_set_text(
        autoDimLbl,
        "Auto Dim");

    lv_obj_center(autoDimLbl);

    lv_obj_add_event_cb(
        autoDimBtn,
        [](lv_event_t *e)
        {
            setAutoDim(lv_obj_has_state(autoDimBtn, LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED,
        NULL);

    // -----------------------------------
    // CENTRE "Auto Dim" + WiFi status as a pair
    // -----------------------------------
    // A full-width row with SPACE_EVENLY gives the same gap at the left
    // edge, between the two, and at the right edge - and it re-balances
    // automatically when the WiFi text changes length.

    lv_obj_t *autoDimRow =
        lv_obj_create(settingsScreen);

    lv_obj_remove_style_all(autoDimRow);            // transparent, no padding
    lv_obj_set_size(autoDimRow, 320, LV_SIZE_CONTENT);
    lv_obj_align(autoDimRow, LV_ALIGN_TOP_LEFT, 0, 280);     // was 350

    lv_obj_remove_flag(autoDimRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(autoDimRow, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_flex_flow(autoDimRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        autoDimRow,
        LV_FLEX_ALIGN_SPACE_EVENLY,     // equal gaps: edge | button | text | edge
        LV_FLEX_ALIGN_CENTER,           // vertically centred on each other
        LV_FLEX_ALIGN_CENTER);

    lv_obj_set_parent(autoDimBtn, autoDimRow);
    lv_obj_set_parent(settingsStatusLabel, autoDimRow);

    // -----------------------------------
    // AUTO DIM LIMITS: two-handle range slider
    //   left handle  = night (low) brightness
    //   right handle = day (high) brightness
    // -----------------------------------

    dimRangeLabel =
        lv_label_create(settingsScreen);

    lv_obj_set_style_text_color(
        dimRangeLabel,
        lv_color_white(),
        0);

    lv_obj_align(
        dimRangeLabel,
        LV_ALIGN_TOP_MID,
        0,
        350);

    dimRangeSlider =
        lv_slider_create(settingsScreen);

    lv_slider_set_mode(dimRangeSlider, LV_SLIDER_MODE_RANGE);

    lv_obj_set_width(dimRangeSlider, 240);

    lv_slider_set_range(
        dimRangeSlider,
        BL_MIN_PERCENT,
        100);

    lv_obj_align(
        dimRangeSlider,
        LV_ALIGN_TOP_MID,
        0,
        382);

    lv_obj_set_ext_click_area(dimRangeSlider, 15);

    // sideways drags adjust the limits, they don't change page
    lv_obj_remove_flag(dimRangeSlider, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // live update while dragging either handle
    lv_obj_add_event_cb(
        dimRangeSlider,
        [](lv_event_t *e)
        {
            autoDimLow  = lv_slider_get_left_value(dimRangeSlider);
            autoDimHigh = lv_slider_get_value(dimRangeSlider);

            updateDimRangeLabel();

            // if Auto Dim is running, show the effect straight away
            if (autoDimEnabled && autoDimPhase != -1)
                showBrightness(autoDimPhase ? autoDimHigh : autoDimLow);
        },
        LV_EVENT_VALUE_CHANGED,
        NULL);

    // save to flash only when the finger lifts
    lv_obj_add_event_cb(
        dimRangeSlider,
        [](lv_event_t *e) { saveDimLimits(); },
        LV_EVENT_RELEASED,
        NULL);

    createPageDots(settingsScreen);
}

// =====================================================
// PICTURES SCREEN
// =====================================================
// The LVGL screen is just a black, gesture-catching surface.
// The JPEG itself is drawn straight to the panel in loop().

void createPicturesScreen()
{
    picturesScreen =
        lv_obj_create(NULL);

    lv_obj_set_style_bg_color(
        picturesScreen,
        lv_color_black(),
        0);

    lv_obj_set_style_bg_opa(
        picturesScreen,
        LV_OPA_COVER,
        0);

    // no scrolling, so vertical swipes become gestures
    lv_obj_remove_flag(
        picturesScreen,
        LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(
        picturesScreen,
        gestureEvent,
        LV_EVENT_GESTURE,
        NULL);

    lv_obj_add_event_cb(
        picturesScreen,
        picturesLoadedEvent,
        LV_EVENT_SCREEN_LOADED,
        NULL);

    picturesMsgLabel =
        lv_label_create(picturesScreen);

    lv_label_set_text(
        picturesMsgLabel,
        "Loading...");

    lv_obj_set_style_text_color(
        picturesMsgLabel,
        lv_color_hex(0xa0a0a0),
        0);

    lv_obj_set_style_text_font(
        picturesMsgLabel,
        &lv_font_montserrat_28,
        0);

    lv_obj_center(picturesMsgLabel);
}

// =====================================================
// WIFI FUNCTION
// =====================================================
bool connectWiFi(
    const char* ssid,
    const char* password,
    uint32_t timeoutMs = 10000)
{
    Serial.println();
    Serial.println("Connecting WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    uint32_t startTime = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - startTime > timeoutMs)
        {
            Serial.println();
            Serial.println("WiFi connection timeout");

            return false;
        }

        delay(250);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi Connected");

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());

    return true;
}

// =====================================================
// NTP
// =====================================================

void startNTP()
{
    Serial.println("Starting NTP...");

    // NTP always works in UTC; the local zone comes from the weather city
    configTime(
        0,
        0,
        "pool.ntp.org",
        "time.nist.gov");

    applyTimeZone(weatherTzOffset);

    Serial.println("NTP started");
}
bool checkTimeSync()
{
    struct tm timeinfo;

    if(getLocalTime(&timeinfo, 100))
    {
        if(!timeSynchronized)
        {
            Serial.println("Time synchronized");
            timeSynchronized = true;
        }

        return true;
    }

    return false;
}

// =====================================================
// WIFI CREDENTIAL STORAGE (flash)
// =====================================================

void loadWifiCredentials()
{
    prefs.begin("wifi", true);
    savedSSID = prefs.getString("ssid", DEFAULT_WIFI_SSID);
    savedPass = prefs.getString("pass", DEFAULT_WIFI_PASS);
    prefs.end();

    Serial.printf("Saved WiFi: %s\n", savedSSID.c_str());
}

void saveWifiCredentials(const String &ssid, const String &pass)
{
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();

    savedSSID = ssid;
    savedPass = pass;
}

// =====================================================
// WIFI SETUP - STATUS TEXT
// =====================================================

void updateSettingsStatus()
{
    if (!settingsStatusLabel)
        return;

    if (WiFi.status() == WL_CONNECTED)
    {
        String s = "Connected to\n" + WiFi.SSID() +
                   "\n" + WiFi.localIP().toString();

        lv_label_set_text(settingsStatusLabel, s.c_str());
    }
    else if (wifiConnecting)
    {
        String s = "Connecting to\n" + pendingSSID + "...";

        lv_label_set_text(settingsStatusLabel, s.c_str());
    }
    else
    {
        lv_label_set_text(settingsStatusLabel, "Not connected");
    }
}

void setWifiStatus(const String &msg)
{
    if (wifiStatusLabel)
        lv_label_set_text(wifiStatusLabel, msg.c_str());
}

// =====================================================
// WIFI SETUP - SCAN
// =====================================================

void startWifiScan()
{
    if (wifiScanRunning || wifiConnecting)
        return;

    // A failed/pending connection blocks scanning, so stop it first
    if (WiFi.status() != WL_CONNECTED)
        WiFi.disconnect();

    WiFi.scanDelete();

    if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED)
    {
        setWifiStatus("Scan failed - tap Scan again");
        return;
    }

    wifiScanRunning = true;

    lv_obj_clean(wifiList);
    lv_list_add_text(wifiList, "Scanning...");

    setWifiStatus("Scanning for networks...");
}

void wifiNetSelected(lv_event_t *e);

void showScanResults()
{
    lv_obj_clean(wifiList);

    if (scannedNets.empty())
    {
        lv_list_add_text(wifiList, "No networks found");
        return;
    }

    for (size_t i = 0; i < scannedNets.size(); i++)
    {
        const WifiNet &n = scannedNets[i];

        String text = n.ssid;

        // simple signal strength bars
        int bars = n.rssi > -55 ? 4 :
                   n.rssi > -65 ? 3 :
                   n.rssi > -75 ? 2 : 1;

        text += "   ";
        for (int b = 0; b < 4; b++)
            text += (b < bars) ? "|" : ".";

        if (n.open)
            text += "  (open)";

        if (n.ssid == WiFi.SSID() && WiFi.status() == WL_CONNECTED)
            text += "  " LV_SYMBOL_OK;

        lv_obj_t *btn =
            lv_list_add_button(
                wifiList,
                LV_SYMBOL_WIFI,
                text.c_str());

        lv_obj_add_event_cb(
            btn,
            wifiNetSelected,
            LV_EVENT_CLICKED,
            (void *)(intptr_t)i);
    }
}

// call from loop()
void pollWifiScan()
{
    if (!wifiScanRunning)
        return;

    int16_t n = WiFi.scanComplete();

    if (n == WIFI_SCAN_RUNNING)
        return;

    wifiScanRunning = false;

    scannedNets.clear();

    if (n < 0)
    {
        setWifiStatus("Scan failed - tap Scan again");
        showScanResults();
        return;
    }

    for (int i = 0; i < n; i++)
    {
        String ssid = WiFi.SSID(i);

        if (ssid.length() == 0)       // hidden network
            continue;

        // keep only the strongest entry for each name
        bool dup = false;
        for (auto &net : scannedNets)
        {
            if (net.ssid == ssid)
            {
                dup = true;
                if (WiFi.RSSI(i) > net.rssi)
                    net.rssi = WiFi.RSSI(i);
                break;
            }
        }

        if (!dup)
        {
            scannedNets.push_back({
                ssid,
                WiFi.RSSI(i),
                WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
        }
    }

    WiFi.scanDelete();

    std::sort(
        scannedNets.begin(),
        scannedNets.end(),
        [](const WifiNet &a, const WifiNet &b) { return a.rssi > b.rssi; });

    String msg = String(scannedNets.size()) + " networks found - tap one";

    // explain why we're here if the startup connection failed
    if (bootFailedSSID.length() > 0)
        msg = "Couldn't connect to " + bootFailedSSID + "\n" + msg;

    setWifiStatus(msg);
    showScanResults();
}

// =====================================================
// WIFI SETUP - CONNECT
// =====================================================

void beginWifiConnect(const String &ssid, const String &pass)
{
    pendingSSID = ssid;
    pendingPass = pass;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    WiFi.begin(ssid.c_str(), pass.c_str());

    wifiConnecting = true;
    wifiConnectStart = millis();

    updateSettingsStatus();

    lv_obj_add_state(wifiScanBtn, LV_STATE_DISABLED);

    setWifiStatus("Connecting to " + ssid + "...");
    Serial.printf("Connecting to %s\n", ssid.c_str());
}

// call from loop()
void pollWifiConnect()
{
    if (!wifiConnecting)
        return;

    wl_status_t st = WiFi.status();

    if (st == WL_CONNECTED)
    {
        wifiConnecting = false;
        wifiConnected = true;
        wifiBootAttempt = false;
        bootFailedSSID = "";

        // show the weather screen 10 s from now
        weatherSwitchPending = true;
        wifiConnectedAt = millis();

        // only write to flash if something actually changed
        if (pendingSSID != savedSSID || pendingPass != savedPass)
            saveWifiCredentials(pendingSSID, pendingPass);

        setWifiStatus("Connected to " + pendingSSID +
                      "\nIP " + WiFi.localIP().toString());

        Serial.printf("Connected, IP %s\n", WiFi.localIP().toString().c_str());

        lv_obj_remove_state(wifiScanBtn, LV_STATE_DISABLED);

        startNTP();

        // fetch weather on the next pass through loop()
        lastWeatherUpdate = millis() - weatherInterval - 1;

        updateSettingsStatus();
        showScanResults();   // refresh the check mark
        return;
    }

    bool failed =
        (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT) ||
        (millis() - wifiConnectStart > 3000 &&
         (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL));

    if (failed)
    {
        wifiConnecting = false;
        wifiConnected = false;

        WiFi.disconnect();

        setWifiStatus("Could not connect to " + pendingSSID +
                      "\nCheck the password and try again");

        Serial.println("WiFi connect failed");

        lv_obj_remove_state(wifiScanBtn, LV_STATE_DISABLED);

        updateSettingsStatus();

        // startup connection failed -> open WiFi Setup with a fresh scan
        if (wifiBootAttempt)
        {
            wifiBootAttempt = false;

            // remove the "Connecting to WiFi" message from the Dashboard
            if (countdownLabelPtr)
            {
                stopThrob((lv_obj_t *)countdownLabelPtr);
                lv_obj_add_flag((lv_obj_t *)countdownLabelPtr, LV_OBJ_FLAG_HIDDEN);
            }
            bootFailedSSID = pendingSSID;

            if (currentScreen == SCREEN_WIFI)
                startWifiScan();
            else
                goToWifiSetup();
        }
    }
}

// =====================================================
// WIFI SETUP - PASSWORD PANEL
// =====================================================

void openPasswordPanel(const String &ssid)
{
    pendingSSID = ssid;

    String title = "Password for\n" + ssid;
    lv_label_set_text(wifiPassTitle, title.c_str());

    // pre-fill if this is the network we already know
    lv_textarea_set_text(
        wifiPassTA,
        (ssid == savedSSID) ? savedPass.c_str() : "");

    lv_obj_remove_flag(wifiPassPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifiPassPanel);
}

void closePasswordPanel()
{
    lv_obj_add_flag(wifiPassPanel, LV_OBJ_FLAG_HIDDEN);
}

void wifiNetSelected(lv_event_t *e)
{
    if (wifiConnecting)
        return;

    int i = (int)(intptr_t)lv_event_get_user_data(e);

    if (i < 0 || i >= (int)scannedNets.size())
        return;

    const WifiNet &n = scannedNets[i];

    if (n.open)
        beginWifiConnect(n.ssid, "");
    else
        openPasswordPanel(n.ssid);
}

void submitPassword()
{
    String pass = lv_textarea_get_text(wifiPassTA);

    if (pass.length() < 8)
    {
        lv_label_set_text(
            wifiPassTitle,
            "Password must be at least\n8 characters");
        return;
    }

    closePasswordPanel();
    beginWifiConnect(pendingSSID, pass);
}

void keyboardEvent(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY)          // keyboard "OK" key
        submitPassword();

    if (code == LV_EVENT_CANCEL)         // keyboard "close" key
        closePasswordPanel();
}

void showPassEvent(lv_event_t *e)
{
    lv_obj_t *cb = (lv_obj_t *)lv_event_get_target(e);

    bool show = lv_obj_has_state(cb, LV_STATE_CHECKED);

    lv_textarea_set_password_mode(wifiPassTA, !show);
}

// =====================================================
// WIFI SETUP SCREEN
// =====================================================

void createWifiScreen()
{
    wifiScreen =
        lv_obj_create(NULL);

    lv_obj_set_style_bg_color(
        wifiScreen,
        lv_color_hex(0x2e2d37),
        0);

    lv_obj_remove_flag(
        wifiScreen,
        LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(
        wifiScreen,
        gestureEvent,
        LV_EVENT_GESTURE,
        NULL);

    // ---------- title ----------

    lv_obj_t *title =
        lv_label_create(wifiScreen);

    lv_label_set_text(title, "WiFi Setup");

    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // ---------- back button ----------

    lv_obj_t *backBtn =
        lv_button_create(wifiScreen);

    lv_obj_set_size(backBtn, 90, 40);

    lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 10, 55);

    lv_obj_add_event_cb(
        backBtn,
        [](lv_event_t *e) { goToSettings(); },
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(backLbl);

    // ---------- scan button ----------

    wifiScanBtn =
        lv_button_create(wifiScreen);

    lv_obj_set_size(wifiScanBtn, 120, 40);

    lv_obj_align(wifiScanBtn, LV_ALIGN_TOP_RIGHT, -10, 55);

    lv_obj_add_event_cb(
        wifiScanBtn,
        [](lv_event_t *e) { startWifiScan(); },
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *scanLbl = lv_label_create(wifiScanBtn);
    lv_label_set_text(scanLbl, LV_SYMBOL_REFRESH " Scan");
    lv_obj_center(scanLbl);

    // ---------- status ----------

    wifiStatusLabel =
        lv_label_create(wifiScreen);

    lv_obj_set_width(wifiStatusLabel, 300);

    lv_obj_set_style_text_align(
        wifiStatusLabel,
        LV_TEXT_ALIGN_CENTER,
        0);

    lv_obj_set_style_text_color(
        wifiStatusLabel,
        lv_color_hex(0xa0a0a0),
        0);

    lv_label_set_text(wifiStatusLabel, "Tap Scan to find networks");

    lv_obj_align(wifiStatusLabel, LV_ALIGN_TOP_MID, 0, 105);

    // ---------- network list ----------

    wifiList =
        lv_list_create(wifiScreen);

    lv_obj_set_size(wifiList, 300, 330);

    lv_obj_align(wifiList, LV_ALIGN_TOP_MID, 0, 142);

    // ---------- password panel (hidden overlay) ----------

    wifiPassPanel =
        lv_obj_create(wifiScreen);

    lv_obj_set_size(wifiPassPanel, 320, 480);
    lv_obj_set_pos(wifiPassPanel, 0, 0);

    lv_obj_set_style_bg_color(wifiPassPanel, lv_color_hex(0x2e2d37), 0);
    lv_obj_set_style_bg_opa(wifiPassPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifiPassPanel, 0, 0);
    lv_obj_set_style_radius(wifiPassPanel, 0, 0);
    lv_obj_set_style_pad_all(wifiPassPanel, 0, 0);

    lv_obj_remove_flag(wifiPassPanel, LV_OBJ_FLAG_SCROLLABLE);

    wifiPassTitle =
        lv_label_create(wifiPassPanel);

    lv_obj_set_width(wifiPassTitle, 300);

    lv_obj_set_style_text_align(wifiPassTitle, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_set_style_text_color(wifiPassTitle, lv_color_white(), 0);

    lv_obj_align(wifiPassTitle, LV_ALIGN_TOP_MID, 0, 12);

    wifiPassTA =
        lv_textarea_create(wifiPassPanel);

    lv_textarea_set_one_line(wifiPassTA, true);
    lv_textarea_set_password_mode(wifiPassTA, true);
    lv_textarea_set_max_length(wifiPassTA, 63);
    lv_textarea_set_placeholder_text(wifiPassTA, "Password");

    lv_obj_set_width(wifiPassTA, 300);

    lv_obj_align(wifiPassTA, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_add_state(wifiPassTA, LV_STATE_FOCUSED);   // show the cursor

    lv_obj_t *showCb =
        lv_checkbox_create(wifiPassPanel);

    lv_checkbox_set_text(showCb, "Show");

    lv_obj_set_style_text_color(showCb, lv_color_white(), 0);

    lv_obj_align(showCb, LV_ALIGN_TOP_LEFT, 12, 120);

    lv_obj_add_event_cb(
        showCb,
        showPassEvent,
        LV_EVENT_VALUE_CHANGED,
        NULL);

    lv_obj_t *cancelBtn =
        lv_button_create(wifiPassPanel);

    lv_obj_set_size(cancelBtn, 90, 40);

    lv_obj_align(cancelBtn, LV_ALIGN_TOP_RIGHT, -112, 112);

    lv_obj_add_event_cb(
        cancelBtn,
        [](lv_event_t *e) { closePasswordPanel(); },
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *cancelLbl = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLbl, "Cancel");
    lv_obj_center(cancelLbl);

    lv_obj_t *connectBtn =
        lv_button_create(wifiPassPanel);

    lv_obj_set_size(connectBtn, 95, 40);

    lv_obj_align(connectBtn, LV_ALIGN_TOP_RIGHT, -10, 112);

    lv_obj_add_event_cb(
        connectBtn,
        [](lv_event_t *e) { submitPassword(); },
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *connectLbl = lv_label_create(connectBtn);
    lv_label_set_text(connectLbl, "Connect");
    lv_obj_center(connectLbl);

    wifiKeyboard =
        lv_keyboard_create(wifiPassPanel);

    lv_obj_set_size(wifiKeyboard, 320, 300);

    lv_obj_align(wifiKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_keyboard_set_textarea(wifiKeyboard, wifiPassTA);

    lv_obj_add_event_cb(wifiKeyboard, keyboardEvent, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(wifiKeyboard, keyboardEvent, LV_EVENT_CANCEL, NULL);

    lv_obj_add_flag(wifiPassPanel, LV_OBJ_FLAG_HIDDEN);
}

void goToWifiSetup()
{
    currentScreen = SCREEN_WIFI;

    closePasswordPanel();

    lv_screen_load_anim(
        wifiScreen,
        LV_SCR_LOAD_ANIM_OVER_LEFT,
        250,
        0,
        false);

    // start scanning straight away
    startWifiScan();
}

// =====================================================
// CITY SCREEN
// =====================================================

lv_obj_t *cityTA = nullptr;
lv_obj_t *stateTA = nullptr;
lv_obj_t *countryTA = nullptr;
lv_obj_t *cityKeyboard = nullptr;
lv_obj_t *cityStatusLabel = nullptr;

void loadCity()
{
    prefs.begin("weather", true);
    weatherCity    = prefs.getString("city",    DEFAULT_CITY);
    weatherState   = prefs.getString("state",   DEFAULT_STATE);
    weatherCountry = prefs.getString("country", DEFAULT_COUNTRY);
    weatherTzOffset = prefs.getLong("tz", -6 * 3600);
    prefs.end();

    Serial.printf("Weather city: %s, %s, %s\n",
                  weatherCity.c_str(),
                  weatherState.c_str(),
                  weatherCountry.c_str());
}

void saveCity()
{
    prefs.begin("weather", false);
    prefs.putString("city",    weatherCity);
    prefs.putString("state",   weatherState);
    prefs.putString("country", weatherCountry);
    prefs.end();
}

void setCityStatus(const String &msg)
{
    if (cityStatusLabel)
        lv_label_set_text(cityStatusLabel, msg.c_str());
}

String currentPlaceText()
{
    String t = weatherCity;

    if (weatherState.length() > 0)
        t += ", " + weatherState;

    if (weatherCountry.length() > 0)
        t += ", " + weatherCountry;

    return t;
}

// Auto-capitalisation for the City screen keyboard
//   City box:          capitals at the start and after a space or hyphen,
//                      lower case after a letter ("Kansas City", "Lee's Summit")
//   State / Country:   always capitals ("MO", "US")
// The ABC/abc key still works if you want to override it.
void updateCityKeyboardCase(bool force)
{
    if (!cityKeyboard)
        return;

    lv_obj_t *ta = lv_keyboard_get_textarea(cityKeyboard);

    if (!ta)
        return;

    lv_keyboard_mode_t mode = lv_keyboard_get_mode(cityKeyboard);

    // leave the numbers / symbols page alone while typing
    if (!force &&
        mode != LV_KEYBOARD_MODE_TEXT_LOWER &&
        mode != LV_KEYBOARD_MODE_TEXT_UPPER)
        return;

    lv_keyboard_mode_t want;

    if (ta == stateTA || ta == countryTA)
    {
        want = LV_KEYBOARD_MODE_TEXT_UPPER;
    }
    else
    {
        const char *text = lv_textarea_get_text(ta);
        uint32_t pos = lv_textarea_get_cursor_pos(ta);

        char prev = (pos == 0) ? ' ' : text[pos - 1];

        want = (prev == ' ' || prev == '-')
                   ? LV_KEYBOARD_MODE_TEXT_UPPER
                   : LV_KEYBOARD_MODE_TEXT_LOWER;
    }

    if (want != mode)
        lv_keyboard_set_mode(cityKeyboard, want);
}

// re-check after every key press in any of the three boxes
void cityFieldChanged(lv_event_t *e)
{
    updateCityKeyboardCase(false);
}

// point the keyboard at whichever box was tapped
void cityFieldClicked(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);

    lv_obj_remove_state(cityTA,    LV_STATE_FOCUSED);
    lv_obj_remove_state(stateTA,   LV_STATE_FOCUSED);
    lv_obj_remove_state(countryTA, LV_STATE_FOCUSED);

    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    lv_keyboard_set_textarea(cityKeyboard, ta);

    updateCityKeyboardCase(true);       // capitals for State/Country, etc.
}

void submitCity()
{
    String c  = lv_textarea_get_text(cityTA);
    String st = lv_textarea_get_text(stateTA);
    String co = lv_textarea_get_text(countryTA);

    c.trim();
    st.trim();
    co.trim();

    st.toUpperCase();
    co.toUpperCase();

    if (c.length() == 0)
    {
        setCityStatus("Please enter a city");
        return;
    }

    if (co.length() == 0)
        co = "US";

    // remember the old values in case the new city is rejected
    String oldCity = weatherCity;
    String oldState = weatherState;
    String oldCountry = weatherCountry;

    weatherCity = c;
    weatherState = st;
    weatherCountry = co;

    // offline: save now, check later
    if (WiFi.status() != WL_CONNECTED)
    {
        saveCity();
        updateWeatherDisplay();          // show the new city name now

        lastWeatherUpdate = millis() - weatherInterval - 1;

        setCityStatus("Saved " + currentPlaceText() +
                      "\nWeather will update when WiFi connects");
        return;
    }

    setCityStatus("Checking " + currentPlaceText() + "...");
    lv_refr_now(display);               // show the message before we block

    if (getWeather())
    {
        saveCity();
        updateWeatherDisplay();

        lastWeatherUpdate = millis();   // just fetched - restart the 10 min timer

        char line[64];
        snprintf(line, sizeof(line),
                 "Saved: %s  %.0fF  %s",
                 weatherPlaceName.c_str(),
                 currentTemp,
                 currentCondition.c_str());

        setCityStatus(line);
    }
    else
    {
        weatherCity = oldCity;
        weatherState = oldState;
        weatherCountry = oldCountry;

        if (lastWeatherHttpCode == 404)
            setCityStatus("City not found - check the spelling");
        else
            setCityStatus("Couldn't reach the weather service\nTry again in a moment");
    }
}

void cityKeyboardEvent(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY)         // keyboard OK key
        submitCity();

    if (code == LV_EVENT_CANCEL)        // keyboard close key
        goToSettings();
}

static lv_obj_t *makeCityField(const char *placeholder, int maxLen)
{
    lv_obj_t *ta = lv_textarea_create(cityScreen);

    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, maxLen);
    lv_textarea_set_placeholder_text(ta, placeholder);

    lv_obj_add_event_cb(ta, cityFieldClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, cityFieldChanged, LV_EVENT_VALUE_CHANGED, NULL);

    return ta;
}

void createCityScreen()
{
    cityScreen =
        lv_obj_create(NULL);

    lv_obj_set_style_bg_color(
        cityScreen,
        lv_color_hex(0x2e2d37),
        0);

    lv_obj_remove_flag(cityScreen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(
        cityScreen,
        gestureEvent,
        LV_EVENT_GESTURE,
        NULL);

    // ---------- title ----------

    lv_obj_t *title = lv_label_create(cityScreen);

    lv_label_set_text(title, "City");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // ---------- back ----------

    lv_obj_t *backBtn = lv_button_create(cityScreen);

    lv_obj_set_size(backBtn, 90, 40);
    lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 10, 55);

    lv_obj_add_event_cb(
        backBtn,
        [](lv_event_t *e) { goToSettings(); },
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(backLbl);

    // ---------- save ----------

    lv_obj_t *saveBtn = lv_button_create(cityScreen);

    lv_obj_set_size(saveBtn, 90, 40);
    lv_obj_align(saveBtn, LV_ALIGN_TOP_RIGHT, -10, 55);

    lv_obj_add_event_cb(
        saveBtn,
        [](lv_event_t *e) { submitCity(); },
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_t *saveLbl = lv_label_create(saveBtn);
    lv_label_set_text(saveLbl, LV_SYMBOL_SAVE " Save");
    lv_obj_center(saveLbl);

    // ---------- fields ----------

    cityTA = makeCityField("City", 40);
    lv_obj_set_width(cityTA, 300);
    lv_obj_align(cityTA, LV_ALIGN_TOP_MID, 0, 105);

    stateTA = makeCityField("State (US only)", 2);
    lv_obj_set_width(stateTA, 145);
    lv_obj_align(stateTA, LV_ALIGN_TOP_LEFT, 10, 155);

    countryTA = makeCityField("Country (US)", 2);
    lv_obj_set_width(countryTA, 145);
    lv_obj_align(countryTA, LV_ALIGN_TOP_RIGHT, -10, 155);

    // ---------- status ----------

    cityStatusLabel = lv_label_create(cityScreen);

    lv_obj_set_width(cityStatusLabel, 300);
    lv_obj_set_style_text_align(cityStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(cityStatusLabel, lv_color_hex(0xa0a0a0), 0);
    lv_obj_align(cityStatusLabel, LV_ALIGN_TOP_MID, 0, 205);

    // ---------- keyboard ----------

    cityKeyboard = lv_keyboard_create(cityScreen);

    lv_obj_set_size(cityKeyboard, 320, 230);
    lv_obj_align(cityKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_add_event_cb(cityKeyboard, cityKeyboardEvent, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(cityKeyboard, cityKeyboardEvent, LV_EVENT_CANCEL, NULL);

    lv_obj_add_event_cb(
        cityScreen,
        [](lv_event_t *e)
        {
            cityScreen = nullptr;
            cityTA = nullptr;
            stateTA = nullptr;
            countryTA = nullptr;
            cityKeyboard = nullptr;
            cityStatusLabel = nullptr;
        },
        LV_EVENT_DELETE,
        NULL);
}

void goToCity()
{
    if (!cityScreen)
        createCityScreen();

    currentScreen = SCREEN_CITY;

    // show what's in use now
    lv_textarea_set_text(cityTA,    weatherCity.c_str());
    lv_textarea_set_text(stateTA,   weatherState.c_str());
    lv_textarea_set_text(countryTA, weatherCountry.c_str());

    setCityStatus("Current: " + currentPlaceText() +
                  "\nTap a box to edit, then Save");

    // start with the City box selected
    lv_obj_remove_state(stateTA,   LV_STATE_FOCUSED);
    lv_obj_remove_state(countryTA, LV_STATE_FOCUSED);
    lv_obj_add_state(cityTA, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(cityKeyboard, cityTA);

    updateCityKeyboardCase(true);

    lv_screen_load_anim(
        cityScreen,
        LV_SCR_LOAD_ANIM_OVER_LEFT,
        250,
        0,
        false);
}

// =====================================================
// WEATHER PAGE COLOR SCREEN
// =====================================================
// LVGL 9 has no built-in colour wheel, so the wheel is generated
// once into a small RGB565 image (140 x 140 = ~39 KB of RAM, only
// allocated the first time the screen is opened).
//
//   angle around the wheel  = hue
//   distance from centre    = saturation
//   vertical slider         = brightness (value)

#define WHEEL_D     140
#define WHEEL_R     (WHEEL_D / 2)
#define WHEEL_X     65              // top-left of the wheel on screen
#define WHEEL_Y     150
#define VSLIDER_X   (WHEEL_X + WHEEL_D + 30)

#define COLOR_SCREEN_BG  0x2e2d37   // the picker screen itself keeps the standard look

enum ColorTarget { TARGET_BG = 0, TARGET_TEXT = 1 };
ColorTarget colorTarget = TARGET_BG;

float pickH = 0;        // 0-360
float pickS = 0;        // 0-1
float pickV = 1;        // 0-1

uint16_t *wheelBuf = nullptr;
lv_image_dsc_t wheelDsc;

lv_obj_t *wheelImg = nullptr;
lv_obj_t *wheelMarker = nullptr;
lv_obj_t *valueSlider = nullptr;
lv_obj_t *colorPreview = nullptr;
lv_obj_t *colorPreviewLbl = nullptr;
lv_obj_t *colorTargetBtns = nullptr;
lv_obj_t *colorHexLabel = nullptr;

// ---- popular colours for the swatches ----
static const uint32_t SWATCHES[12] =
{
    0x000000, 0xFFFFFF, 0x808080, 0xE53935,   // black, white, gray, red
    0xFB8C00, 0xFDD835, 0x43A047, 0x00897B,   // orange, yellow, green, teal
    0x1E88E5, 0x1A237E, 0x8E24AA, 0xEC407A    // blue, navy, purple, pink
};

// ---------------- colour maths ----------------

static uint32_t hsvToRgb(float h, float s, float v)
{
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;

    float r = 0, g = 0, b = 0;

    switch (((int)(h / 60.0f)) % 6)
    {
        case 0: r = c; g = x; b = 0; break;
        case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;
        case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;
        default: r = c; g = 0; b = x; break;
    }

    uint8_t R = (uint8_t)((r + m) * 255.0f + 0.5f);
    uint8_t G = (uint8_t)((g + m) * 255.0f + 0.5f);
    uint8_t B = (uint8_t)((b + m) * 255.0f + 0.5f);

    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}

static void rgbToHsv(uint32_t rgb, float &h, float &s, float &v)
{
    float r = ((rgb >> 16) & 0xFF) / 255.0f;
    float g = ((rgb >> 8) & 0xFF) / 255.0f;
    float b = (rgb & 0xFF) / 255.0f;

    float mx = max(r, max(g, b));
    float mn = min(r, min(g, b));
    float d = mx - mn;

    v = mx;
    s = (mx <= 0.0f) ? 0.0f : d / mx;

    if (d <= 0.0f)
        h = 0;
    else if (mx == r)
        h = 60.0f * fmodf((g - b) / d, 6.0f);
    else if (mx == g)
        h = 60.0f * ((b - r) / d + 2.0f);
    else
        h = 60.0f * ((r - g) / d + 4.0f);

    if (h < 0)
        h += 360.0f;
}

static uint16_t rgbTo565(uint32_t rgb)
{
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;

    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// ---------------- storage ----------------

void loadWeatherColors()
{
    prefs.begin("wcolors", true);
    weatherBgColor   = prefs.getUInt("bg",   DEFAULT_WEATHER_BG);
    weatherTextColor = prefs.getUInt("text", DEFAULT_WEATHER_TEXT);
    prefs.end();
}

void saveWeatherColors()
{
    prefs.begin("wcolors", false);
    prefs.putUInt("bg",   weatherBgColor);
    prefs.putUInt("text", weatherTextColor);
    prefs.end();
}

// ---------------- apply to the weather screen ----------------

void applyWeatherColors()
{
    if (!weatherScreen)
        return;

    lv_obj_set_style_bg_color(
        weatherScreen,
        lv_color_hex(weatherBgColor),
        0);

    lv_obj_t *labels[] =
    {
        timeLabel, dateLabel, cityNameLabel, tempLabel,
        feelsLikeLabel, humidityLabel, sunriseLabel, sunsetLabel,
        weatherCredit
    };

    for (lv_obj_t *l : labels)
    {
        if (l)
            lv_obj_set_style_text_color(l, lv_color_hex(weatherTextColor), 0);
    }

    // the moon icon's cut-out uses the background colour
    drawWeatherIcon(currentIconCode);

    // the Wind and Forecast screens follow the weather screen's colours
    applyWindColors();
    applyForecastColors();
}

// ---------------- picker UI ----------------

uint32_t &targetColor()
{
    return (colorTarget == TARGET_BG) ? weatherBgColor : weatherTextColor;
}

void updateColorPreview()
{
    if (!colorPreview)
        return;

    lv_obj_set_style_bg_color(colorPreview, lv_color_hex(weatherBgColor), 0);
    lv_obj_set_style_text_color(colorPreviewLbl, lv_color_hex(weatherTextColor), 0);

    char hex[24];
    snprintf(hex, sizeof(hex), "%s  #%06lX",
             colorTarget == TARGET_BG ? "Background" : "Text",
             (unsigned long)targetColor());

    lv_label_set_text(colorHexLabel, hex);
}

// move the ring + slider to show pickH / pickS / pickV
void showPickerPosition()
{
    float a = pickH * PI / 180.0f;
    float d = pickS * (WHEEL_R - 2);

    int x = WHEEL_X + WHEEL_R + (int)(cosf(a) * d);
    int y = WHEEL_Y + WHEEL_R + (int)(sinf(a) * d);

    lv_obj_set_pos(wheelMarker, x - 8, y - 8);

    lv_slider_set_value(valueSlider, (int)(pickV * 100.0f + 0.5f), LV_ANIM_OFF);

    // slider shows the fully-bright version of the hue at the top
    lv_obj_set_style_bg_color(
        valueSlider,
        lv_color_hex(hsvToRgb(pickH, pickS, 1.0f)),
        LV_PART_MAIN);
}

// load the selected target's colour into the picker
void syncPickerToTarget()
{
    rgbToHsv(targetColor(), pickH, pickS, pickV);
    showPickerPosition();
    updateColorPreview();
}

// picker changed -> store colour and apply it
void pickerChanged()
{
    targetColor() = hsvToRgb(pickH, pickS, pickV);

    applyWeatherColors();
    showPickerPosition();
    updateColorPreview();
}

void wheelTouched(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_RELEASED)
    {
        saveWeatherColors();
        return;
    }

    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);

    float dx = p.x - (WHEEL_X + WHEEL_R);
    float dy = p.y - (WHEEL_Y + WHEEL_R);
    float d = sqrtf(dx * dx + dy * dy);

    pickH = atan2f(dy, dx) * 180.0f / PI;
    if (pickH < 0)
        pickH += 360.0f;

    pickS = min(d / WHEEL_R, 1.0f);

    // picking a hue while brightness is near 0 would still look black
    if (pickV < 0.2f)
        pickV = 1.0f;

    pickerChanged();
}

void valueSliderEvent(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
    {
        saveWeatherColors();
        return;
    }

    pickV = lv_slider_get_value(valueSlider) / 100.0f;
    pickerChanged();
}

void swatchClicked(lv_event_t *e)
{
    uint32_t rgb = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    targetColor() = rgb;

    applyWeatherColors();
    syncPickerToTarget();       // jumps the ring to the swatch's spot
    saveWeatherColors();
}

void colorDefaultClicked(lv_event_t *e)
{
    weatherBgColor = DEFAULT_WEATHER_BG;
    weatherTextColor = DEFAULT_WEATHER_TEXT;

    applyWeatherColors();
    syncPickerToTarget();
    saveWeatherColors();
}

void colorTargetChanged(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(colorTargetBtns);

    colorTarget = (id == 1) ? TARGET_TEXT : TARGET_BG;

    syncPickerToTarget();
}

// build the wheel image (first visit only)
bool buildWheel()
{
    if (wheelBuf)
    {
        lv_image_set_src(wheelImg, &wheelDsc);
        return true;
    }

    wheelBuf = (uint16_t *)malloc(WHEEL_D * WHEEL_D * sizeof(uint16_t));

    if (!wheelBuf)
    {
        Serial.println("Color wheel: not enough memory");
        return false;
    }

    uint16_t outside = rgbTo565(COLOR_SCREEN_BG);

    for (int y = 0; y < WHEEL_D; y++)
    {
        for (int x = 0; x < WHEEL_D; x++)
        {
            float dx = x - WHEEL_R + 0.5f;
            float dy = y - WHEEL_R + 0.5f;
            float d = sqrtf(dx * dx + dy * dy);

            if (d > WHEEL_R)
            {
                wheelBuf[y * WHEEL_D + x] = outside;
                continue;
            }

            float h = atan2f(dy, dx) * 180.0f / PI;
            if (h < 0)
                h += 360.0f;

            wheelBuf[y * WHEEL_D + x] =
                rgbTo565(hsvToRgb(h, d / WHEEL_R, 1.0f));
        }
    }

    memset(&wheelDsc, 0, sizeof(wheelDsc));

#ifdef LV_IMAGE_HEADER_MAGIC
    wheelDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
#endif
    wheelDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    wheelDsc.header.w = WHEEL_D;
    wheelDsc.header.h = WHEEL_D;
    wheelDsc.header.stride = WHEEL_D * sizeof(uint16_t);
    wheelDsc.data_size = WHEEL_D * WHEEL_D * sizeof(uint16_t);
    wheelDsc.data = (const uint8_t *)wheelBuf;

    lv_image_set_src(wheelImg, &wheelDsc);

    return true;
}

void createColorScreen()
{
    colorScreen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(colorScreen, lv_color_hex(COLOR_SCREEN_BG), 0);
    lv_obj_remove_flag(colorScreen, LV_OBJ_FLAG_SCROLLABLE);

    // (no swipe navigation here - dragging on the wheel would trigger it)

    // ---------- top row: Back | title | Default ----------

    lv_obj_t *backBtn = lv_button_create(colorScreen);
    lv_obj_set_size(backBtn, 80, 36);
    lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_add_event_cb(backBtn, [](lv_event_t *e) { goToSettings(); },
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(backLbl);

    lv_obj_t *title = lv_label_create(colorScreen);
    lv_label_set_text(title, "Weather Colors");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *defBtn = lv_button_create(colorScreen);
    lv_obj_set_size(defBtn, 80, 36);
    lv_obj_align(defBtn, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_add_event_cb(defBtn, colorDefaultClicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *defLbl = lv_label_create(defBtn);
    lv_label_set_text(defLbl, "Default");
    lv_obj_center(defLbl);

    // ---------- Background / Text selector ----------

    static const char *targetMap[] = { "Background", "Text", "" };

    colorTargetBtns = lv_buttonmatrix_create(colorScreen);
    lv_buttonmatrix_set_map(colorTargetBtns, targetMap);
    lv_buttonmatrix_set_button_ctrl_all(colorTargetBtns, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(colorTargetBtns, true);
    lv_buttonmatrix_set_button_ctrl(colorTargetBtns, 0, LV_BUTTONMATRIX_CTRL_CHECKED);

    lv_obj_set_size(colorTargetBtns, 300, 44);
    lv_obj_set_style_pad_all(colorTargetBtns, 4, 0);
    lv_obj_align(colorTargetBtns, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_add_event_cb(colorTargetBtns, colorTargetChanged,
                        LV_EVENT_VALUE_CHANGED, NULL);

    // ---------- live preview ----------

    colorPreview = lv_obj_create(colorScreen);
    lv_obj_set_size(colorPreview, 300, 40);
    lv_obj_align(colorPreview, LV_ALIGN_TOP_MID, 0, 102);
    lv_obj_set_style_border_color(colorPreview, lv_color_hex(0x606060), 0);
    lv_obj_set_style_border_width(colorPreview, 1, 0);
    lv_obj_set_style_pad_all(colorPreview, 0, 0);
    lv_obj_remove_flag(colorPreview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(colorPreview, LV_OBJ_FLAG_CLICKABLE);

    colorPreviewLbl = lv_label_create(colorPreview);
    lv_label_set_text(colorPreviewLbl, "Preview  72F  Sunny");
    lv_obj_set_style_text_font(colorPreviewLbl, &lv_font_montserrat_28, 0);
    lv_obj_center(colorPreviewLbl);

    // ---------- wheel ----------

    wheelImg = lv_image_create(colorScreen);
    lv_obj_set_size(wheelImg, WHEEL_D, WHEEL_D);
    lv_obj_set_pos(wheelImg, WHEEL_X, WHEEL_Y);

    lv_obj_add_flag(wheelImg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(wheelImg, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_add_event_cb(wheelImg, wheelTouched, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(wheelImg, wheelTouched, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(wheelImg, wheelTouched, LV_EVENT_RELEASED, NULL);

    // selection ring
    wheelMarker = lv_obj_create(colorScreen);
    lv_obj_remove_style_all(wheelMarker);
    lv_obj_set_size(wheelMarker, 16, 16);
    lv_obj_set_style_radius(wheelMarker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(wheelMarker, lv_color_white(), 0);
    lv_obj_set_style_border_width(wheelMarker, 3, 0);
    lv_obj_set_style_outline_color(wheelMarker, lv_color_black(), 0);
    lv_obj_set_style_outline_width(wheelMarker, 1, 0);
    lv_obj_remove_flag(wheelMarker, LV_OBJ_FLAG_CLICKABLE);

    // ---------- brightness (value) slider, vertical ----------

    valueSlider = lv_slider_create(colorScreen);
    lv_obj_set_size(valueSlider, 18, WHEEL_D);         // taller than wide = vertical
    lv_obj_set_pos(valueSlider, VSLIDER_X, WHEEL_Y);
    lv_slider_set_range(valueSlider, 0, 100);

    // track: bright colour at the top fading to black at the bottom
    lv_obj_set_style_bg_grad_color(valueSlider, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(valueSlider, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(valueSlider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(valueSlider, LV_OPA_TRANSP, LV_PART_INDICATOR);

    lv_obj_set_ext_click_area(valueSlider, 12);
    lv_obj_remove_flag(valueSlider, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_add_event_cb(valueSlider, valueSliderEvent, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(valueSlider, valueSliderEvent, LV_EVENT_RELEASED, NULL);

    // ---------- current colour readout ----------

    colorHexLabel = lv_label_create(colorScreen);
    lv_obj_set_style_text_color(colorHexLabel, lv_color_hex(0xa0a0a0), 0);
    lv_obj_align(colorHexLabel, LV_ALIGN_TOP_MID, 0, WHEEL_Y + WHEEL_D + 8);

    // ---------- popular colour swatches: 2 rows x 6 ----------

    const int SW = 40, SH = 36, GAP = 8;
    const int startX = (320 - (6 * SW + 5 * GAP)) / 2;
    const int startY = WHEEL_Y + WHEEL_D + 34;

    for (int i = 0; i < 12; i++)
    {
        lv_obj_t *sw = lv_button_create(colorScreen);

        lv_obj_set_size(sw, SW, SH);
        lv_obj_set_pos(sw,
                       startX + (i % 6) * (SW + GAP),
                       startY + (i / 6) * (SH + GAP));

        lv_obj_set_style_bg_color(sw, lv_color_hex(SWATCHES[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(0x808080), 0);
        lv_obj_set_style_border_width(sw, 1, 0);
        lv_obj_set_style_radius(sw, 6, 0);
        lv_obj_set_style_shadow_width(sw, 0, 0);

        lv_obj_add_event_cb(sw, swatchClicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)SWATCHES[i]);
    }

    // when this screen is deleted, forget its objects and free the wheel
    lv_obj_add_event_cb(
        colorScreen,
        [](lv_event_t *e)
        {
            colorScreen = nullptr;
            wheelImg = nullptr;
            wheelMarker = nullptr;
            valueSlider = nullptr;
            colorPreview = nullptr;
            colorPreviewLbl = nullptr;
            colorTargetBtns = nullptr;
            colorHexLabel = nullptr;

            // the wheel image is deleted with this screen, so its
            // pixel buffer can be given back (~39 KB)
            if (wheelBuf)
            {
                free(wheelBuf);
                wheelBuf = nullptr;
            }
        },
        LV_EVENT_DELETE,
        NULL);

    lv_obj_t *hint = lv_label_create(colorScreen);
    lv_label_set_text(hint, "Changes are saved automatically");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x808080), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, startY + 2 * SH + GAP + 10);
}

void goToWeatherColors()
{
    if (!colorScreen)
        createColorScreen();

    currentScreen = SCREEN_COLORS;

    bool wheelOk = buildWheel();

    syncPickerToTarget();

    if (!wheelOk)
        lv_label_set_text(colorHexLabel, "Not enough memory for the wheel");

    lv_screen_load_anim(
        colorScreen,
        LV_SCR_LOAD_ANIM_OVER_LEFT,
        250,
        0,
        false);
}

// =====================================================
// AUTO DIM (sunset -> 5%, sunrise -> 100%)
// =====================================================

// set the backlight and move the Settings slider/label to match
void showBrightness(int percent)
{
    setBacklight(percent);

    if (brightSliderObj)
        lv_slider_set_value(brightSliderObj, brightnessPercent, LV_ANIM_OFF);

    if (brightLabelObj)
        lv_label_set_text_fmt(
            brightLabelObj,
            LV_SYMBOL_IMAGE "  Brightness  %d%%",
            brightnessPercent);
}

// grey out the slider and ignore touches while Auto Dim is on
void updateSliderLock()
{
    if (!brightSliderObj)
        return;

    if (autoDimEnabled)
    {
        lv_obj_add_state(brightSliderObj, LV_STATE_DISABLED);
        lv_obj_remove_flag(brightSliderObj, LV_OBJ_FLAG_CLICKABLE);
    }
    else
    {
        lv_obj_remove_state(brightSliderObj, LV_STATE_DISABLED);
        lv_obj_add_flag(brightSliderObj, LV_OBJ_FLAG_CLICKABLE);
    }
}

// called from loop(): switch brightness when the sun rises or sets
void checkAutoDim()
{
    if (!autoDimEnabled)
        return;

    static unsigned long lastCheck = 0;

    if (autoDimPhase != -1 && millis() - lastCheck < 5000)
        return;

    lastCheck = millis();

    time_t now = time(nullptr);

    // need a synced clock and today's sun times from the weather
    if (now < 1700000000 || currentSunrise <= 0 || currentSunset <= 0)
        return;

    int phase = (now >= currentSunrise && now < currentSunset) ? 1 : 0;

    if (phase == autoDimPhase)
        return;

    autoDimPhase = phase;

    int target = phase ? autoDimHigh : autoDimLow;

    Serial.printf("Auto Dim: %s -> %d%%\n",
                  phase ? "sunrise" : "sunset", target);

    showBrightness(target);
}

void updateDimRangeLabel()
{
    if (dimRangeLabel)
        lv_label_set_text_fmt(
            dimRangeLabel,
            "Auto Dim  night %d%%  -  day %d%%",
            autoDimLow,
            autoDimHigh);
}

void saveDimLimits()
{
    prefs.begin("display", false);
    prefs.putInt("dimlo", autoDimLow);
    prefs.putInt("dimhi", autoDimHigh);
    prefs.end();
}

void saveAutoDim()
{
    prefs.begin("display", false);
    prefs.putBool("autodim", autoDimEnabled);
    prefs.end();
}

void setAutoDim(bool on)
{
    autoDimEnabled = on;
    autoDimPhase = -1;              // re-evaluate straight away

    saveAutoDim();
    updateSliderLock();

    if (on)
    {
        checkAutoDim();
    }
    else
    {
        // back to manual: restore the brightness last set on the slider
        prefs.begin("display", true);
        int manual = prefs.getInt("bright", 100);
        prefs.end();

        showBrightness(manual);
    }
}

// restore the toggle after a reboot (call after the Settings screen exists)
void loadAutoDim()
{
    prefs.begin("display", true);
    // Auto Dim is ON the very first time (nothing saved yet);
    // after that, whatever the user last chose is remembered.
    autoDimEnabled = prefs.getBool("autodim", true);

    autoDimLow  = prefs.getInt("dimlo", AUTO_DIM_NIGHT_PERCENT);
    autoDimHigh = prefs.getInt("dimhi", AUTO_DIM_DAY_PERCENT);
    prefs.end();

    if (autoDimBtn)
    {
        if (autoDimEnabled)
            lv_obj_add_state(autoDimBtn, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(autoDimBtn, LV_STATE_CHECKED);
    }

    // put both handles where the saved limits are
    if (dimRangeSlider)
    {
        lv_slider_set_value(dimRangeSlider, autoDimHigh, LV_ANIM_OFF);
        lv_slider_set_left_value(dimRangeSlider, autoDimLow, LV_ANIM_OFF);
    }

    updateDimRangeLabel();

    autoDimPhase = -1;
    updateSliderLock();
}

// =====================================================
// WIND SCREEN (opened by tapping the weather icon)
// =====================================================
// Compass with an arrow pointing the way the wind is blowing,
// plus speed, the direction it's coming FROM, and gusts.
// Uses the Weather screen's background / text colours.

#define COMPASS_CX     160
#define COMPASS_CY     205
#define COMPASS_R      100
#define WIND_ARROW_COL 0xFF7043

lv_obj_t *compassRing = nullptr;
lv_obj_t *compassLabels[4] = { nullptr };
lv_obj_t *compassTicks[8] = { nullptr };
lv_obj_t *windTitle = nullptr;
lv_obj_t *windShaft = nullptr;
lv_obj_t *windHead = nullptr;
lv_obj_t *windHub = nullptr;
lv_obj_t *windSpeedLabel = nullptr;
lv_obj_t *windFromLabel = nullptr;
lv_obj_t *windGustLabel = nullptr;

// arrow points must stay in memory, so they're static
static lv_point_precise_t shaftPts[2];
static lv_point_precise_t headPts[3];

// 0-359 degrees -> "N", "NNE", "NE" ...
const char *compassName(int deg)
{
    static const char *names[16] =
    {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
    };

    int i = (int)((deg % 360 + 360) % 360 / 22.5f + 0.5f) % 16;
    return names[i];
}

// Beaufort-style description of the wind speed (mph)
const char *windDescription(float mph)
{
    if (mph < 1)  return "Calm";
    if (mph < 4)  return "Light air";
    if (mph < 8)  return "Light breeze";
    if (mph < 13) return "Gentle breeze";
    if (mph < 19) return "Moderate breeze";
    if (mph < 25) return "Fresh breeze";
    if (mph < 32) return "Strong breeze";
    if (mph < 39) return "Near gale";
    if (mph < 47) return "Gale";
    if (mph < 55) return "Strong gale";
    if (mph < 64) return "Storm";
    if (mph < 73) return "Violent storm";
    return "Hurricane force";
}

void updateWindScreen()
{
    if (!windScreen)
        return;

    // ---------- arrow ----------
    bool haveDir = currentWindDeg >= 0 && currentWindMph >= 1.0f;

    if (haveDir)
    {
        // wind blows TOWARD the opposite of where it comes from
        float toward = (currentWindDeg + 180) * PI / 180.0f;

        float dx = sinf(toward);      // screen x: east is right
        float dy = -cosf(toward);     // screen y: north is up

        float headX = COMPASS_CX + dx * COMPASS_R * 0.72f;
        float headY = COMPASS_CY + dy * COMPASS_R * 0.72f;

        shaftPts[0].x = COMPASS_CX - dx * COMPASS_R * 0.55f;
        shaftPts[0].y = COMPASS_CY - dy * COMPASS_R * 0.55f;
        shaftPts[1].x = headX;
        shaftPts[1].y = headY;

        // arrowhead wings: back along the shaft, out to each side
        float px = -dy, py = dx;      // perpendicular
        headPts[0].x = headX - dx * 22 + px * 13;
        headPts[0].y = headY - dy * 22 + py * 13;
        headPts[1].x = headX;
        headPts[1].y = headY;
        headPts[2].x = headX - dx * 22 - px * 13;
        headPts[2].y = headY - dy * 22 - py * 13;

        lv_line_set_points(windShaft, shaftPts, 2);
        lv_line_set_points(windHead, headPts, 3);

        lv_obj_remove_flag(windShaft, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(windHead, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(windShaft, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(windHead, LV_OBJ_FLAG_HIDDEN);
    }

    // ---------- text ----------
    // LVGL's own _fmt can't print floats (prints "f"), so use snprintf
    char buf[48];

    snprintf(buf, sizeof(buf), "%.0f mph", currentWindMph);
    lv_label_set_text(windSpeedLabel, buf);

    if (haveDir)
        lv_label_set_text_fmt(windFromLabel, "%s from the %s (%d deg)",
                              windDescription(currentWindMph),
                              compassName(currentWindDeg),
                              currentWindDeg);
    else
        lv_label_set_text(windFromLabel, windDescription(currentWindMph));

    if (currentWindGust > currentWindMph + 0.5f)
    {
        snprintf(buf, sizeof(buf), "Gusts to %.0f mph", currentWindGust);
        lv_label_set_text(windGustLabel, buf);
    }
    else
        lv_label_set_text(windGustLabel, "No gusts reported");
}

// match the Weather screen's colours
void applyWindColors()
{
    if (!windScreen)
        return;

    lv_color_t bg = lv_color_hex(weatherBgColor);
    lv_color_t fg = lv_color_hex(weatherTextColor);

    lv_obj_set_style_bg_color(windScreen, bg, 0);
    lv_obj_set_style_border_color(compassRing, fg, 0);

    lv_obj_t *texts[] = { windTitle, windSpeedLabel, windFromLabel, windGustLabel,
                          compassLabels[0], compassLabels[1],
                          compassLabels[2], compassLabels[3] };

    for (lv_obj_t *t : texts)
        if (t) lv_obj_set_style_text_color(t, fg, 0);

    for (lv_obj_t *t : compassTicks)
        if (t) lv_obj_set_style_bg_color(t, fg, 0);
}

static lv_obj_t *windDot(int cx, int cy, int d, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(windScreen);

    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);

    return o;
}

void createWindScreen()
{
    windScreen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(windScreen, lv_color_hex(weatherBgColor), 0);
    lv_obj_remove_flag(windScreen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(windScreen, gestureEvent, LV_EVENT_GESTURE, NULL);

    // ---------- back + title ----------

    lv_obj_t *backBtn = lv_button_create(windScreen);
    lv_obj_set_size(backBtn, 80, 36);
    lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_event_cb(backBtn, [](lv_event_t *e) { goBackToWeather(); },
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(backLbl);

    // NEW: top-right button -> 5-day forecast
    lv_obj_t *fcBtn = lv_button_create(windScreen);
    lv_obj_set_size(fcBtn, 90, 36);
    lv_obj_align(fcBtn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(fcBtn, [](lv_event_t *e) { goToForecast(); },
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *fcLbl = lv_label_create(fcBtn);
    lv_label_set_text(fcLbl, "5-Day " LV_SYMBOL_RIGHT);
    lv_obj_center(fcLbl);

    windTitle = lv_label_create(windScreen);
    lv_label_set_text(windTitle, "Wind");
    lv_obj_set_style_text_font(windTitle, &lv_font_montserrat_28, 0);
    lv_obj_align(windTitle, LV_ALIGN_TOP_MID, 0, 12);

    // ---------- compass ring ----------

    compassRing = lv_obj_create(windScreen);
    lv_obj_remove_style_all(compassRing);
    lv_obj_set_size(compassRing, COMPASS_R * 2, COMPASS_R * 2);
    lv_obj_set_pos(compassRing, COMPASS_CX - COMPASS_R, COMPASS_CY - COMPASS_R);
    lv_obj_set_style_radius(compassRing, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(compassRing, 3, 0);
    lv_obj_set_style_border_opa(compassRing, LV_OPA_COVER, 0);
    lv_obj_remove_flag(compassRing, LV_OBJ_FLAG_CLICKABLE);

    // tick marks every 45 degrees, just inside the ring
    for (int i = 0; i < 8; i++)
    {
        float a = i * PI / 4.0f;
        int r = COMPASS_R - 12;
        int d = (i % 2 == 0) ? 8 : 5;      // bigger dots at N/E/S/W

        compassTicks[i] = windDot(COMPASS_CX + (int)(sinf(a) * r),
                                  COMPASS_CY - (int)(cosf(a) * r),
                                  d, weatherTextColor);
    }

    // N / E / S / W just outside the ring
    const char *card[4] = { "N", "E", "S", "W" };

    for (int i = 0; i < 4; i++)
    {
        float a = i * PI / 2.0f;
        int r = COMPASS_R + 18;

        compassLabels[i] = lv_label_create(windScreen);
        lv_label_set_text(compassLabels[i], card[i]);
        lv_obj_set_style_text_font(compassLabels[i], &lv_font_montserrat_28, 0);

        // centre each letter on its point
        lv_obj_align(compassLabels[i], LV_ALIGN_CENTER,
                     (int)(sinf(a) * r) + COMPASS_CX - 160,
                     -(int)(cosf(a) * r) + COMPASS_CY - 240);
    }

    // ---------- arrow ----------

    windShaft = lv_line_create(windScreen);
    lv_obj_set_pos(windShaft, 0, 0);
    lv_obj_set_style_line_width(windShaft, 6, 0);
    lv_obj_set_style_line_color(windShaft, lv_color_hex(WIND_ARROW_COL), 0);
    lv_obj_set_style_line_rounded(windShaft, true, 0);
    lv_obj_remove_flag(windShaft, LV_OBJ_FLAG_CLICKABLE);

    windHead = lv_line_create(windScreen);
    lv_obj_set_pos(windHead, 0, 0);
    lv_obj_set_style_line_width(windHead, 6, 0);
    lv_obj_set_style_line_color(windHead, lv_color_hex(WIND_ARROW_COL), 0);
    lv_obj_set_style_line_rounded(windHead, true, 0);
    lv_obj_remove_flag(windHead, LV_OBJ_FLAG_CLICKABLE);

    windHub = windDot(COMPASS_CX, COMPASS_CY, 14, WIND_ARROW_COL);

    // ---------- readouts ----------

    windSpeedLabel = lv_label_create(windScreen);
    lv_obj_set_style_text_font(windSpeedLabel, &lv_font_montserrat_48, 0);
    lv_obj_align(windSpeedLabel, LV_ALIGN_TOP_MID, 0, 345);

    windFromLabel = lv_label_create(windScreen);
    lv_obj_set_width(windFromLabel, 300);
    lv_obj_set_style_text_align(windFromLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(windFromLabel, LV_ALIGN_TOP_MID, 0, 402);

    windGustLabel = lv_label_create(windScreen);
    lv_obj_align(windGustLabel, LV_ALIGN_TOP_MID, 0, 428);

    // OpenWeather credit along the bottom
    addCreditTicker(windScreen, 458, weatherTextColor);

    lv_obj_add_event_cb(
        windScreen,
        [](lv_event_t *e)
        {
            windScreen = nullptr;
            compassRing = nullptr;
            windTitle = nullptr;
            windShaft = nullptr;
            windHead = nullptr;
            windHub = nullptr;
            windSpeedLabel = nullptr;
            windFromLabel = nullptr;
            windGustLabel = nullptr;

            for (auto &o : compassLabels) o = nullptr;
            for (auto &o : compassTicks) o = nullptr;
        },
        LV_EVENT_DELETE,
        NULL);

    applyWindColors();
    updateWindScreen();
}

void goToWind()
{
    if (!windScreen)
        createWindScreen();

    currentScreen = SCREEN_WIND;

    updateWindScreen();

    lv_screen_load_anim(
        windScreen,
        LV_SCR_LOAD_ANIM_OVER_LEFT,
        250,
        0,
        false);
}

void goBackToWeather()
{
    bool freeOld =
        (currentScreen == SCREEN_WIND ||
         currentScreen == SCREEN_FORECAST);

    currentScreen = SCREEN_WEATHER;

    lv_screen_load_anim(
        weatherScreen,
        LV_SCR_LOAD_ANIM_OVER_RIGHT,
        250,
        0,
        freeOld);       // delete Wind / Forecast screen after the slide
}

// =====================================================
// 5-DAY FORECAST SCREEN (double-tap the weather icon)
// =====================================================
// Uses OpenWeatherMap's free 5 day / 3 hour forecast and rolls the
// 3-hour slots up into one row per day: high, low, rain chance and
// the icon from the slot closest to midday.

#define FC_DAYS          5
#define FC_ROW_Y0        62
#define FC_ROW_H         78
#define FC_ICON_K        0.6f              // 90x70 icon drawn at 54x42
#define FC_MAX_AGE_MS    (30UL * 60UL * 1000UL)   // refresh after 30 min

struct ForecastDay
{
    bool  valid;
    float lo;
    float hi;
    int   pop;          // max chance of precipitation, %
    String icon;
    long  bestNoonDiff;
    int   weekday;      // 0 = Sun
};

ForecastDay forecast[FC_DAYS];

unsigned long lastForecastFetch = 0;
String forecastPlaceKey;                   // which city the data is for

bool forecastFetchPending = false;
unsigned long forecastRequestAt = 0;

lv_obj_t *fcTitle = nullptr;
lv_obj_t *fcStatus = nullptr;
lv_obj_t *fcRow[FC_DAYS];
lv_obj_t *fcDay[FC_DAYS];
lv_obj_t *fcIcon[FC_DAYS];
lv_obj_t *fcTemps[FC_DAYS];
lv_obj_t *fcPop[FC_DAYS];
lv_obj_t *fcLine[FC_DAYS];

// ---------------- single / double tap on the weather icon ----------------

#define DOUBLE_TAP_MS 350

static lv_timer_t *iconTapTimer = nullptr;

static void iconSingleTap(lv_timer_t *t)
{
    iconTapTimer = nullptr;             // one-shot timer deletes itself

    if (currentScreen == SCREEN_WEATHER)
        goToWind();
}

void weatherIconClicked(lv_event_t *e)
{
    if (iconTapTimer)
    {
        // second tap inside the window -> double tap
        lv_timer_delete(iconTapTimer);
        iconTapTimer = nullptr;

        goToForecast();
    }
    else
    {
        // first tap: wait briefly to see if a second one follows
        iconTapTimer = lv_timer_create(iconSingleTap, DOUBLE_TAP_MS, NULL);
        lv_timer_set_repeat_count(iconTapTimer, 1);
    }
}

// ---------------- download ----------------

bool getForecast()
{
    if (WiFi.status() != WL_CONNECTED)
        return false;

    String place = weatherCity;

    if (weatherState.length() > 0)
        place += "," + weatherState;

    if (weatherCountry.length() > 0)
        place += "," + weatherCountry;

    place.replace(" ", "%20");

    String url =
        "https://api.openweathermap.org/data/2.5/forecast?q=" +
        place +
        "&units=imperial&appid=" +
        String(OWM_API_KEY);

    Serial.println("Downloading forecast...");

    HTTPClient http;

    http.useHTTP10(true);               // plain stream, so it can be parsed on the fly
    http.begin(url);
    http.setConnectTimeout(3000);
    http.setTimeout(5000);

    int code = http.GET();

    if (code != HTTP_CODE_OK)
    {
        Serial.printf("Forecast HTTP Error: %d\n", code);
        http.end();
        return false;
    }

    // only keep the fields we use - the full reply is ~16 KB
    JsonDocument filter;
    filter["city"]["timezone"] = true;
    filter["list"][0]["dt"] = true;
    filter["list"][0]["main"]["temp_min"] = true;
    filter["list"][0]["main"]["temp_max"] = true;
    filter["list"][0]["weather"][0]["icon"] = true;
    filter["list"][0]["pop"] = true;

    JsonDocument doc;

    DeserializationError err =
        deserializeJson(doc, http.getStream(),
                        DeserializationOption::Filter(filter));

    http.end();

    if (err)
    {
        Serial.printf("Forecast JSON Error: %s\n", err.c_str());
        return false;
    }

    long tz = doc["city"]["timezone"] | weatherTzOffset;

    JsonArray list = doc["list"];

    if (list.size() == 0)
        return false;

    // "today" in the city's local time
    time_t now = time(nullptr);

    if (now < 1700000000)                       // clock not synced yet
        now = list[0]["dt"].as<long>();

    long today = (long)((now + tz) / 86400);

    for (int i = 0; i < FC_DAYS; i++)
    {
        forecast[i].valid = false;
        forecast[i].lo = 999;
        forecast[i].hi = -999;
        forecast[i].pop = 0;
        forecast[i].icon = "";
        forecast[i].bestNoonDiff = 999999;
        forecast[i].weekday = (int)((today + i + 4) % 7);   // 1 Jan 1970 was a Thursday
    }

    for (JsonObject item : list)
    {
        long local = item["dt"].as<long>() + tz;
        long idx = local / 86400 - today;

        if (idx < 0 || idx >= FC_DAYS)
            continue;

        ForecastDay &d = forecast[idx];

        d.valid = true;
        d.lo = min(d.lo, item["main"]["temp_min"].as<float>());
        d.hi = max(d.hi, item["main"]["temp_max"].as<float>());
        d.pop = max(d.pop, (int)(item["pop"].as<float>() * 100.0f + 0.5f));

        // icon from the slot nearest 12:00 local, shown as a daytime icon
        long noonDiff = labs((local % 86400) - 43200);

        if (noonDiff < d.bestNoonDiff)
        {
            d.bestNoonDiff = noonDiff;

            String ic = item["weather"][0]["icon"].as<String>();
            d.icon = ic.substring(0, 2) + "d";
        }
    }

    Serial.println("Forecast OK");
    return true;
}

// ---------------- screen ----------------

void updateForecastScreen()
{
    static const char *days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

    char buf[32];
    int shown = 0;

    for (int i = 0; i < FC_DAYS; i++)
    {
        ForecastDay &d = forecast[i];

        if (!d.valid)
        {
            lv_obj_add_flag(fcRow[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_remove_flag(fcRow[i], LV_OBJ_FLAG_HIDDEN);
        shown++;

        lv_label_set_text(fcDay[i], i == 0 ? "Today" : days[d.weekday]);

        // (LVGL's _fmt can't print floats, so use snprintf)
        snprintf(buf, sizeof(buf), "%.0f / %.0f", d.hi, d.lo);
        lv_label_set_text(fcTemps[i], buf);

        if (d.pop > 0)
            snprintf(buf, sizeof(buf), "%d%%\nrain", d.pop);
        else
            snprintf(buf, sizeof(buf), " ");
        lv_label_set_text(fcPop[i], buf);

        drawIconInto(fcIcon[i], d.icon, FC_ICON_K, i + 1);
    }

    snprintf(buf, sizeof(buf), "%d-Day Forecast", shown > 0 ? shown : FC_DAYS);
    lv_label_set_text(fcTitle, buf);
}

void showForecastStatus(const char *msg)
{
    lv_label_set_text(fcStatus, msg);
    lv_obj_remove_flag(fcStatus, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < FC_DAYS; i++)
        lv_obj_add_flag(fcRow[i], LV_OBJ_FLAG_HIDDEN);
}

void applyForecastColors()
{
    if (!forecastScreen)
        return;

    lv_color_t fg = lv_color_hex(weatherTextColor);

    lv_obj_set_style_bg_color(forecastScreen, lv_color_hex(weatherBgColor), 0);
    lv_obj_set_style_text_color(fcTitle, fg, 0);
    lv_obj_set_style_text_color(fcStatus, fg, 0);

    for (int i = 0; i < FC_DAYS; i++)
    {
        lv_obj_set_style_text_color(fcDay[i], fg, 0);
        lv_obj_set_style_text_color(fcTemps[i], fg, 0);
        lv_obj_set_style_text_color(fcPop[i], fg, 0);
        lv_obj_set_style_bg_color(fcLine[i], fg, 0);
    }

    // moon cut-outs use the background colour, so redraw icons
    if (lastForecastFetch)
        updateForecastScreen();
}

void createForecastScreen()
{
    forecastScreen = lv_obj_create(NULL);

    lv_obj_remove_flag(forecastScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(forecastScreen, gestureEvent, LV_EVENT_GESTURE, NULL);

    // ---------- back + title ----------

    lv_obj_t *backBtn = lv_button_create(forecastScreen);
    lv_obj_set_size(backBtn, 80, 36);
    lv_obj_align(backBtn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_event_cb(backBtn, [](lv_event_t *e) { goBackToWeather(); },
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(backLbl);

    fcTitle = lv_label_create(forecastScreen);
    lv_label_set_text(fcTitle, "5-Day Forecast");
    lv_obj_align(fcTitle, LV_ALIGN_TOP_LEFT, 110, 20);

    // ---------- status (loading / error) ----------

    fcStatus = lv_label_create(forecastScreen);
    lv_obj_set_width(fcStatus, 300);
    lv_obj_set_style_text_align(fcStatus, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(fcStatus, LV_ALIGN_TOP_MID, 0, 220);
    lv_obj_add_flag(fcStatus, LV_OBJ_FLAG_HIDDEN);

    // ---------- 5 rows:  Day | icon | hi / lo | rain ----------

    for (int i = 0; i < FC_DAYS; i++)
    {
        lv_obj_t *row = lv_obj_create(forecastScreen);

        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 320, FC_ROW_H);
        lv_obj_set_pos(row, 0, FC_ROW_Y0 + i * FC_ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        fcRow[i] = row;

        // thin divider at the top of each row
        fcLine[i] = lv_obj_create(row);
        lv_obj_remove_style_all(fcLine[i]);
        lv_obj_set_size(fcLine[i], 296, 1);
        lv_obj_set_pos(fcLine[i], 12, 0);
        lv_obj_set_style_bg_opa(fcLine[i], LV_OPA_30, 0);

        fcDay[i] = lv_label_create(row);
        lv_obj_set_style_text_font(fcDay[i], &lv_font_montserrat_28, 0);
        lv_obj_align(fcDay[i], LV_ALIGN_LEFT_MID, 12, 0);

        fcIcon[i] = lv_obj_create(row);
        lv_obj_remove_style_all(fcIcon[i]);
        lv_obj_set_size(fcIcon[i], (int)(90 * FC_ICON_K), (int)(70 * FC_ICON_K));
        lv_obj_align(fcIcon[i], LV_ALIGN_LEFT_MID, 98, 0);
        lv_obj_remove_flag(fcIcon[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(fcIcon[i], LV_OBJ_FLAG_CLICKABLE);

        fcTemps[i] = lv_label_create(row);
        lv_obj_set_style_text_font(fcTemps[i], &lv_font_montserrat_28, 0);
        lv_obj_align(fcTemps[i], LV_ALIGN_LEFT_MID, 160, 0);

        fcPop[i] = lv_label_create(row);
        lv_obj_set_style_text_align(fcPop[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(fcPop[i], LV_ALIGN_RIGHT_MID, -10, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    // forecast data is kept; only the screen objects go away
    lv_obj_add_event_cb(
        forecastScreen,
        [](lv_event_t *e) { forecastScreen = nullptr; },
        LV_EVENT_DELETE,
        NULL);

    // OpenWeather credit along the bottom, below the last row
    addCreditTicker(forecastScreen, 458, weatherTextColor);

    applyForecastColors();
}

void goToForecast()
{
    if (!forecastScreen)
        createForecastScreen();

    // coming from the Wind screen: delete it after the slide to save memory
    bool freeOld = (currentScreen == SCREEN_WIND);

    currentScreen = SCREEN_FORECAST;

    String key = weatherCity + "," + weatherState + "," + weatherCountry;

    bool stale =
        lastForecastFetch == 0 ||
        millis() - lastForecastFetch > FC_MAX_AGE_MS ||
        key != forecastPlaceKey;

    if (stale)
    {
        showForecastStatus("Loading forecast...");

        // fetch after the slide-in so the animation stays smooth
        forecastFetchPending = true;
        forecastRequestAt = millis();
    }
    else
    {
        lv_obj_add_flag(fcStatus, LV_OBJ_FLAG_HIDDEN);
        updateForecastScreen();
    }

    lv_screen_load_anim(
        forecastScreen,
        LV_SCR_LOAD_ANIM_OVER_LEFT,
        250,
        0,
        freeOld);
}

// call from loop()
void pollForecastFetch()
{
    if (!forecastFetchPending ||
        currentScreen != SCREEN_FORECAST ||
        millis() - forecastRequestAt < 350)
        return;

    forecastFetchPending = false;

    if (WiFi.status() != WL_CONNECTED)
    {
        showForecastStatus("No WiFi connection");
        return;
    }

    if (getForecast())
    {
        lastForecastFetch = millis();
        forecastPlaceKey = weatherCity + "," + weatherState + "," + weatherCountry;

        lv_obj_add_flag(fcStatus, LV_OBJ_FLAG_HIDDEN);
        updateForecastScreen();
    }
    else
    {
        showForecastStatus("Couldn't load the forecast\nTry again in a moment");
    }
}

// =====================================================
// MEMORY REPORT (Serial Monitor, every 10 s)
// =====================================================
// heap      = ESP32 free memory (HTTPS needs ~40 KB in one block)
// largest   = biggest single free block
// min       = lowest free heap seen since boot
// LVGL      = LVGL's own pool (only shown if it uses its built-in pool)

void reportMemory()
{
    static unsigned long last = 0;

    if (millis() - last < 10000)
        return;

    last = millis();

    Serial.printf(
        "[MEM] heap %u  largest %u  min %u",
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)ESP.getMinFreeHeap());

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    Serial.printf(
        "  |  LVGL used %u%%  free %u of %u  biggest %u",
        (unsigned)mon.used_pct,
        (unsigned)mon.free_size,
        (unsigned)mon.total_size,
        (unsigned)mon.free_biggest_size);
#endif

    Serial.println();
}

// =====================================================
// SD INIT
// =====================================================

void initSD()
{
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    // drop to 4000000 if your card is flaky
    sdReady = SD.begin(SD_CS, sdSPI, 20000000);

    if (sdReady)
        Serial.printf("TF card OK, %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
    else
        Serial.println("TF card mount failed");
}

// =====================================================
// SETUP
// =====================================================

void setup()
{
    Serial.begin(115200);

    initBacklight();      // PWM backlight, restores saved brightness

    gfx->begin();

    Wire.begin(33, 32);

    touch.begin();

    initSD();

    lv_init();

    display =
        lv_display_create(
            320,
            480);

    lv_display_set_flush_cb(
        display,
        my_flush_cb);

    drawBuf = (uint16_t *)heap_caps_malloc(
        DRAW_BUF_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!drawBuf)
    {
        Serial.println("FATAL: no memory for LVGL draw buffer");
        while (true) delay(1000);
    }

    lv_display_set_buffers(
        display,
        drawBuf,       // allocated just above
        NULL,
        DRAW_BUF_BYTES,
        LV_DISPLAY_RENDER_MODE_PARTIAL);

    touch_indev =
        lv_indev_create();

    lv_indev_set_type(
        touch_indev,
        LV_INDEV_TYPE_POINTER);

    lv_indev_set_read_cb(
        touch_indev,
        my_touch_read);

    loadWifiCredentials();
    loadCity();
    loadWeatherColors();

    createHomeScreen();
    createWeatherScreen();
    createSettingsScreen();
    createPicturesScreen();
    createWifiScreen();
    // City, Weather Page Color, Wind and Forecast screens are built
    // when first opened and deleted when you leave them, to keep
    // LVGL's memory free (running out caused LoadProhibited crashes).

    applyWeatherColors();     // saved colours onto the weather screen

    loadAutoDim();            // Auto Dim on/off from flash

    updateWeatherDisplay();

    lv_screen_load(homeScreen);

    Serial.println("UI READY");

    // Connect in the background so the Dashboard shows immediately.
    // pollWifiConnect() in loop() finishes the job, or opens
    // WiFi Setup if it fails.
    if (savedSSID.length() > 0)
    {
        wifiBootAttempt = true;
        beginWifiConnect(savedSSID, savedPass);

        // show "Connecting to WiFi" where the countdown will appear
        if (countdownLabelPtr)
        {
            lv_obj_t *cd = (lv_obj_t *)countdownLabelPtr;

            lv_label_set_text(cd, "Connecting to WiFi");
            lv_obj_remove_flag(cd, LV_OBJ_FLAG_HIDDEN);

            startThrob(cd);         // gentle pulsing while connecting
        }
    }
    else
    {
        goToWifiSetup();
    }
}

// =====================================================
// UPDATE CLOCK
// =====================================================
void updateClock()
{
    if(!timeLabel || !dateLabel)
        return;

    struct tm timeinfo;

    // FIX: timeout 0 = return immediately.
    // The default waits 5 SECONDS whenever the clock hasn't synced yet,
    // which froze the touch screen while offline.
    if(!getLocalTime(&timeinfo, 0))
    {
        // only touch the labels once, not every second
        static bool placeholderShown = false;

        if (!placeholderShown)
        {
            lv_label_set_text(
                timeLabel,
                "--:--");

            lv_label_set_text(
                dateLabel,
                "Waiting for NTP");

            placeholderShown = true;
        }

        return;
    }

    char timeBuffer[32];

    int hour = timeinfo.tm_hour;
    String ampm = "AM";

    if(hour >= 12)
    {
        ampm = "PM";
    }

    hour = hour % 12;

    if(hour == 0)
    {
        hour = 12;
    }

    sprintf(
    timeBuffer,
    "%d:%02d %s",
    hour,
    timeinfo.tm_min,
    ampm.c_str());

    char dateBuffer[64];


    strftime(
        dateBuffer,
        sizeof(dateBuffer),
        "%B %d, %Y",
        &timeinfo);

    lv_label_set_text(
        timeLabel,
        timeBuffer);

    lv_label_set_text(
        dateLabel,
        dateBuffer);
}

// =====================================================
// LOOP
// =====================================================

void loop()
{
    // FIX: skip weather while connecting, scanning or offline
    if (
        WiFi.status() == WL_CONNECTED &&
        !wifiConnecting &&
        !wifiScanRunning &&
        millis() - lastWeatherUpdate >
        weatherInterval)
    {
        lastWeatherUpdate =
            millis();

        if (getWeather())
        {
            updateWeatherDisplay();
        }
    }

    static uint32_t lastTick = millis();

    uint32_t now = millis();

    lv_tick_inc(now - lastTick);

    lastTick = now;

    lv_timer_handler();

    // WiFi setup: background scan + connect attempts
    pollWifiScan();
    pollWifiConnect();

    // Auto Dim: adjust brightness at sunrise / sunset
    checkAutoDim();

    // memory report to Serial Monitor every 10 s
    reportMemory();

    // download the 5-day forecast once its screen has slid in
    pollForecastFetch();

    // 10 s after a successful WiFi connection, switch to the weather screen,
    // with a 10 -> 0 countdown on the Dashboard while waiting
    if (weatherSwitchPending)
    {
        lv_obj_t *cd = (lv_obj_t *)countdownLabelPtr;
        static int lastShown = -1;

        unsigned long elapsed = millis() - wifiConnectedAt;

        int remaining =
            (elapsed >= WEATHER_SWITCH_DELAY)
                ? 0
                : (int)((WEATHER_SWITCH_DELAY - elapsed + 999) / 1000);

        // first second shows 10, last shows 1, then 0 as it switches
        if (cd && remaining != lastShown)
        {
            lastShown = remaining;

            stopThrob(cd);          // countdown is shown solid, not pulsing

            lv_label_set_text_fmt(cd, "Weather in %d", remaining);
            lv_obj_remove_flag(cd, LV_OBJ_FLAG_HIDDEN);
        }

        bool wifiLost = (WiFi.status() != WL_CONNECTED);

        if (elapsed >= WEATHER_SWITCH_DELAY || wifiLost)
        {
            weatherSwitchPending = false;
            lastShown = -1;

            if (!wifiLost && currentScreen != SCREEN_WEATHER)
                goToWeather();

            // leave "0" visible during the slide, then hide it
            if (cd)
            {
                lv_timer_t *t = lv_timer_create(
                    [](lv_timer_t *t)
                    {
                        lv_obj_add_flag((lv_obj_t *)countdownLabelPtr,
                                        LV_OBJ_FLAG_HIDDEN);
                    },
                    wifiLost ? 0 : 400,
                    NULL);

                lv_timer_set_repeat_count(t, 1);
            }
        }
    }

    // Draw the current picture once LVGL has finished
    // its own rendering of the (black) Pictures screen.
    if (pictureDrawPending &&
        currentScreen == SCREEN_PICTURES &&
        millis() - pictureRequestAt > 30)
    {
        pictureDrawPending = false;

        lv_refr_now(display);   // flush anything LVGL still had queued

        showPicture(pictureIndex);
    }

    if (millis() - lastClockUpdate > 1000)
    {
        lastClockUpdate = millis();

        updateClock();
    }

    delay(5);
}
// =====================================================
// UPDATE WEATHER DISPLAY
// =====================================================
void updateWeatherDisplay()
{
    if (!tempLabel)
        return;

    char buffer[32];

    sprintf(
        buffer,
        "%.0fF",
        currentTemp);

    lv_label_set_text(
        tempLabel,
        buffer);

    if (feelsLikeLabel)
    {
        sprintf(
            buffer,
            "Feels like %.0fF",
            currentFeelsLike);

        lv_label_set_text(
            feelsLikeLabel,
            buffer);
    }

    lv_label_set_text(
        cityNameLabel,
        cityWithState().c_str());

    sprintf(
        buffer,
        "Humidity %.0f%%",
        currentHumidity);

    lv_label_set_text(
        humidityLabel,
        buffer);

    if (sunriseLabel && sunsetLabel)
    {
        String rise = "Sunrise " + formatCityTime(currentSunrise);
        String set  = "Sunset "  + formatCityTime(currentSunset);

        lv_label_set_text(sunriseLabel, rise.c_str());
        lv_label_set_text(sunsetLabel,  set.c_str());
    }

    drawWeatherIcon(currentIconCode);

    updateWindScreen();
}
