#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <U8g2lib.h>
#include <WiFiClientSecure.h>
#include <cstring>

#if __has_include("secrets.h")
#include "secrets.h"
#else
constexpr char WIFI_SSID[] = "";
constexpr char WIFI_PASSWORD[] = "";
#endif

namespace {
constexpr uint8_t buzzerPin = D1; // GPIO5
constexpr uint16_t noteGapMs = 35;
constexpr uint16_t maxNotes = 800;
constexpr uint8_t maxFolders = 48;
constexpr size_t maxFolderNameLength = 24;
constexpr uint8_t maxSongFetchAttempts = 5;
constexpr uint16_t httpTimeoutMs = 5000;
constexpr char repoContentsUrl[] =
    "https://api.github.com/repos/robsoncouto/arduino-songs/contents/";
constexpr char rawSongUrl[] =
    "https://raw.githubusercontent.com/robsoncouto/arduino-songs/master/";

struct Note {
  uint8_t midi;
  uint16_t durationMs;
};

U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(
    U8G2_R0, U8X8_PIN_NONE, D5, D6);

char folders[maxFolders][maxFolderNameLength];
uint8_t folderCount = 0;
Note currentNotes[maxNotes];
Note parsedNotes[maxNotes];
uint16_t currentNoteCount = 0;
uint16_t nextNote = 0;
int8_t currentFolder = -1;
unsigned long nextNoteAt = 0;
bool notePlaying = false;
bool onlineCatalogAvailable = false;
char currentTitle[24] = "Connecting Wi-Fi";

constexpr Note fallbackNotes[] = {
    {69, 180}, {72, 180}, {76, 270}, {74, 180}, {72, 180}, {69, 360},
    {67, 180}, {69, 180}, {72, 270}, {76, 180}, {79, 360}, {76, 180},
    {74, 180}, {72, 270}, {69, 180}, {72, 360}, {67, 180}, {69, 180},
    {72, 180}, {74, 270}, {76, 180}, {74, 180}, {72, 360}, {69, 180},
    {72, 180}, {76, 270}, {79, 180}, {81, 360}, {79, 180}, {76, 180},
    {74, 270}, {72, 540},
};

void drawScreen(const char *noteName) {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 11, "RANDOM ONLINE");
  display.drawHLine(0, 15, 128);
  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(0, 29, currentTitle);
  display.setFont(u8g2_font_logisoso24_tf);
  display.drawStr(0, 62, noteName);
  display.sendBuffer();
}

int parsePitch(const char *token) {
  if (strcmp(token, "REST") == 0) {
    return 0;
  }
  if (strncmp(token, "NOTE_", 5) != 0 || token[5] < 'A' || token[5] > 'G') {
    return -1;
  }

  const char letter = token[5];
  const int semitones[] = {9, 11, 0, 2, 4, 5, 7};
  int semitone = semitones[letter - 'A'];
  int position = 6;
  if (token[position] == 'S') {
    ++semitone;
    ++position;
  }
  const int octave = atoi(token + position);
  const int midi = (octave + 1) * 12 + semitone;
  return midi >= 0 && midi <= 127 ? midi : -1;
}

bool addToken(char *token, bool &expectPitch, int &pendingPitch, int tempo,
              uint16_t &noteCount, Note *notes) {
  if (token[0] == '\0') {
    return true;
  }

  if (expectPitch) {
    pendingPitch = parsePitch(token);
    expectPitch = false;
    return pendingPitch >= 0;
  }

  const int divider = atoi(token);
  if (divider == 0) {
    return false;
  }
  const int wholeNoteMs = 240000 / tempo;
  int durationMs = wholeNoteMs / abs(divider);
  if (divider < 0) {
    durationMs = durationMs * 3 / 2;
  }
  durationMs = constrain(durationMs, 40, 6000);
  if (noteCount >= maxNotes) {
    return false;
  }

  notes[noteCount++] = {
      static_cast<uint8_t>(pendingPitch), static_cast<uint16_t>(durationMs)};
  expectPitch = true;
  return true;
}

bool parseSongLine(char *line, int &tempo, bool &inMelody, bool &expectPitch,
                   int &pendingPitch, uint16_t &noteCount, Note *notes,
                   bool &finished) {
  char *comment = strstr(line, "//");
  if (comment != nullptr) {
    *comment = '\0';
  }

  if (!inMelody) {
    char *tempoText = strstr(line, "tempo");
    if (tempoText != nullptr) {
      char *equals = strchr(tempoText, '=');
      if (equals != nullptr) {
        tempo = atoi(equals + 1);
      }
    }

    char *melodyText = strstr(line, "melody");
    char *openingBrace = melodyText == nullptr ? nullptr : strchr(melodyText, '{');
    if (openingBrace == nullptr) {
      return true;
    }
    inMelody = true;
    line = openingBrace + 1;
  }

  char token[16] = {};
  size_t tokenLength = 0;
  for (char *cursor = line; *cursor != '\0'; ++cursor) {
    if (*cursor == '}') {
      token[tokenLength] = '\0';
      if (!addToken(token, expectPitch, pendingPitch, tempo, noteCount, notes)) {
        return false;
      }
      finished = true;
      return true;
    }

    if (*cursor == ',' || isspace(static_cast<unsigned char>(*cursor))) {
      token[tokenLength] = '\0';
      if (!addToken(token, expectPitch, pendingPitch, tempo, noteCount, notes)) {
        return false;
      }
      tokenLength = 0;
    } else if (tokenLength + 1 < sizeof(token)) {
      token[tokenLength++] = *cursor;
    } else {
      return false;
    }
  }

  token[tokenLength] = '\0';
  return addToken(token, expectPitch, pendingPitch, tempo, noteCount, notes);
}

bool discoverFolders() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, repoContentsUrl)) {
    return false;
  }
  http.addHeader("User-Agent", "ESP8266-PassiveBuzer");

  const int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  StaticJsonDocument<128> filter;
  filter[0]["type"] = true;
  filter[0]["name"] = true;
  DynamicJsonDocument document(8192);
  const DeserializationError error = deserializeJson(
      document, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    return false;
  }

  JsonArray entries = document.as<JsonArray>();
  for (JsonObject entry : entries) {
    if (folderCount >= maxFolders || entry["type"] != "dir") {
      continue;
    }

    const char *name = entry["name"] | "";
    const size_t nameLength = strlen(name);
    if (nameLength == 0 || nameLength >= maxFolderNameLength) {
      continue;
    }

    strncpy(folders[folderCount], name, maxFolderNameLength - 1);
    folders[folderCount][maxFolderNameLength - 1] = '\0';
    ++folderCount;
  }
  return folderCount > 0;
}

bool fetchSong(uint8_t folderIndex) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(httpTimeoutMs);
  const char *folder = folders[folderIndex];
  String fileName = folder;
  if (strcmp(folder, "thebadinerie") == 0) {
    fileName = "badinerie";
  }

  String url = rawSongUrl;
  url += folder;
  url += "/";
  url += fileName;
  url += ".ino";

  if (!http.begin(client, url)) {
    return false;
  }
  const int responseCode = http.GET();
  if (responseCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  uint16_t parsedNoteCount = 0;
  int tempo = 0;
  bool inMelody = false;
  bool expectPitch = true;
  int pendingPitch = 0;
  bool finished = false;
  bool valid = true;
  char line[512];

  WiFiClient *stream = http.getStreamPtr();
  stream->setTimeout(httpTimeoutMs);
  while (!finished && (http.connected() || stream->available())) {
    String sourceLine = stream->readStringUntil('\n');
    if (sourceLine.length() >= sizeof(line)) {
      valid = false;
      break;
    }
    sourceLine.toCharArray(line, sizeof(line));
    if (!parseSongLine(line, tempo, inMelody, expectPitch, pendingPitch,
                       parsedNoteCount, parsedNotes, finished)) {
      valid = false;
      break;
    }
  }
  http.end();

  if (!valid || !finished || tempo <= 0 || parsedNoteCount == 0 ||
      !expectPitch) {
    return false;
  }

  memcpy(currentNotes, parsedNotes, parsedNoteCount * sizeof(Note));
  currentNoteCount = parsedNoteCount;
  nextNote = 0;
  currentFolder = folderIndex;
  strncpy(currentTitle, folder, sizeof(currentTitle) - 1);
  currentTitle[sizeof(currentTitle) - 1] = '\0';
  return true;
}

bool fetchRandomSong() {
  if (WiFi.status() != WL_CONNECTED || folderCount == 0) {
    return false;
  }

  const uint8_t attempts =
      folderCount < maxSongFetchAttempts ? folderCount : maxSongFetchAttempts;
  for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
    uint8_t selected = random(folderCount);
    if (folderCount > 1 && selected == currentFolder) {
      selected = (selected + 1 + random(folderCount - 1)) % folderCount;
    }
    if (fetchSong(selected)) {
      return true;
    }
  }
  return false;
}

void useFallbackSong() {
  memcpy(currentNotes, fallbackNotes, sizeof(fallbackNotes));
  currentNoteCount = sizeof(fallbackNotes) / sizeof(fallbackNotes[0]);
  nextNote = 0;
  currentFolder = -1;
  strncpy(currentTitle, "Neon Dreams", sizeof(currentTitle) - 1);
  currentTitle[sizeof(currentTitle) - 1] = '\0';
}

void loadNextSong() {
  drawScreen("Loading");
  if (onlineCatalogAvailable && fetchRandomSong()) {
    drawScreen("Ready");
    return;
  }

  useFallbackSong();
  drawScreen("Fallback");
}
} // namespace

void setup() {
  display.begin();
  drawScreen("Wi-Fi");

  pinMode(buzzerPin, OUTPUT);
  analogWriteRange(1023);
  analogWrite(buzzerPin, 0);
  display.drawStr(0, 44, "Buzzer");
  display.sendBuffer();

  if (WIFI_SSID[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    const unsigned long startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < 12000) {
      delay(100);
    }
  }

  randomSeed(ESP.getCycleCount());
  onlineCatalogAvailable =
      WiFi.status() == WL_CONNECTED && discoverFolders();
  loadNextSong();
  nextNoteAt = millis();
}

void loop() {
  const unsigned long now = millis();
  if (static_cast<int32_t>(now - nextNoteAt) < 0) {
    return;
  }

  if (notePlaying) {
    analogWrite(buzzerPin, 0);
    notePlaying = false;
    drawScreen("...");
    nextNoteAt = now + noteGapMs;
    return;
  }

  if (nextNote >= currentNoteCount) {
    loadNextSong();
  }

  const Note note = currentNotes[nextNote++];
  if (note.midi == 0) {
    analogWrite(buzzerPin, 0);
    drawScreen("REST");
    nextNoteAt = now + note.durationMs;
    return;
  }

  const uint8_t semitone = note.midi % 12;
  const uint8_t octave = note.midi / 12 - 1;
  constexpr const char *noteNames[] = {
      "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  char noteName[5];
  snprintf(noteName, sizeof(noteName), "%s%d", noteNames[semitone], octave);

  const uint16_t frequency = static_cast<uint16_t>(
      440.0f * powf(2.0f, (static_cast<int>(note.midi) - 69) / 12.0f) + 0.5f);
  analogWriteFreq(frequency);
  analogWrite(buzzerPin, 512);
  notePlaying = true;
  drawScreen(noteName);
  nextNoteAt = now + note.durationMs - noteGapMs;
}
