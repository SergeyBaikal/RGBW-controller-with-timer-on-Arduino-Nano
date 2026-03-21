/*
  Управление RGBW лентой 24В через Arduino Nano
  с автоматической сменой цвета по расписанию
  Потапов С.А. poet1988@list.ru
  Версия 1.8.5 
  
  Библиотеки:
  - EncButton v3.7.4
  - GyverDS3231Min v1.2+
  - GyverPWM
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverDS3231Min.h>
#include <EncButton.h>
#include <EEPROM.h>
#include <GyverPWM.h>  

// =============== НАСТРОЙКИ КОМПИЛЯЦИИ ===============
#define EB_DEB_TIME 50
#define EB_CLICK_TIME 500
#define EB_HOLD_TIME 600
#define EB_STEP_TIME 200
#define EB_FAST_TIME 30
#define EB_TOUT_TIME 1000

// =============== НАСТРОЙКА ЧАСТОТЫ ШИМ ===============
// Выбор режима ШИМ:
// 0 - Fast PWM (максимальная частота, меньше мерцания)
// 1 - Phase-correct PWM (симметричный, меньше помех)
#define PWM_MODE 0

// Выбор предделителя (частоты):
// 1 - максимальная частота (62.5/31.4 кГц)
// 2 - средняя частота (7.8/4 кГц для Timer1, 8/4 кГц для Timer2)
// 3 - низкая частота (976/490 Гц для Timer1, 2к/980 Гц для Timer2)
#define PWM_PRESCALER 1

/*
   ТАБЛИЦА ЧАСТОТ ШИМ ДЛЯ ARDUINO NANO 
   _________________________________________________________________
  |       | Timer1 (пины 9 и 10) 8 bit | Timer2 (пины 3 и 11) 8 bit|
  |_______|____________________________|___________________________|
  |mode   | Phase-correct | Fast PWM   | Phase-correct | Fast PWM  |
  |_______|_______________|____________|_______________|___________|
  |1      | 31.4 kHz      | 62.5 kHz   | 31.4 kHz      | 62.5 kHz  |
  |2      | 4 kHz         | 7.8 kHz    | 4 kHz         | 8 kHz     |
  |3      | 490 Hz        | 976 Hz     | 980 Hz        | 2 kHz     |
  |_______|_______________|____________|_______________|___________|
  
  
*/

// =============== КОНСТАНТЫ И НАСТРОЙКИ ===============

const char FIRMWARE_VERSION[] = "1.8.5";

// Настройки дисплея
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

// Настройки энкодера
#define ENC_CLK 4      // D4 - CLK энкодера
#define ENC_DT 5       // D5 - DT энкодера 
#define ENC_SW 6       // D6 - SW энкодера (кнопка)

// Пины ШИМ
#define PIN_RED 3      // D3 - КРАСНЫЙ канал (Timer2)
#define PIN_GREEN 9    // D9 - ЗЕЛЁНЫЙ канал (Timer1)
#define PIN_BLUE 10    // D10 - СИНИЙ канал (Timer1)
#define PIN_WHITE 11   // D11 - БЕЛЫЙ канал (Timer2)

// Кнопка сброса
#define RESET_BUTTON 7

// Настройки EEPROM
#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0xAA56
#define EEPROM_SAVE_DELAY 5000

#define MAX_STEPS 6
#define TIMEZONE 3
#define ENCODER_TYPE EB_STEP4_LOW

// =============== СТРУКТУРЫ ДАННЫХ ===============

enum Color {
  COLOR_RED,
  COLOR_GREEN,
  COLOR_BLUE,
  COLOR_WHITE,
  COLOR_DARK,
  COLOR_SKIP,
  COLOR_COUNT
};

struct Settings {
  uint16_t magic;
  uint8_t version;
  Color chain[MAX_STEPS];
  uint8_t brightness[4];
  uint16_t durations[MAX_STEPS];
  bool repeat;
  uint8_t backlightTimeout;
};

struct RunState {
  uint8_t currentStep;
  bool active;
  uint32_t stepStartUnix;
  
  RunState() : currentStep(0), active(false), stepStartUnix(0) {}
} runState;

struct Flags {
  uint8_t displayOn : 1;
  uint8_t rtcOk : 1;
  uint8_t settingsChanged : 1;
  uint8_t showChain : 1;
  uint8_t inSubMenu : 1;
  uint8_t editingValue : 1;
  uint8_t rtcMessageShown : 1;
  uint8_t rtcTimeValid : 1;
  uint8_t testRunning : 1;
} flags;

// =============== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ===============

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
GyverDS3231Min rtc;
EncButton enc(ENC_CLK, ENC_DT, ENC_SW);

Settings settings;

enum MenuState {
  MENU_MAIN,
  MENU_CHAIN,
  MENU_BRIGHTNESS,
  MENU_TIME,
  MENU_CLOCK,
  MENU_REPEAT,
  MENU_DISPLAY,
  MENU_RUN,
  MENU_TEST
};

MenuState currentMenu = MENU_MAIN;
uint8_t menuCursor = 0;
uint8_t subMenuCursor = 0;
uint8_t editPart = 0;
uint8_t editHours = 0;
uint8_t editMinutes = 0;
uint8_t editValue = 0;

// Таймеры
unsigned long displayTimer = 0;
unsigned long saveTimer = 0;
unsigned long rtcCheckTimer = 0;
unsigned long runUpdateTimer = 0;
unsigned long runDisplayTimer = 0;
unsigned long testTimer = 0;
unsigned long userActivity = 0;
unsigned long chainShowTime = 0;

// Для быстрого обновления при редактировании
unsigned long lastEditUpdate = 0;
const unsigned long EDIT_UPDATE_INTERVAL = 50;

// Буферы
char lineBuffer[17];

// Флаги обновления
uint8_t lastHour = 255;
uint8_t lastMinute = 255;
uint16_t lastRemainingTime = 65535;
bool displayChanged = true;
bool partialUpdate = false;
bool forceUpdate = false;

// =============== ПРОТОТИПЫ ФУНКЦИЙ ===============

void initSystem();
void initDisplay();
void initPWM();
void loadSettings();
void saveSettings();
void resetSettings();
bool checkRTC();
void checkUnixTime();

void setColor(Color color, bool forceBrightness = false);
void setChannel(uint8_t pin, uint8_t brightness);
void forceAllPinsOff();
void runTestSequence();

void handleEncoder();
void updateDisplay();
void drawMainMenu();
void drawChainMenu();
void drawBrightnessMenu();
void drawTimeMenu();
void drawClockMenu();
void drawRepeatMenu();
void drawDisplayMenu();
void drawRunScreen();
void drawTestScreen();
void drawPartialRunScreen();
void drawChainOverview();

char colorToChar(Color color);
void formatTime(uint16_t minutes, char* buffer);
uint16_t calculateRemainingTime();

void runScheduler();
void advanceChain();
void startRunMode();
void stopRunMode();

// =============== ИНИЦИАЛИЗАЦИЯ ===============

void setup() {
  Serial.begin(115200);
  Serial.print(F("RGBW Controller v"));
  Serial.println(FIRMWARE_VERSION);
  
  // Инициализация пинов
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_WHITE, OUTPUT);
  
  // Принудительно выключаем все пины
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_BLUE, LOW);
  digitalWrite(PIN_WHITE, LOW);
  
  pinMode(RESET_BUTTON, INPUT_PULLUP);
  
  // Инициализация ШИМ
  initPWM();
  
  Wire.begin();
  initDisplay();
  
  setStampZone(TIMEZONE);
  
  flags.rtcOk = false;
  flags.rtcTimeValid = false;
  
  if (rtc.begin()) {
    delay(100);
    if (checkRTC()) {
      flags.rtcOk = true;
      checkUnixTime();
    }
  }
  
  enc.setEncType(ENCODER_TYPE);
  
  loadSettings();
  
  runState.active = false;
  runState.currentStep = 0;
  
  unsigned long now = millis();
  displayTimer = now;
  saveTimer = now;
  rtcCheckTimer = now;
  runUpdateTimer = now;
  runDisplayTimer = now;
  userActivity = now;
  
  flags.displayOn = true;
  flags.showChain = false;
  flags.rtcMessageShown = false;
  flags.testRunning = false;
  
  lcd.clear();
  if (flags.rtcOk) {
    lcd.print(F("RTC OK"));
    lcd.setCursor(0, 1);
    lcd.print(F("Unix: "));
    lcd.print(flags.rtcTimeValid ? F("OK") : F("FAIL"));
  } else {
    lcd.print(F("RTC ERROR!"));
  }
  delay(2000);
  
  if (flags.rtcOk && !flags.rtcTimeValid) {
    lcd.clear();
    lcd.print(F("Set build time"));
    rtc.setBuildTime();
    delay(1500);
    checkUnixTime();
  }
  
  lcd.clear();
  flags.rtcMessageShown = true;
  
  // Убеждаемся, что все каналы выключены
  forceAllPinsOff();
  
  Serial.println(F("System initialized"));
}

// =============== ИНИЦИАЛИЗАЦИЯ ШИМ ===============

void initPWM() {
  // Устанавливаем Timer1 (пины 9 и 10) в 8-битный режим
  PWM_TMR1_8BIT();
  
  // Настройка Timer1 (пины 9,10)
  PWM_mode(PIN_GREEN, PWM_MODE);
  PWM_mode(PIN_BLUE, PWM_MODE);
  PWM_prescaler(PIN_GREEN, PWM_PRESCALER);
  PWM_prescaler(PIN_BLUE, PWM_PRESCALER);
  
  // Настройка Timer2 (пины 3,11)
  PWM_mode(PIN_RED, PWM_MODE);
  PWM_mode(PIN_WHITE, PWM_MODE);
  PWM_prescaler(PIN_RED, PWM_PRESCALER);
  PWM_prescaler(PIN_WHITE, PWM_PRESCALER);
  
  // Сбрасываем регистры Timer2 в 0
  OCR2A = 0;
  OCR2B = 0;
  
  // Инициализация всех каналов в 0
  PWM_set(PIN_RED, 0);
  PWM_set(PIN_GREEN, 0);
  PWM_set(PIN_BLUE, 0);
  PWM_set(PIN_WHITE, 0);
  
  // Вывод информации о частотах
  Serial.println(F("=== PWM Configuration ==="));
  Serial.print(F("Mode: "));
  Serial.println(PWM_MODE ? F("Phase-correct") : F("Fast PWM"));
  Serial.print(F("Prescaler: "));
  Serial.println(PWM_PRESCALER);
  
  Serial.println(F("Frequencies:"));
  
  // Timer1 (9,10) - 8-bit
  Serial.print(F("  D9,D10 (Timer1): "));
  if (PWM_MODE == 0) { // Fast PWM
    switch(PWM_PRESCALER) {
      case 1: Serial.println(F("62.5 kHz")); break;
      case 2: Serial.println(F("7.8 kHz")); break;
      case 3: Serial.println(F("976 Hz")); break;
    }
  } else { // Phase-correct
    switch(PWM_PRESCALER) {
      case 1: Serial.println(F("31.4 kHz")); break;
      case 2: Serial.println(F("4 kHz")); break;
      case 3: Serial.println(F("490 Hz")); break;
    }
  }
  
  // Timer2 (3,11)
  Serial.print(F("  D3,D11 (Timer2): "));
  if (PWM_MODE == 0) { // Fast PWM
    switch(PWM_PRESCALER) {
      case 1: Serial.println(F("62.5 kHz")); break;
      case 2: Serial.println(F("8 kHz")); break;
      case 3: Serial.println(F("2 kHz")); break;
    }
  } else { // Phase-correct
    switch(PWM_PRESCALER) {
      case 1: Serial.println(F("31.4 kHz")); break;
      case 2: Serial.println(F("4 kHz")); break;
      case 3: Serial.println(F("980 Hz")); break;
    }
  }
  Serial.println(F("========================"));
}

// =============== ИНИЦИАЛИЗАЦИЯ ДИСПЛЕЯ ===============

void initDisplay() {
  Wire.beginTransmission(LCD_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println(F("LCD not found!"));
  }
  
  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  lcd.setCursor(0, 0);
  lcd.print(F("RGBW Controller"));
  lcd.setCursor(0, 1);
  lcd.print(F("v"));
  lcd.print(FIRMWARE_VERSION);
  
  delay(1500);
}

// =============== РАБОТА С EEPROM ===============

void loadSettings() {
  EEPROM.get(0, settings);
  
  if (settings.magic != EEPROM_MAGIC || settings.version != 2) {
    Serial.println(F("Invalid settings, loading defaults"));
    resetSettings();
    return;
  }
  
  Serial.println(F("Settings loaded"));
}

void saveSettings() {
  settings.magic = EEPROM_MAGIC;
  settings.version = 2;
  
  EEPROM.put(0, settings);
  
  Settings verify;
  EEPROM.get(0, verify);
  
  if (memcmp(&settings, &verify, sizeof(Settings)) == 0) {
    flags.settingsChanged = false;
    Serial.println(F("Settings saved"));
  }
}

void resetSettings() {
  settings.magic = EEPROM_MAGIC;
  settings.version = 2;
  
  settings.chain[0] = COLOR_RED;
  settings.chain[1] = COLOR_GREEN;
  settings.chain[2] = COLOR_BLUE;
  settings.chain[3] = COLOR_WHITE;
  settings.chain[4] = COLOR_DARK;
  settings.chain[5] = COLOR_SKIP;
  
  for (int i = 0; i < 4; i++) {
    settings.brightness[i] = 128;
  }
  
  for (int i = 0; i < MAX_STEPS; i++) {
    settings.durations[i] = 120;
  }
  
  settings.repeat = true;
  settings.backlightTimeout = 1;
  
  flags.settingsChanged = true;
  saveSettings();
}

// =============== УПРАВЛЕНИЕ СВЕТОМ ===============

void setColor(Color color, bool forceBrightness) {
  // Сначала выключаем все каналы
  if (color == COLOR_DARK || color == COLOR_SKIP) {
    forceAllPinsOff();
    return;
  }
  
  uint8_t brightness = forceBrightness ? 255 : settings.brightness[color];
  
  // В зависимости от цвета включаем нужный канал
  switch (color) {
    case COLOR_RED:
      setChannel(PIN_RED, brightness);
      setChannel(PIN_GREEN, 0);
      setChannel(PIN_BLUE, 0);
      setChannel(PIN_WHITE, 0);
      break;
      
    case COLOR_GREEN:
      setChannel(PIN_RED, 0);
      setChannel(PIN_GREEN, brightness);
      setChannel(PIN_BLUE, 0);
      setChannel(PIN_WHITE, 0);
      break;
      
    case COLOR_BLUE:
      setChannel(PIN_RED, 0);
      setChannel(PIN_GREEN, 0);
      setChannel(PIN_BLUE, brightness);
      setChannel(PIN_WHITE, 0);
      break;
      
    case COLOR_WHITE:
      setChannel(PIN_RED, 0);
      setChannel(PIN_GREEN, 0);
      setChannel(PIN_BLUE, 0);
      setChannel(PIN_WHITE, brightness);
      break;
      
    default:
      forceAllPinsOff();
      break;
  }
}

void setChannel(uint8_t pin, uint8_t brightness) {
  // Для всех пинов используем PWM_set
  PWM_set(pin, brightness);
  
  // Для Timer2 дополнительно обновляем регистры для надёжности
  if (pin == PIN_RED) {
    OCR2B = brightness;
  } else if (pin == PIN_WHITE) {
    OCR2A = brightness;
  }
}

void forceAllPinsOff() {
  // Выключаем через PWM_set
  PWM_set(PIN_RED, 0);
  PWM_set(PIN_GREEN, 0);
  PWM_set(PIN_BLUE, 0);
  PWM_set(PIN_WHITE, 0);
  
  // Сбрасываем регистры Timer2
  OCR2A = 0;
  OCR2B = 0;
  
  // Дополнительно digitalWrite для гарантии
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_BLUE, LOW);
  digitalWrite(PIN_WHITE, LOW);
}

void runTestSequence() {
  if (!flags.testRunning) {
    flags.testRunning = true;
    testTimer = millis();
    currentMenu = MENU_TEST;
  }
}

// =============== ПРОВЕРКА RTC ===============

bool checkRTC() {
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() != 0) return false;
  
  Wire.requestFrom((uint8_t)0x68, (uint8_t)3);
  if (Wire.available() < 3) return false;
  
  uint8_t sec = Wire.read();
  uint8_t min = Wire.read();
  uint8_t hour = Wire.read();
  
  if ((sec & 0x7F) > 0x59) return false;
  if (min > 0x59) return false;
  if ((hour & 0x3F) > 0x23) return false;
  
  return true;
}

void checkUnixTime() {
  uint32_t unixTime = rtc.getUnix();
  flags.rtcTimeValid = (unixTime > 1700000000);
  
  Serial.print(F("Unix time: "));
  Serial.println(unixTime);
}

// =============== ОБРАБОТКА ЭНКОДЕРА ===============

void handleEncoder() {
  if (enc.turn()) {
    int8_t change = enc.dir();
    
    if (flags.editingValue) {
      if (currentMenu == MENU_CLOCK || currentMenu == MENU_TIME) {
        if (editPart == 0) {
          if (currentMenu == MENU_CLOCK) {
            editHours = (editHours + change + 24) % 24;
          } else {
            editHours = constrain(editHours + change, 0, 99);
          }
        } else {
          editMinutes = (editMinutes + change + 60) % 60;
        }
        partialUpdate = true;
        forceUpdate = true;
        
      } else if (currentMenu == MENU_BRIGHTNESS) {
        editValue = constrain(editValue + change, 0, 255);
        
        // Обновляем соответствующий канал
        uint8_t pin;
        switch (subMenuCursor) {
          case 0: pin = PIN_RED; break;
          case 1: pin = PIN_GREEN; break;
          case 2: pin = PIN_BLUE; break;
          case 3: pin = PIN_WHITE; break;
          default: return;
        }
        setChannel(pin, editValue);
        
        partialUpdate = true;
        forceUpdate = true;
      }
      
    } else if (flags.inSubMenu) {
      if (currentMenu == MENU_CHAIN || currentMenu == MENU_TIME) {
        subMenuCursor = (subMenuCursor + change + MAX_STEPS) % MAX_STEPS;
      } else if (currentMenu == MENU_BRIGHTNESS) {
        subMenuCursor = (subMenuCursor + change + 4) % 4;
      }
      displayChanged = true;
      
    } else {
      menuCursor = (menuCursor + change + 8) % 8;
      displayChanged = true;
    }
  }
  
  if (enc.click()) {
    if (currentMenu == MENU_RUN) {
      flags.showChain = true;
      chainShowTime = millis();
      displayChanged = true;
      
    } else if (currentMenu == MENU_TEST && !flags.testRunning) {
      runTestSequence();
      
    } else if (!flags.inSubMenu && currentMenu == MENU_MAIN) {
      flags.inSubMenu = true;
      subMenuCursor = 0;
      flags.editingValue = false;
      editPart = 0;
      
      switch (menuCursor) {
        case 0: currentMenu = MENU_CHAIN; break;
        case 1: currentMenu = MENU_BRIGHTNESS; break;
        case 2: currentMenu = MENU_TIME; break;
        case 3: currentMenu = MENU_REPEAT; break;
        case 4: 
          currentMenu = MENU_RUN; 
          if (!runState.active && flags.rtcOk) startRunMode();
          break;
        case 5: currentMenu = MENU_CLOCK; break;
        case 6: currentMenu = MENU_TEST; break;
        case 7: currentMenu = MENU_DISPLAY; break;
      }
      displayChanged = true;
      
    } else if (flags.editingValue) {
      if (currentMenu == MENU_CLOCK || currentMenu == MENU_TIME) {
        if (editPart == 0) {
          editPart = 1;
          partialUpdate = true;
          forceUpdate = true;
        } else {
          flags.editingValue = false;
          
          if (currentMenu == MENU_CLOCK && flags.rtcOk) {
            Datime dt = rtc.getTime();
            dt.hour = editHours;
            dt.minute = editMinutes;
            dt.second = 0;
            rtc.setTime(dt);
            flags.settingsChanged = true;
            
          } else if (currentMenu == MENU_TIME) {
            if (settings.chain[subMenuCursor] == COLOR_SKIP) {
              settings.durations[subMenuCursor] = 0;
            } else {
              settings.durations[subMenuCursor] = editHours * 60 + editMinutes;
            }
            flags.settingsChanged = true;
          }
          
          editPart = 0;
          displayChanged = true;
        }
      } else {
        exitEditing();
      }
      
    } else {
      enterEditing();
      displayChanged = true;
    }
  }
  
  if (enc.hold()) {
    if (currentMenu == MENU_RUN && runState.active) {
      stopRunMode();
      lcd.clear();
      lcd.print(F("STOP JOB"));
      delay(2000);
      currentMenu = MENU_MAIN;
      flags.inSubMenu = false;
      displayChanged = true;
      
    } else if (currentMenu == MENU_TEST && flags.testRunning) {
      flags.testRunning = false;
      forceAllPinsOff();
      setColor(COLOR_DARK);
      currentMenu = MENU_TEST;
      displayChanged = true;
      
    } else if (flags.inSubMenu || flags.editingValue) {
      flags.editingValue = false;
      flags.inSubMenu = false;
      editPart = 0;
      currentMenu = MENU_MAIN;
      setColor(COLOR_DARK);
      displayChanged = true;
    }
  }
}

void enterEditing() {
  flags.editingValue = true;
  
  if (currentMenu == MENU_BRIGHTNESS) {
    editValue = settings.brightness[subMenuCursor];
    setColor(COLOR_DARK);
    
    // Включаем только выбранный канал
    uint8_t pin;
    switch (subMenuCursor) {
      case 0: pin = PIN_RED; break;
      case 1: pin = PIN_GREEN; break;
      case 2: pin = PIN_BLUE; break;
      case 3: pin = PIN_WHITE; break;
      default: return;
    }
    setChannel(pin, editValue);
    
  } else if (currentMenu == MENU_CLOCK) {
    if (flags.rtcOk) {
      Datime dt = rtc.getTime();
      editHours = dt.hour;
      editMinutes = dt.minute;
    }
    
  } else if (currentMenu == MENU_TIME) {
    uint16_t duration = settings.durations[subMenuCursor];
    editHours = duration / 60;
    editMinutes = duration % 60;
    if (settings.chain[subMenuCursor] == COLOR_SKIP) {
      editHours = 0;
      editMinutes = 0;
    }
    
  } else if (currentMenu == MENU_CHAIN) {
    settings.chain[subMenuCursor] = (Color)((settings.chain[subMenuCursor] + 1) % COLOR_COUNT);
    flags.settingsChanged = true;
    flags.editingValue = false;
    
  } else if (currentMenu == MENU_REPEAT) {
    settings.repeat = !settings.repeat;
    flags.settingsChanged = true;
    flags.editingValue = false;
    
  } else if (currentMenu == MENU_DISPLAY) {
    settings.backlightTimeout = (settings.backlightTimeout + 1) % 4;
    flags.displayOn = true;
    lcd.backlight();
    flags.settingsChanged = true;
    flags.editingValue = false;
  }
}

void exitEditing() {
  flags.editingValue = false;
  
  if (currentMenu == MENU_BRIGHTNESS) {
    settings.brightness[subMenuCursor] = editValue;
    flags.settingsChanged = true;
    setColor(COLOR_DARK);
  }
  
  editPart = 0;
  displayChanged = true;
}

// =============== ФУНКЦИИ ОТОБРАЖЕНИЯ ===============

void drawMainMenu() {
  const char* menuItems[] = {
    "CHAIN", "BRIGHTNESS", "TIME", "REPEAT",
    "RUN", "CLOCK", "TEST", "DISPLAY"
  };
  
  lcd.setCursor(0, 0);
  lcd.print('>');
  lcd.print(menuItems[menuCursor]);
  
  lcd.setCursor(0, 1);
  uint8_t nextItem = (menuCursor + 1) % 8;
  lcd.print(' ');
  lcd.print(menuItems[nextItem]);
}

void drawChainMenu() {
  lcd.setCursor(0, 0);
  lcd.print(F("CHAIN"));
  
  lcd.setCursor(0, 1);
  sprintf(lineBuffer, "%d#: %c", subMenuCursor + 1, 
          colorToChar(settings.chain[subMenuCursor]));
  lcd.print(lineBuffer);
}

void drawBrightnessMenu() {
  lcd.setCursor(0, 0);
  lcd.print(F("BRIGHTNESS"));
  
  lcd.setCursor(0, 1);
  if (flags.editingValue) {
    sprintf(lineBuffer, "Value: %3d", editValue);
  } else {
    const char* colors[] = {"R", "G", "B", "W"};
    sprintf(lineBuffer, "%s: %3d", colors[subMenuCursor],
            settings.brightness[subMenuCursor]);
  }
  lcd.print(lineBuffer);
}

void drawTimeMenu() {
  lcd.setCursor(0, 0);
  lcd.print(F("TIME SETUP"));
  
  lcd.setCursor(0, 1);
  if (flags.editingValue) {
    if (settings.chain[subMenuCursor] == COLOR_SKIP) {
      sprintf(lineBuffer, "00:00  SKIP");
    } else {
      sprintf(lineBuffer, "%02d:%02d %c", editHours, editMinutes,
              editPart == 0 ? 'H' : 'M');
    }
  } else {
    uint16_t duration = settings.durations[subMenuCursor];
    if (settings.chain[subMenuCursor] == COLOR_SKIP) {
      sprintf(lineBuffer, "SKIP  %d#:%c", subMenuCursor + 1,
              colorToChar(settings.chain[subMenuCursor]));
    } else {
      formatTime(duration, lineBuffer);
      sprintf(lineBuffer + 5, " %d#:%c", subMenuCursor + 1,
              colorToChar(settings.chain[subMenuCursor]));
    }
  }
  lcd.print(lineBuffer);
}

void drawClockMenu() {
  lcd.setCursor(0, 0);
  lcd.print(F("SET CLOCK"));
  
  lcd.setCursor(0, 1);
  if (!flags.rtcOk) {
    lcd.print(F("RTC ERROR"));
    return;
  }
  
  if (flags.editingValue) {
    sprintf(lineBuffer, "%02d:%02d %c", editHours, editMinutes,
            editPart == 0 ? 'H' : 'M');
  } else {
    Datime dt = rtc.getTime();
    sprintf(lineBuffer, "%02d:%02d", dt.hour, dt.minute);
  }
  lcd.print(lineBuffer);
}

void drawRepeatMenu() {
  lcd.setCursor(0, 0);
  lcd.print(F("REPEAT"));
  
  lcd.setCursor(0, 1);
  lcd.print(settings.repeat ? F("YES") : F("NO"));
}

void drawDisplayMenu() {
  lcd.setCursor(0, 0);
  lcd.print(F("DISPLAY"));
  
  lcd.setCursor(0, 1);
  const char* modes[] = {"OFF", "15MIN", "1HOUR", "ALWAYS"};
  lcd.print(modes[settings.backlightTimeout]);
}

void drawRunScreen() {
  if (!flags.rtcOk) {
    lcd.setCursor(0, 0);
    lcd.print(F("RTC ERROR"));
    lcd.setCursor(0, 1);
    lcd.print(F("Check module"));
    return;
  }
  
  Datime dt = rtc.getTime();
  
  sprintf(lineBuffer, "RUN %02d:%02d", dt.hour, dt.minute);
  lcd.setCursor(0, 0);
  lcd.print(lineBuffer);
  
  drawRunStatus();
}

void drawRunStatus() {
  lcd.setCursor(0, 1);
  lcd.print(F("                "));
  lcd.setCursor(0, 1);
  
  if (runState.active && runState.currentStep < MAX_STEPS) {
    if (settings.chain[runState.currentStep] == COLOR_SKIP) {
      lcd.print("SKIP  ");
      lcd.print(runState.currentStep + 1);
      lcd.print("#:X");
    } else {
      uint16_t remaining = calculateRemainingTime();
      char timeStr[6];
      formatTime(remaining, timeStr);
      
      lcd.print(timeStr);
      lcd.print(" ");
      lcd.print(runState.currentStep + 1);
      lcd.print("#:");
      lcd.print(colorToChar(settings.chain[runState.currentStep]));
    }
  } else {
    lcd.print("00:00  0#:X");
  }
}

void drawPartialRunScreen() {
  if (!flags.rtcOk) return;
  
  Datime dt = rtc.getTime();
  
  if (dt.hour != lastHour || dt.minute != lastMinute) {
    lastHour = dt.hour;
    lastMinute = dt.minute;
    
    sprintf(lineBuffer, "RUN %02d:%02d", dt.hour, dt.minute);
    lcd.setCursor(0, 0);
    lcd.print(lineBuffer);
  }
  
  if (runState.active) {
    uint16_t remaining = calculateRemainingTime();
    if (remaining != lastRemainingTime) {
      lastRemainingTime = remaining;
      drawRunStatus();
    }
  }
}

void drawChainOverview() {
  lcd.clear();
  lcd.print(F("CHAIN:"));
  lcd.setCursor(0, 1);
  for (int i = 0; i < MAX_STEPS; i++) {
    lcd.print(colorToChar(settings.chain[i]));
    if (i < MAX_STEPS - 1) lcd.print('-');
  }
}

void drawTestScreen() {
  if (!flags.testRunning) {
    lcd.setCursor(0, 0);
    lcd.print(F("TEST MODE"));
    lcd.setCursor(0, 1);
    lcd.print(F("Click to test"));
  }
}

// =============== УТИЛИТЫ ===============

char colorToChar(Color color) {
  switch (color) {
    case COLOR_RED: return 'R';
    case COLOR_GREEN: return 'G';
    case COLOR_BLUE: return 'B';
    case COLOR_WHITE: return 'W';
    case COLOR_DARK: return 'D';
    case COLOR_SKIP: return 'X';
    default: return '?';
  }
}

void formatTime(uint16_t minutes, char* buffer) {
  uint8_t hours = minutes / 60;
  uint8_t mins = minutes % 60;
  sprintf(buffer, "%02d:%02d", hours, mins);
}

uint16_t calculateRemainingTime() {
  if (!runState.active || !flags.rtcOk || settings.chain[runState.currentStep] == COLOR_SKIP) {
    return 0;
  }
  
  uint32_t currentUnix = rtc.getUnix();
  uint32_t elapsedSeconds = currentUnix - runState.stepStartUnix;
  uint32_t durationSeconds = (uint32_t)settings.durations[runState.currentStep] * 60UL;
  
  if (elapsedSeconds >= durationSeconds) {
    return 0;
  }
  
  uint32_t remainingSeconds = durationSeconds - elapsedSeconds;
  return (remainingSeconds + 59) / 60;
}

// =============== УПРАВЛЕНИЕ РЕЖИМОМ RUN ===============

void startRunMode() {
  if (!flags.rtcOk) {
    Serial.println(F("Cannot start RUN: RTC error"));
    lcd.clear();
    lcd.print(F("RTC ERROR"));
    lcd.setCursor(0, 1);
    lcd.print(F("Can't start"));
    delay(2000);
    return;
  }
  
  runState.active = true;
  runState.currentStep = 0;
  
  // Пропускаем COLOR_SKIP в начале
  while (runState.currentStep < MAX_STEPS - 1 && 
         settings.chain[runState.currentStep] == COLOR_SKIP) {
    runState.currentStep++;
  }
  
  // Если все шаги SKIP, останавливаем
  if (settings.chain[runState.currentStep] == COLOR_SKIP) {
    stopRunMode();
    return;
  }
  
  runState.stepStartUnix = rtc.getUnix();
  setColor(settings.chain[runState.currentStep]);
  
  lastRemainingTime = 65535;
  lastHour = 255;
  lastMinute = 255;
  
  Serial.println(F("RUN started"));
}

void stopRunMode() {
  runState.active = false;
  runState.currentStep = 0;
  forceAllPinsOff();
  
  Serial.println(F("RUN stopped"));
}

void runScheduler() {
  if (!flags.rtcOk) {
    if (runState.active) stopRunMode();
    return;
  }
  
  if (!runState.active) return;
  
  // Проверка на SKIP или нулевую длительность
  if (settings.chain[runState.currentStep] == COLOR_SKIP || 
      settings.durations[runState.currentStep] == 0) {
    advanceChain();
    return;
  }
  
  uint32_t currentUnix = rtc.getUnix();
  uint32_t elapsedSeconds = currentUnix - runState.stepStartUnix;
  uint32_t durationSeconds = (uint32_t)settings.durations[runState.currentStep] * 60UL;
  
  if (elapsedSeconds >= durationSeconds) {
    advanceChain();
  }
}

void advanceChain() {
  if (!flags.rtcOk) return;
  
  uint8_t startStep = runState.currentStep;
  uint8_t stepsChecked = 0;
  
  do {
    runState.currentStep = (runState.currentStep + 1) % MAX_STEPS;
    stepsChecked++;
    
    // Проверка на зацикливание (все шаги SKIP)
    if (stepsChecked > MAX_STEPS) {
      stopRunMode();
      return;
    }
    
    // Если вернулись к началу и repeat выключен - останавливаем
    if (runState.currentStep == 0 && !settings.repeat) {
      stopRunMode();
      return;
    }
    
  } while (settings.chain[runState.currentStep] == COLOR_SKIP);
  
  runState.stepStartUnix = rtc.getUnix();
  setColor(settings.chain[runState.currentStep]);
  
  lastRemainingTime = 65535;
}

// =============== ОСНОВНОЙ ЦИКЛ ===============

void loop() {
  unsigned long now = millis();
  
  // ===== ЭНКОДЕР =====
  enc.tick();
  
  if (enc.turn() || enc.click() || enc.hold()) {
    userActivity = now;
    
    if (!flags.displayOn) {
      flags.displayOn = true;
      lcd.backlight();
      displayChanged = true;
    }
    
    handleEncoder();
    
    if (flags.editingValue) {
      forceUpdate = true;
      lastEditUpdate = now;
    }
  }
  
  // ===== ДИСПЛЕЙ =====
  bool needUpdate = false;
  
  if (flags.editingValue) {
    if (forceUpdate || (now - lastEditUpdate >= EDIT_UPDATE_INTERVAL)) {
      needUpdate = true;
      lastEditUpdate = now;
      forceUpdate = false;
    }
  } else if (now - displayTimer >= 200) {
    needUpdate = true;
    displayTimer = now;
  }
  
  if (needUpdate) {
    if (flags.showChain) {
      if (now - chainShowTime > 5000) {
        flags.showChain = false;
        displayChanged = true;
      }
    }
    
    if (displayChanged) {
      lcd.clear();
      
      if (currentMenu == MENU_RUN && flags.showChain) {
        drawChainOverview();
      } else {
        switch (currentMenu) {
          case MENU_MAIN: drawMainMenu(); break;
          case MENU_CHAIN: drawChainMenu(); break;
          case MENU_BRIGHTNESS: drawBrightnessMenu(); break;
          case MENU_TIME: drawTimeMenu(); break;
          case MENU_CLOCK: drawClockMenu(); break;
          case MENU_REPEAT: drawRepeatMenu(); break;
          case MENU_DISPLAY: drawDisplayMenu(); break;
          case MENU_RUN: drawRunScreen(); break;
          case MENU_TEST: drawTestScreen(); break;
        }
      }
      
      displayChanged = false;
      partialUpdate = false;
      
    } else if (partialUpdate) {
      if (currentMenu == MENU_RUN) {
        drawPartialRunScreen();
      } else if (currentMenu == MENU_MAIN) {
        lcd.setCursor(0, 0);
        lcd.print(' ');
        lcd.setCursor(0, 1);
        lcd.print(' ');
        lcd.setCursor(0, menuCursor % 2);
        lcd.print('>');
      } else {
        lcd.clear();
        switch (currentMenu) {
          case MENU_BRIGHTNESS: drawBrightnessMenu(); break;
          case MENU_TIME: drawTimeMenu(); break;
          case MENU_CLOCK: drawClockMenu(); break;
          default: break;
        }
      }
      partialUpdate = false;
    }
  }
  
  // ===== СОХРАНЕНИЕ НАСТРОЕК =====
  if (flags.settingsChanged && (now - saveTimer >= EEPROM_SAVE_DELAY)) {
    saveTimer = now;
    saveSettings();
  }
  
  // ===== ПОДСВЕТКА =====
  static unsigned long lastBacklightCheck = 0;
  if (now - lastBacklightCheck >= 1000) {
    lastBacklightCheck = now;
    
    if (flags.displayOn && settings.backlightTimeout > 0 && settings.backlightTimeout < 3) {
      unsigned long timeout = (settings.backlightTimeout == 1) ? 900000 : 3600000;
      if (now - userActivity > timeout) {
        flags.displayOn = false;
        lcd.noBacklight();
      }
    }
  }
  
  // ===== РЕЖИМ RUN =====
  if (currentMenu == MENU_RUN && flags.rtcOk) {
    if (now - runUpdateTimer >= 1000) {
      runUpdateTimer = now;
      runScheduler();
    }
    if (now - runDisplayTimer >= 500) {
      runDisplayTimer = now;
      partialUpdate = true;
    }
  }
  
  // ===== ТЕСТ =====
  if (flags.testRunning) {
    if (now - testTimer >= 1000) {
      testTimer = now;
      
      static uint8_t testStep = 0;
      switch (testStep) {
        case 0:
          lcd.clear();
          lcd.print(F("Test RED"));
          setColor(COLOR_RED, true);
          testStep = 1;
          break;
        case 1:
          lcd.clear();
          lcd.print(F("Test GREEN"));
          setColor(COLOR_GREEN, true);
          testStep = 2;
          break;
        case 2:
          lcd.clear();
          lcd.print(F("Test BLUE"));
          setColor(COLOR_BLUE, true);
          testStep = 3;
          break;
        case 3:
          lcd.clear();
          lcd.print(F("Test WHITE"));
          setColor(COLOR_WHITE, true);
          testStep = 4;
          break;
        case 4:
          forceAllPinsOff();
          flags.testRunning = false;
          currentMenu = MENU_TEST;
          displayChanged = true;
          testStep = 0;
          break;
      }
    }
  }
  
  // ===== ПРОВЕРКА RTC =====
  if (now - rtcCheckTimer >= 60000) {
    rtcCheckTimer = now;
    bool rtcNow = checkRTC();
    if (rtcNow != flags.rtcOk) {
      flags.rtcOk = rtcNow;
      displayChanged = true;
      if (!rtcNow && runState.active) stopRunMode();
    }
  }
  
  // ===== КНОПКА СБРОСА =====
  static bool lastResetState = HIGH;
  bool currentResetState = digitalRead(RESET_BUTTON);
  
  if (lastResetState == HIGH && currentResetState == LOW) {
    delay(50);
    if (digitalRead(RESET_BUTTON) == LOW) {
      lcd.clear();
      lcd.print(F("RESET MEMORY"));
      delay(1000);
      
      resetSettings();
      loadSettings();
      
      lcd.clear();
      displayChanged = true;
    }
  }
  lastResetState = currentResetState;
}