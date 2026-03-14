/*
  Управление RGBW лентой 24В через Arduino Nano
  с автоматической сменой цвета по расписанию
  Потапов С.А. poet1988@list.ru
  Версия 1.8.4 
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverDS3231Min.h>
#include <EncButton.h>
#include <EEPROM.h>
#include <GyverPWM.h>  

// =============== КОНСТАНТЫ И НАСТРОЙКИ ===============

// Версия прошивки
const char FIRMWARE_VERSION[] = "1.8.4";

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

// =============== НАСТРОЙКА ЧАСТОТЫ ШИМ ===============
 
#define PWM_PRESCALER 1  // 1 - 31.4 кГц, 2 - 4 кГц 
#define PWM_MODE 1       // 1 - Phase-correct PWM, 0 - Fast PWM

// Настройки EEPROM
#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0xAA56
#define EEPROM_SAVE_DELAY 5000

// Максимальное количество шагов
#define MAX_STEPS 6

// Тип энкодера
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
  uint8_t brightness[4];  // 0-255 для всех каналов
  uint16_t durations[MAX_STEPS];
  bool repeat;
  uint8_t backlightTimeout;
};

struct RunState {
  uint8_t currentStep;
  bool active;
  
  RunState() : currentStep(0), active(false) {}
} runState;

struct Timer {
  unsigned long lastTick;
  unsigned long interval;
  
  Timer() : lastTick(0), interval(0) {}
  
  void setInterval(unsigned long ms) {
    interval = ms;
  }
  
  void reset() {
    lastTick = millis();
  }
  
bool check() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastTick >= interval) {
    lastTick = currentMillis;
    return true;
  }
  return false;
}
};

struct Flags {
  uint8_t displayOn : 1;
  uint8_t rtcOk : 1;
  uint8_t settingsChanged : 1;
  uint8_t showChain : 1;
  uint8_t inSubMenu : 1;
  uint8_t editingValue : 1;
  uint8_t chainDisplayRequest : 1;
  uint8_t rtcMessageShown : 1;
} flags;

struct StepTime {
  uint32_t startUnix;
  uint16_t duration;
  
  StepTime() : startUnix(0), duration(0) {}
} stepTime;
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

// Системные таймеры
Timer displayTimer;
Timer saveTimer;
Timer rtcCheckTimer;
Timer runUpdateTimer;
Timer runDisplayTimer;

// Таймер для показа цепочки 
unsigned long chainShowTime = 0;

// Буферы
char displayBuffer[17];
char lineBuffer[17];

// Флаги обновления
uint8_t lastMenuCursor = 255;
uint8_t lastSubMenuCursor = 255;
uint8_t lastHour = 255;
uint8_t lastMinute = 255;
uint16_t lastRemainingTime = 65535;
bool displayChanged = true;
bool partialUpdate = false;

// Для отслеживания активности энкодера
unsigned long lastEncoderActivity = 0;
bool encoderActive = false;

// =============== НОВАЯ ПЕРЕМЕННАЯ ДЛЯ ПОДСВЕТКИ ===============
unsigned long lastUserActivity = 0;  // Время последнего действия пользователя

// =============== ПРОТОТИПЫ ФУНКЦИЙ ===============

void initSystem();
void initDisplay();
void initPWM();
void loadSettings();
void saveSettings();
void resetSettings();
bool checkRTC();

void setColor(Color color, bool forceBrightness = false);
void setChannel(uint8_t pin, uint8_t brightness);
void forceAllPinsOff();
void runTestSequence();

void handleEncoder();
void encoderTick();
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
const char* colorToString(Color color);
void formatTime(uint16_t minutes, char* buffer);
uint32_t getCurrentMinutes();
uint16_t calculateRemainingTime();

void runScheduler();
void advanceChain();
void startRunMode();
void stopRunMode();
void handleBacklight();

// =============== НОВЫЕ ФУНКЦИИ ОБРАБОТКИ ЭНКОДЕРА ===============

void handleMainMenuTurn(int8_t change) {
  menuCursor = (menuCursor + change + 8) % 8;
}

void handleSubMenuTurn(int8_t change) {
  if (currentMenu == MENU_CHAIN || currentMenu == MENU_TIME) {
    subMenuCursor = (subMenuCursor + change + MAX_STEPS) % MAX_STEPS;
  } else if (currentMenu == MENU_BRIGHTNESS) {
    subMenuCursor = (subMenuCursor + change + 4) % 4;
  }
}

void handleEditTurn(int8_t change) {
  if (currentMenu == MENU_CLOCK || currentMenu == MENU_TIME) {
    if (editPart == 0) {
      if (currentMenu == MENU_CLOCK) {
        editHours = (editHours + change + 24) % 24;
      } else {
        editHours = (editHours + change + 100) % 100;
      }
    } else {
      editMinutes = (editMinutes + change + 60) % 60;
    }
  } else if (currentMenu == MENU_BRIGHTNESS) {
    editValue = constrain(editValue + change, 0, 255);
    
    if (flags.editingValue) {
      // Обновляем яркость выбранного канала в реальном времени
      switch (subMenuCursor) {
        case 0:
          setChannel(PIN_RED, editValue);
          break;
        case 1:
          setChannel(PIN_GREEN, editValue);
          break;
        case 2:
          setChannel(PIN_BLUE, editValue);
          break;
        case 3:
          setChannel(PIN_WHITE, editValue);
          break;
      }
    }
  }
}

// =============== РЕАЛИЗАЦИЯ ФУНКЦИЙ ===============

void setup() {
  Serial.begin(115200);
  Serial.print(F("RGBW Controller v"));
  Serial.println(FIRMWARE_VERSION);
  
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_WHITE, OUTPUT);
  
  
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_BLUE, LOW);
  digitalWrite(PIN_WHITE, LOW);
  
  pinMode(RESET_BUTTON, INPUT_PULLUP);
  
  initPWM();  
  
  Wire.begin();
  initDisplay();
  
  rtc.begin();
  
  
  if (rtc.getUnix() == 0) {
    Serial.println(F("Setting compile time"));
    
    const char *compileDate = __DATE__;
    const char *compileTime = __TIME__;
    
    
    int year = 2024, month = 1, day = 1;
    int hour = 12, minute = 0, second = 0;
    
    
    char monthStr[4];
    memcpy(monthStr, compileDate, 3);
    monthStr[3] = '\0';
    
    if (strcmp(monthStr, "Jan") == 0) month = 1;
    else if (strcmp(monthStr, "Feb") == 0) month = 2;
    else if (strcmp(monthStr, "Mar") == 0) month = 3;
    else if (strcmp(monthStr, "Apr") == 0) month = 4;
    else if (strcmp(monthStr, "May") == 0) month = 5;
    else if (strcmp(monthStr, "Jun") == 0) month = 6;
    else if (strcmp(monthStr, "Jul") == 0) month = 7;
    else if (strcmp(monthStr, "Aug") == 0) month = 8;
    else if (strcmp(monthStr, "Sep") == 0) month = 9;
    else if (strcmp(monthStr, "Oct") == 0) month = 10;
    else if (strcmp(monthStr, "Nov") == 0) month = 11;
    else if (strcmp(monthStr, "Dec") == 0) month = 12;
    
    
    day = atoi(compileDate + 4);
    
    
    year = atoi(compileDate + 9);
    
    
    hour = atoi(compileTime);
    minute = atoi(compileTime + 3);
    second = atoi(compileTime + 6);
    
    Datime dt(year, month, day, hour, minute, second);
    rtc.setTime(dt);
    Serial.println(F("Compile time set"));
  }
  
  // Проверка RTC 
  flags.rtcOk = false;
  for (int i = 0; i < 5; i++) {
    if (checkRTC()) {
      flags.rtcOk = true;
      break;
    }
    delay(100);
  }
  
  enc.setEncType(ENCODER_TYPE);
  
  loadSettings();
  
  runState.active = false;
  runState.currentStep = 0;
  
  displayTimer.setInterval(200);
  saveTimer.setInterval(EEPROM_SAVE_DELAY);
  rtcCheckTimer.setInterval(60000);
  runUpdateTimer.setInterval(1000);
  runDisplayTimer.setInterval(500);
  
  flags.displayOn = true;
  flags.chainDisplayRequest = false;
  flags.rtcMessageShown = false;
  flags.showChain = false;
  lastUserActivity = millis();  
  
  // Показываем сообщение о состоянии RTC после заставки
  lcd.clear();
  if (flags.rtcOk) {
    lcd.print(F("RTC OK"));
  } else {
    lcd.print(F("RTC ERROR!"));
    lcd.setCursor(0, 1);
    lcd.print(F("Check module"));
  }
  delay(2000);
  lcd.clear();
  flags.rtcMessageShown = true;
  
  Serial.println(F("System initialized"));
  Serial.println(F("MOSFET mode: HIGH (active HIGH)"));
}

void loop() {
  enc.tick();
  
  if (enc.turn() || enc.click() || enc.hold()) {
    lastEncoderActivity = millis();
    encoderActive = true;

   
  static unsigned long lastUnixCheck = 0;
  if (millis() - lastUnixCheck > 60000 && flags.rtcOk) {
    lastUnixCheck = millis();
    Serial.print(F("Unix time: "));
    Serial.println(rtc.getUnix());
    
    Datime dt = rtc.getTime();
    Serial.print(F("RTC time: "));
    Serial.print(dt.hour);
    Serial.print(F(":"));
    if (dt.minute < 10) Serial.print('0');
    Serial.print(dt.minute);
    Serial.print(F(":"));
    if (dt.second < 10) Serial.print('0');
    Serial.println(dt.second);
  }   
    // ========== ЛОГИКА ПОДСВЕТКИ ==========
    // Немедленно включаем подсветку при любом действии с энкодером
    if (!flags.displayOn) {
      flags.displayOn = true;
      lcd.backlight();
      displayChanged = true;
    }
    
    lastUserActivity = millis();
    
    
    handleEncoder();
  } else {
    encoderActive = false;
  }
  
  // Автоматическое скрытие показа цепочки через 5 секунд 
  if (flags.showChain && (millis() - chainShowTime > 5000)) {
    flags.showChain = false;
    flags.chainDisplayRequest = false;
    displayChanged = true;
  }
  
  if (displayTimer.check()) {
    if (displayChanged || partialUpdate || flags.chainDisplayRequest) {
      updateDisplay();
      displayChanged = false;
      partialUpdate = false;
      flags.chainDisplayRequest = false;
    }
  }
  
  if (saveTimer.check() && flags.settingsChanged) {
    saveSettings();
  }
  
  // ========== УПРАВЛЕНИЕ ВЫКЛЮЧЕНИЕМ ПОДСВЕТКИ ПО ТАЙМАУТУ ==========
  static unsigned long lastBacklightCheck = 0;
  if (millis() - lastBacklightCheck > 1000) {  // Проверяем раз в секунду
    lastBacklightCheck = millis();
    
    if (flags.displayOn && settings.backlightTimeout > 0 && settings.backlightTimeout < 3) {
      unsigned long timeout = 0;
      if (settings.backlightTimeout == 1) timeout = 900000;      // 15 минут
      else if (settings.backlightTimeout == 2) timeout = 3600000; // 1 час
      
      if (timeout > 0 && (millis() - lastUserActivity > timeout)) {
        flags.displayOn = false;
        lcd.noBacklight();
      }
    }
  }
  // =================================================================
  
  if (currentMenu == MENU_RUN && flags.rtcOk) {
    if (runUpdateTimer.check()) {
      runScheduler();
    }
    if (!encoderActive && runDisplayTimer.check()) {
      partialUpdate = true;
    }
  }
  
  if (rtcCheckTimer.check()) {
    bool rtcNow = checkRTC();
    if (rtcNow != flags.rtcOk) {
      flags.rtcOk = rtcNow;
      displayChanged = true;
      if (!rtcNow && runState.active) stopRunMode();
    }
  }
  
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
      
      Serial.println(F("Settings reset"));
    }
  }
  lastResetState = currentResetState;
  
  delay(10);
}

// =============== ИНИЦИАЛИЗАЦИЯ ШИМ ===============

void initPWM() {
  // Устанавливаем Timer1 (пины 9 и 10) в 8-битный режим
  PWM_TMR1_8BIT();
  
  // Настраиваем частоту и режим для всех ШИМ пинов
  // Для пинов 3 и 11 (Timer2)
  PWM_prescaler(3, PWM_PRESCALER);   // Устанавливаем предделитель
  PWM_prescaler(11, PWM_PRESCALER);  // для пина 11 то же самое, т.к. оба на Timer2
  PWM_mode(3, PWM_MODE);             // Устанавливаем режим (Phase-correct или Fast PWM)
  PWM_mode(11, PWM_MODE);            // для пина 11
  
  // Для пинов 9 и 10 (Timer1)
  PWM_prescaler(9, PWM_PRESCALER);   // Устанавливаем предделитель
  PWM_prescaler(10, PWM_PRESCALER);  // для пина 10 то же самое, т.к. оба на Timer1
  PWM_mode(9, PWM_MODE);             // Устанавливаем режим (Phase-correct или Fast PWM)
  PWM_mode(10, PWM_MODE);            // для пина 10
 
  // Выводим информацию о частотах
  Serial.println(F("PWM initialized"));
  Serial.print(F("Frequency: "));
  switch(PWM_PRESCALER) {
    case 1: Serial.println(F("31.4 kHz")); break;
    case 2: Serial.println(F("4 kHz")); break;
    default: Serial.println(F("custom"));
  }
  Serial.println(F("Mode: Phase-correct PWM"));
}

// =============== ИНИЦИАЛИЗАЦИЯ ДИСПЛЕЯ ===============

void initDisplay() {
  Wire.beginTransmission(LCD_ADDR);
  byte error = Wire.endTransmission();
  
  if (error != 0) {
    Serial.print(F("LCD not found at address 0x"));
    Serial.println(LCD_ADDR, HEX);
    
    byte addresses[] = {0x27, 0x3F, 0x20, 0x26};
    bool found = false;
    
    for (byte i = 0; i < 4; i++) {
      Wire.beginTransmission(addresses[i]);
      if (Wire.endTransmission() == 0) {
        Serial.print(F("LCD found at address 0x"));
        Serial.println(addresses[i], HEX);
        Serial.println(F("Change LCD_ADDR in sketch!"));
        found = true;
        break;
      }
    }
    
    if (!found) {
      Serial.println(F("No LCD found!"));
    }
    delay(2000);
  }
  
  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  // Заставка с версией
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
  
  // После загрузки применяем настройки яркости к текущему состоянию
  if (runState.active) {
    setColor(settings.chain[runState.currentStep]);
  }
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
  } else {
    Serial.println(F("EEPROM write failed!"));
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
  Serial.println(F("Default settings saved"));
}
// Проверка RTC модуля
bool checkRTC() {
  Wire.beginTransmission(0x68);
  Wire.write(0x00);  
  if (Wire.endTransmission() != 0) return false;
  
  
  Wire.requestFrom((uint8_t)0x68, (uint8_t)2);
  if (Wire.available() < 2) return false;
  
  uint8_t sec = Wire.read();
  uint8_t min = Wire.read();
  
  
  if ((sec & 0x7F) > 0x59) return false;  
  if (min > 0x59) return false;            
  
  return true;
}

// =============== УПРАВЛЕНИЕ СВЕТОМ ===============

void setColor(Color color, bool forceBrightness = false) {
  // Принудительно выключаем ВСЕ пины через digitalWrite LOW
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_BLUE, LOW);
  digitalWrite(PIN_WHITE, LOW);
  
  // Дополнительно устанавливаем ШИМ в 0 для надежности
  PWM_set(PIN_RED, 0);
  PWM_set(PIN_GREEN, 0);
  PWM_set(PIN_BLUE, 0);
  PWM_set(PIN_WHITE, 0);
  
  // Если это не DARK - включаем нужный канал
  if (color != COLOR_DARK) {
    switch (color) {
      case COLOR_RED:
        setChannel(PIN_RED, forceBrightness ? 255 : settings.brightness[0]);
        break;
      case COLOR_GREEN:
        setChannel(PIN_GREEN, forceBrightness ? 255 : settings.brightness[1]);
        break;
      case COLOR_BLUE:
        setChannel(PIN_BLUE, forceBrightness ? 255 : settings.brightness[2]);
        break;
      case COLOR_WHITE:
        setChannel(PIN_WHITE, forceBrightness ? 255 : settings.brightness[3]);
        break;
      default:
        break;
    }
  }
}

void setChannel(uint8_t pin, uint8_t brightness) {
  if (brightness == 0) {
    
    digitalWrite(pin, LOW);
    PWM_set(pin, 0);
  } else {
    
    PWM_set(pin, brightness);
  }
}

void forceAllPinsOff() {
  
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_BLUE, LOW);
  digitalWrite(PIN_WHITE, LOW);
  
  PWM_set(PIN_RED, 0);
  PWM_set(PIN_GREEN, 0);
  PWM_set(PIN_BLUE, 0);
  PWM_set(PIN_WHITE, 0);
  
  
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_WHITE, OUTPUT);
  
  Serial.println(F("All pins forced OFF"));
}

void runTestSequence() {
  lcd.clear();
  lcd.print(F("Test sequence"));
  delay(500);
  
  // Тест красного
  forceAllPinsOff();
  setColor(COLOR_RED, true);
  delay(1000);
  
  // Тест зеленого
  forceAllPinsOff();
  setColor(COLOR_GREEN, true);
  delay(1000);
  
  // Тест синего
  forceAllPinsOff();
  setColor(COLOR_BLUE, true);
  delay(1000);
  
  // Тест белого
  forceAllPinsOff();
  setColor(COLOR_WHITE, true);
  delay(1000);
  
  // Тест DARK (выключено)
  forceAllPinsOff();
  setColor(COLOR_DARK);
  
  lcd.clear();
  lcd.print(F("Test complete"));
  delay(1000);
  displayChanged = true;
}

// =============== ОБРАБОТКА ЭНКОДЕРА ===============

void encoderTick() {
  
}

void handleEncoder() {
  lastEncoderActivity = millis();
  
  if (enc.turn()) {
    int8_t change = -enc.dir();
    
    if (flags.editingValue) {
      handleEditTurn(change);
      partialUpdate = true;
    } else if (flags.inSubMenu) {
      handleSubMenuTurn(change);
      displayChanged = true;
    } else {
      handleMainMenuTurn(change);
      displayChanged = true;
    }
  }
  
  if (enc.click()) {
    if (currentMenu == MENU_RUN) {
      flags.showChain = true;
      flags.chainDisplayRequest = true;
      chainShowTime = millis();  
      displayChanged = true;
    } else if (!flags.inSubMenu && currentMenu == MENU_MAIN) {
      enterSubMenu();
      displayChanged = true;
    } else if (flags.editingValue) {
      if (currentMenu == MENU_CLOCK || currentMenu == MENU_TIME) {
        if (editPart == 0) {
          editPart = 1;
          partialUpdate = true;
        } else {
          exitEditing();
        }
      } else {
        exitEditing();
      }
    } else {
      if (currentMenu == MENU_TEST) {
        runTestSequence();
      } else {
        enterEditing();
      }
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

void enterSubMenu() {
  flags.inSubMenu = true;
  subMenuCursor = 0;
  flags.editingValue = false;
  editPart = 0;
  
  switch (menuCursor) {
    case 0: currentMenu = MENU_CHAIN; break;
    case 1: currentMenu = MENU_BRIGHTNESS; break;
    case 2: currentMenu = MENU_TIME; break;
    case 3: currentMenu = MENU_REPEAT; break;
    case 4: currentMenu = MENU_RUN; break;
    case 5: currentMenu = MENU_CLOCK; break;
    case 6: currentMenu = MENU_TEST; break;
    case 7: currentMenu = MENU_DISPLAY; break;
  }
  
  if (currentMenu == MENU_RUN && !runState.active && flags.rtcOk) {
    startRunMode();
  }
}

void enterEditing() {
  flags.editingValue = true;
  
  if (currentMenu == MENU_BRIGHTNESS) {
    editValue = settings.brightness[subMenuCursor];
    // Выключаем все каналы
    setColor(COLOR_DARK);
    
    // Включаем выбранный канал с текущей яркостью
    switch (subMenuCursor) {
      case 0:
        setChannel(PIN_RED, editValue);
        break;
      case 1:
        setChannel(PIN_GREEN, editValue);
        break;
      case 2:
        setChannel(PIN_BLUE, editValue);
        break;
      case 3:
        setChannel(PIN_WHITE, editValue);
        break;
    }
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
  }
  
  displayChanged = true;
}

void exitEditing() {
  flags.editingValue = false;
  
  if (currentMenu == MENU_CLOCK && flags.rtcOk) {
    
    Datime dt = rtc.getTime();
    
    
    dt.hour = editHours;
    dt.minute = editMinutes;
    dt.second = 0;  
    
    
    rtc.setTime(dt);
    
    
    rtc.setUnix(rtc.getUnix());
    
    flags.settingsChanged = true;
    
    // Отладка
    Serial.print(F("Time set to: "));
    Serial.print(editHours);
    Serial.print(F(":"));
    Serial.print(editMinutes);
    Serial.println(F(":00"));
  } else if (currentMenu == MENU_TIME) {
    if (settings.chain[subMenuCursor] == COLOR_SKIP) {
      settings.durations[subMenuCursor] = 0;
    } else {
      settings.durations[subMenuCursor] = editHours * 60 + editMinutes;
    }
    flags.settingsChanged = true;
  } else if (currentMenu == MENU_BRIGHTNESS) {
    settings.brightness[subMenuCursor] = editValue;
    flags.settingsChanged = true;
    setColor(COLOR_DARK);
  } else if (currentMenu == MENU_CHAIN) {
    settings.chain[subMenuCursor] = (Color)((settings.chain[subMenuCursor] + 1) % COLOR_COUNT);
    flags.settingsChanged = true;
  } else if (currentMenu == MENU_REPEAT) {
    settings.repeat = !settings.repeat;
    flags.settingsChanged = true;
  } else if (currentMenu == MENU_DISPLAY) {
    settings.backlightTimeout = (settings.backlightTimeout + 1) % 4;
    flags.displayOn = true;
    lcd.backlight();
    flags.settingsChanged = true;
  }
  
  editPart = 0;
  displayChanged = true;
}

// =============== ОБНОВЛЕНИЕ ДИСПЛЕЯ ===============

void updateDisplay() {
  
  if (currentMenu == MENU_RUN && flags.showChain) {
    drawChainOverview();
    return;  
  }
  
  if (displayChanged) {
    lcd.clear();
    drawFullScreen();
  } else if (partialUpdate) {
    drawPartialUpdate();
  }
}

void drawFullScreen() {
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

void drawPartialUpdate() {
  switch (currentMenu) {
    case MENU_MAIN:
      drawMainMenuCursor();
      break;
    case MENU_RUN:
      drawPartialRunScreen();
      break;
    default:
      drawFullScreen();
      break;
  }
}

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

void drawMainMenuCursor() {
  lcd.setCursor(0, 0);
  lcd.print(' ');
  lcd.setCursor(0, 1);
  lcd.print(' ');
  
  if (menuCursor % 2 == 0) {
    lcd.setCursor(0, 0);
  } else {
    lcd.setCursor(0, 1);
  }
  lcd.print('>');
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
    lcd.print(F("RTC ERROR      "));
    lcd.setCursor(0, 1);
    lcd.print(F("Check module   "));
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
  
  // Очищаем всю строку пробелами
  lcd.print("                "); // 16 пробелов
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
  lcd.setCursor(0, 0);
  lcd.print(F("TEST MODE"));
  lcd.setCursor(0, 1);
  lcd.print(F("Click to test"));
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

const char* colorToString(Color color) {
  switch (color) {
    case COLOR_RED: return "RED";
    case COLOR_GREEN: return "GREEN";
    case COLOR_BLUE: return "BLUE";
    case COLOR_WHITE: return "WHITE";
    case COLOR_DARK: return "DARK";
    case COLOR_SKIP: return "SKIP";
    default: return "UNKNOWN";
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
  uint32_t elapsedSeconds = currentUnix - stepTime.startUnix;
  uint32_t durationSeconds = (uint32_t)stepTime.duration * 60UL;
  
  if (elapsedSeconds >= durationSeconds) {
    return 0;
  }
  
  uint32_t remainingSeconds = durationSeconds - elapsedSeconds;
  return (remainingSeconds + 59) / 60; 
}
void initStepTime() {
  if (flags.rtcOk) {
    stepTime.startUnix = rtc.getUnix();
    stepTime.duration = settings.durations[runState.currentStep];
    
    Serial.print(F("Step started at Unix: "));
    Serial.println(stepTime.startUnix);
    Serial.print(F("Duration: "));
    Serial.println(stepTime.duration);
  }
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
  
  while (settings.chain[runState.currentStep] == COLOR_SKIP && 
         runState.currentStep < MAX_STEPS - 1) {
    runState.currentStep++;
  }
  
  initStepTime();
  setColor(settings.chain[runState.currentStep]);
  
  lastRemainingTime = 65535;
  lastHour = 255;
  lastMinute = 255;
  
  Serial.print(F("RUN started at step "));
  Serial.println(runState.currentStep);
}

void stopRunMode() {
  runState.active = false;
  runState.currentStep = 0;
  forceAllPinsOff();  
  setColor(COLOR_DARK); 
  
  Serial.println(F("RUN stopped"));
}

// =============== ОСНОВНАЯ ЛОГИКА ===============

void runScheduler() {
  if (!flags.rtcOk) {
    if (runState.active) stopRunMode();
    return;
  }
  
  if (!runState.active) return;
  
  if (settings.chain[runState.currentStep] == COLOR_SKIP) {
    Serial.println(F("Skip step detected, advancing"));
    advanceChain();
    return;
  }
  
  
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  
  if (now - lastCheck >= 1000) { 
    lastCheck = now;
    
    uint32_t currentUnix = rtc.getUnix();
    uint32_t elapsedSeconds = currentUnix - stepTime.startUnix;
    uint32_t durationSeconds = (uint32_t)stepTime.duration * 60UL;
    
    if (elapsedSeconds >= durationSeconds) {
      Serial.println(F("Step finished!"));
      advanceChain();
    }
  }
}

void advanceChain() {
  if (!flags.rtcOk) return;
  
  uint8_t startStep = runState.currentStep;
  
  do {
    runState.currentStep = (runState.currentStep + 1) % MAX_STEPS;
    
    if (runState.currentStep == 0 && !settings.repeat) {
      stopRunMode();
      Serial.println(F("Chain finished"));
      return;
    }
    
  } while (settings.chain[runState.currentStep] == COLOR_SKIP && 
           runState.currentStep != startStep);
  
  initStepTime();
  setColor(settings.chain[runState.currentStep]);
  
  lastRemainingTime = 65535;
  
  Serial.print(F("Advanced to step "));
  Serial.print(runState.currentStep);
  Serial.print(F(", color: "));
  Serial.print(colorToChar(settings.chain[runState.currentStep]));
  Serial.print(F(", duration: "));
  Serial.println(settings.durations[runState.currentStep]);
}