/* ========================================================
 * SISTEM MONITORING & OTOMASI AKUARIUM IOT (GABUNGAN FINAL)
 * FITUR: Sensor Asli + Cooldown/Warmup + LCD UX + Dual Path Firebase
 *        + Log Riwayat Harian + Rekap Statistik Harian
 * INTERVAL: Realtime 5 dtk | Log & Harian 30 dtk
 * ========================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_ADS1X15.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include "time.h"

// ==========================================
// KREDENSIAL WIFI & FIREBASE
// ==========================================
#define WIFI_SSID     "RAFISQY"
#define WIFI_PASSWORD "REMBOREB"
#define API_KEY       "AIzaSyDps5vc8pREiwr3BrNbX1k7xN1zleMZR2A"
#define DATABASE_URL  "realtime-database-5c956-default-rtdb.asia-southeast1.firebasedatabase.app"

// ==========================================
// KONFIGURASI PIN HARDWARE
// ==========================================
#define LCD_SDA 21
#define LCD_SCL 22
#define PIN_RELAY_KURAS 26
#define PIN_RELAY_ISI   27
#define pinTrig 32
#define pinEcho 33

// Relay Active-HIGH
#define RELAY_NYALA HIGH
#define RELAY_MATI  LOW

// ==========================================
// OBJEK GLOBAL (JALUR GANDA FIREBASE)
// ==========================================
Adafruit_ADS1115    ads;
LiquidCrystal_I2C   lcd(0x27, 16, 2);
FirebaseData        fbdoRead;
FirebaseData        fbdoWrite;
FirebaseAuth        auth;
FirebaseConfig      config;

// ==========================================
// VARIABEL SENSOR pH
// ==========================================
float V_pH686    = 1.233;
float V_pH401    = 0.470;
const int totalSamples = 30;
float ph_buffer[20];
int   ph_index = 0;

// ==========================================
// VARIABEL SENSOR TURBIDITY
// ==========================================
const int jumlahSampel = 31;
float voltJernih = 1.370;
float faktorNTU  = 1500;

// ==========================================
// VARIABEL SENSOR ULTRASONIK
// ==========================================
const float tinggiMaksimal = 60.0;

// ==========================================
// VARIABEL LOGIKA POMPA
// ==========================================
const float BATAS_PENUH       = 55.0;
const float TARGET_KURAS_TURB = BATAS_PENUH - 30.0;
const float TARGET_KURAS_PH   = BATAS_PENUH - 15.0;

// State Machine
enum StatusSistem { NORMAL, KURAS_KOTOR, KURAS_KIMIA, PENGISIAN, MANUAL_KURAS, MANUAL_ISI };
StatusSistem statusSekarang = NORMAL;
String modeSistem = "AUTO";

// ==========================================
// TIMER FIREBASE
// ==========================================
unsigned long timerKirim  = 5000;
unsigned long timerBaca   = 0;
unsigned long timerLog    = 0;
unsigned long timerHarian = 0;

// ==========================================
// VARIABEL REKAP HARIAN
// ==========================================
int   lastDay          = -1;
float phDailySum       = 0;
float turbDailySum     = 0;
float tinggiDailySum   = 0;
int   dailySampleCount = 0;
float phMax            = -1.0;
float phMin            = 99.0;
float turbMax          = -1.0;

// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- [SISTEM AKUARIUM IOT - GABUNGAN FINAL] ---");

  // 1. PIN RELAY & ULTRASONIK
  pinMode(pinTrig,         OUTPUT);
  pinMode(pinEcho,         INPUT);
  pinMode(PIN_RELAY_KURAS, OUTPUT);
  pinMode(PIN_RELAY_ISI,   OUTPUT);
  digitalWrite(PIN_RELAY_KURAS, RELAY_MATI);
  digitalWrite(PIN_RELAY_ISI,   RELAY_MATI);

  // 2. LCD
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();

  // 3. ADS1115
  if (!ads.begin()) {
    Serial.println("[ERROR] ADS1115 tidak terdeteksi!");
    lcd.setCursor(0, 0); lcd.print("Error: ADS1115  ");
    while (1);
  }

  for (int i = 0; i < 20; i++) ph_buffer[i] = 7.0;

  // 4. WIFI & FIREBASE
  lcd.setCursor(0, 0); lcd.print("Sistem Akuarium ");
  lcd.setCursor(0, 1); lcd.print("Koneksi WiFi... ");
  Serial.print("[WIFI] Menghubungkan ke "); Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int timeoutCounter = 0;
  while (WiFi.status() != WL_CONNECTED && timeoutCounter < 40) {
    delay(500); Serial.print("."); timeoutCounter++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Sukses Terhubung!");
    lcd.setCursor(0, 1); lcd.print("WiFi Connected! ");

    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");

    config.api_key          = API_KEY;
    config.database_url     = DATABASE_URL;
    config.signer.test_mode = true;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    fbdoRead.setResponseSize(1024);
    fbdoWrite.setResponseSize(1024);
    Serial.println("[FIREBASE] Jalur Ganda Siap.");
  } else {
    Serial.println("\n[WIFI GAGAL] Mode Offline.");
    lcd.setCursor(0, 1); lcd.print("WiFi Gagal!     ");
  }

  delay(2500);
  lcd.clear();
}

// ==========================================
void loop() {

  // ====================================================
  // 1. SENSOR pH (Pin A2 ADS1115)
  // ====================================================
  ads.setGain(GAIN_TWOTHIRDS);
  delay(2);

  int raw_samples[totalSamples];
  for (int i = 0; i < totalSamples; i++) {
    raw_samples[i] = ads.readADC_SingleEnded(2);
    delay(10);
  }

  for (int i = 0; i < totalSamples - 1; i++) {
    for (int j = i + 1; j < totalSamples; j++) {
      if (raw_samples[i] > raw_samples[j]) {
        int temp       = raw_samples[i];
        raw_samples[i] = raw_samples[j];
        raw_samples[j] = temp;
      }
    }
  }

  long total_adc = 0;
  for (int i = 10; i < 20; i++) total_adc += raw_samples[i];
  float avg_adc           = (float)total_adc / 10.0;
  float currentVoltage_pH = ads.computeVolts(avg_adc);

  float slope     = (6.86 - 4.01) / (V_pH686 - V_pH401);
  float instantPH = 6.86 + ((currentVoltage_pH - V_pH686) * slope);
  if (instantPH < 0.0)  instantPH = 0.0;
  if (instantPH > 14.0) instantPH = 14.0;

  ph_buffer[ph_index] = instantPH;
  ph_index = (ph_index + 1) % 20;

  float runningPH = 0;
  for (int i = 0; i < 20; i++) runningPH += ph_buffer[i];
  float finalPH = runningPH / 20.0;

  // ====================================================
  // 2. SENSOR TURBIDITY (Pin A1 ADS1115)
  // ====================================================
  ads.setGain(GAIN_ONE);
  delay(2);

  float bufferVolt[jumlahSampel];
  for (int i = 0; i < jumlahSampel; i++) {
    int16_t adcRaw = ads.readADC_SingleEnded(1);
    bufferVolt[i]  = ads.computeVolts(adcRaw);
    delay(5);
  }

  for (int i = 0; i < jumlahSampel - 1; i++) {
    for (int j = i + 1; j < jumlahSampel; j++) {
      if (bufferVolt[i] > bufferVolt[j]) {
        float temp    = bufferVolt[i];
        bufferVolt[i] = bufferVolt[j];
        bufferVolt[j] = temp;
      }
    }
  }

  float totalTengah  = 0;
  int   jumlahTengah = 5;
  int   startIdx     = (jumlahSampel / 2) - 2;
  for (int i = startIdx; i < startIdx + jumlahTengah; i++) {
    totalTengah += bufferVolt[i];
  }
  float voltFinal_Turb = totalTengah / jumlahTengah;

  float ntu_mentah = (voltJernih - voltFinal_Turb) * faktorNTU;
  if (ntu_mentah < 0) ntu_mentah = 0;

  static float ntuPenyimpanan = 0;
  ntuPenyimpanan = (0.40 * ntu_mentah) + (0.60 * ntuPenyimpanan);
  float ntu = ntuPenyimpanan;

  // ====================================================
  // 3. SENSOR ULTRASONIK (Pin 32 & 33)
  // ====================================================
  float totalJarak     = 0;
  int   jumlahSampelUS = 10;

  for (int i = 0; i < jumlahSampelUS; i++) {
    digitalWrite(pinTrig, LOW);
    delayMicroseconds(2);
    digitalWrite(pinTrig, HIGH);
    delayMicroseconds(10);
    digitalWrite(pinTrig, LOW);

    long  durasi  = pulseIn(pinEcho, HIGH, 30000);
    float jarakCm = (durasi * 0.034) / 2.0;
    totalJarak   += jarakCm;
    delay(15);
  }

  float jarakRataRata = totalJarak / jumlahSampelUS;
  float tinggiAir     = tinggiMaksimal - jarakRataRata;

  // ====================================================
  // BACA PERINTAH FIREBASE (fbdoRead — tiap 2 dtk)
  // ====================================================
  if (WiFi.status() == WL_CONNECTED && Firebase.ready() && (millis() - timerBaca > 2000)) {
    timerBaca = millis();

    if (Firebase.getString(fbdoRead, "/SistemAir/modeSistem")) {
      modeSistem = fbdoRead.stringData();
    }

    if (modeSistem == "MANUAL") {
      if (Firebase.getString(fbdoRead, "/SistemAir/pompaStatus")) {
        String perintahWeb = fbdoRead.stringData();
        if      (perintahWeb == "KURAS") statusSekarang = MANUAL_KURAS;
        else if (perintahWeb == "ISI")   statusSekarang = MANUAL_ISI;
        else                             statusSekarang = NORMAL;
      }
    }
  }

  // ====================================================
  // 4. OTAK OTOMASI POMPA (Warmup + Validasi + Cooldown)
  // ====================================================
  static unsigned long waktuMulaiKotor = 0;
  static unsigned long waktuMulaiKimia = 0;
  static unsigned long waktuSelesaiIsi = 0;

  const unsigned long TUNDA_TURBIDITY     = 3000;
  const unsigned long TUNDA_PH            = 3000;
  const unsigned long WAKTU_PEMANASAN     = 15000;
  const unsigned long JEDA_PASCAPENGISIAN = 300000;

  if (modeSistem == "AUTO" && (statusSekarang == MANUAL_KURAS || statusSekarang == MANUAL_ISI)) {
    statusSekarang = NORMAL;
  }

  if (modeSistem == "AUTO") {
    if (millis() < WAKTU_PEMANASAN ||
       (waktuSelesaiIsi != 0 && millis() - waktuSelesaiIsi < JEDA_PASCAPENGISIAN)) {
      statusSekarang  = NORMAL;
      waktuMulaiKotor = 0;
      waktuMulaiKimia = 0;
    } else {
      if (statusSekarang == NORMAL) {

        if (ntu > 50) {
          if (waktuMulaiKotor == 0) waktuMulaiKotor = millis();
          else if (millis() - waktuMulaiKotor >= TUNDA_TURBIDITY) {
            statusSekarang  = KURAS_KOTOR;
            waktuMulaiKotor = 0;
          }
        } else {
          waktuMulaiKotor = 0;
        }

        if (finalPH < 6.5 || finalPH > 8.5) {
          if (waktuMulaiKimia == 0) waktuMulaiKimia = millis();
          else if (millis() - waktuMulaiKimia >= TUNDA_PH) {
            statusSekarang  = KURAS_KIMIA;
            waktuMulaiKimia = 0;
          }
        } else {
          waktuMulaiKimia = 0;
        }
      }
    }
  }

  // ====================================================
  // 5. EKSEKUSI RELAY
  // ====================================================
  String pmpStatus = "OFF";

  switch (statusSekarang) {
    case KURAS_KOTOR:
    case KURAS_KIMIA:
      digitalWrite(PIN_RELAY_KURAS, RELAY_NYALA);
      digitalWrite(PIN_RELAY_ISI,   RELAY_MATI);
      pmpStatus = "KURAS";
      if (statusSekarang == KURAS_KOTOR && tinggiAir <= TARGET_KURAS_TURB) statusSekarang = PENGISIAN;
      if (statusSekarang == KURAS_KIMIA && tinggiAir <= TARGET_KURAS_PH)   statusSekarang = PENGISIAN;
      break;

    case PENGISIAN:
      digitalWrite(PIN_RELAY_KURAS, RELAY_MATI);
      digitalWrite(PIN_RELAY_ISI,   RELAY_NYALA);
      pmpStatus = "ISI";
      if (tinggiAir >= BATAS_PENUH) {
        statusSekarang  = NORMAL;
        waktuSelesaiIsi = millis();
      }
      break;

    case MANUAL_KURAS:
      digitalWrite(PIN_RELAY_KURAS, RELAY_NYALA);
      digitalWrite(PIN_RELAY_ISI,   RELAY_MATI);
      pmpStatus = "Manual-KURAS";
      break;

    case MANUAL_ISI:
      digitalWrite(PIN_RELAY_KURAS, RELAY_MATI);
      digitalWrite(PIN_RELAY_ISI,   RELAY_NYALA);
      pmpStatus = "Manual-ISI";
      break;

    case NORMAL:
    default:
      digitalWrite(PIN_RELAY_KURAS, RELAY_MATI);
      digitalWrite(PIN_RELAY_ISI,   RELAY_MATI);
      pmpStatus = "OFF";
      break;
  }

  // ====================================================
  // 6. SERIAL MONITOR
  // ====================================================
  Serial.print("ADC: ");          Serial.print(avg_adc, 0);
  Serial.print(" | Volt pH: ");   Serial.print(currentVoltage_pH, 3);
  Serial.print(" V | pH: ");      Serial.print(finalPH, 2);
  Serial.print(" | Volt Turb: "); Serial.print(voltFinal_Turb, 3);
  Serial.print(" V | NTU: ");     Serial.print(ntu, 0);
  Serial.print(" | Air: ");       Serial.print(tinggiAir, 1);
  Serial.print(" cm | Mode: ");   Serial.print(modeSistem);
  Serial.print(" | Status: ");

  if (statusSekarang == NORMAL) {
    if (waktuSelesaiIsi != 0 && millis() - waktuSelesaiIsi < JEDA_PASCAPENGISIAN && modeSistem == "AUTO")
      Serial.println("NORMAL (JEDA STABILISASI AIR)");
    else if (millis() < WAKTU_PEMANASAN)
      Serial.println("NORMAL (WARM-UP AWAL)");
    else
      Serial.println("NORMAL (SIAGA)");
  }
  else if (statusSekarang == KURAS_KOTOR)  Serial.println("KURAS KEKERUHAN (-30cm)");
  else if (statusSekarang == KURAS_KIMIA)  Serial.println("KURAS PH (-15cm)");
  else if (statusSekarang == PENGISIAN)    Serial.println("PENGISIAN");
  else if (statusSekarang == MANUAL_KURAS) Serial.println("MANUAL PENGURASAN (WEB)");
  else if (statusSekarang == MANUAL_ISI)   Serial.println("MANUAL PENGISIAN (WEB)");

  // ====================================================
  // 7. LCD DASHBOARD (Bergantian tiap 3 dtk)
  // ====================================================
  static unsigned long lcdMillis = 0;
  static bool lcdPage = false;

  if (statusSekarang != NORMAL) {
    lcd.setCursor(0, 0);
    lcd.print("POMPA: ");
    lcd.print((statusSekarang == PENGISIAN || statusSekarang == MANUAL_ISI) ? "PENGISIAN" : "KURAS AIR");
    lcd.print("  ");

    lcd.setCursor(0, 1);
    if (jarakRataRata > 400 || jarakRataRata < 2) {
      lcd.print("AIR: ERR        ");
    } else {
      lcd.print("AIR: ");
      lcd.print(tinggiAir, 1);
      lcd.print(" cm     ");
    }
  } else {
    if (millis() - lcdMillis > 3000) {
      lcdMillis = millis();
      lcdPage   = !lcdPage;
      lcd.clear();
    }

    if (lcdPage == false) {
      lcd.setCursor(0, 0); lcd.print("pH SENSOR: "); lcd.print(finalPH, 2);
      lcd.setCursor(0, 1); lcd.print("TURB: "); lcd.print((int)ntu); lcd.print(" NTU      ");
    } else {
      lcd.setCursor(0, 0);
      if (jarakRataRata > 400 || jarakRataRata < 2) {
        lcd.print("AIR: ERR        ");
      } else {
        lcd.print("TINGGI AIR: "); lcd.print((int)tinggiAir); lcd.print("cm ");
      }
      lcd.setCursor(0, 1);
      lcd.print("POMPA: ");
      if      (modeSistem == "MANUAL")                                                   lcd.print("MODE MANUAL  ");
      else if (waktuSelesaiIsi != 0 && millis() - waktuSelesaiIsi < JEDA_PASCAPENGISIAN) lcd.print("HOLD/COOLDOWN");
      else if (millis() < WAKTU_PEMANASAN)                                               lcd.print("WARM UP      ");
      else                                                                               lcd.print("SIAGA/OFF    ");
    }
  }

  // ====================================================
  // 8. KIRIM REALTIME KE FIREBASE (tiap 5 dtk)
  // ====================================================
  if (millis() - timerKirim > 5000) {
    timerKirim = millis();

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      time_t now; time(&now);

      String fbPompa = "OFF";
      if      (statusSekarang == KURAS_KOTOR || statusSekarang == KURAS_KIMIA || statusSekarang == MANUAL_KURAS) fbPompa = "KURAS";
      else if (statusSekarang == PENGISIAN   || statusSekarang == MANUAL_ISI)                                    fbPompa = "ISI";

      FirebaseJson payload;
      payload.set("pH",          finalPH);
      payload.set("turbidity",   ntu);
      payload.set("tinggiAir",   tinggiAir);
      payload.set("timestamp",   (double)now);
      payload.set("statusSistem", fbPompa);
      if (modeSistem == "AUTO") payload.set("pompaStatus", fbPompa);

      if (Firebase.updateNode(fbdoWrite, "/SistemAir", payload)) {
        Serial.println(" -> [Firebase: REALTIME TERKIRIM]");
      } else {
        Serial.printf(" -> [Firebase ERR: %s]\n", fbdoWrite.errorReason().c_str());
      }
    }
  }

  // ====================================================
  // 9. LOG RIWAYAT (SistemAir_Log — tiap 30 dtk)
  // ====================================================
  if (millis() - timerLog > 30000) {
    timerLog = millis();

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      time_t now; time(&now);

      if (now > 1600000000) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
          char dateStr[16];
          sprintf(dateStr, "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

          FirebaseJson payloadLog;
          payloadLog.set("pH",        finalPH);
          payloadLog.set("turbidity", ntu);
          payloadLog.set("tinggiAir", tinggiAir);
          payloadLog.set("timestamp", (double)now);

          String pathLog = "/SistemAir_Log/" + String(dateStr) + "/" + String(now);
          if (Firebase.setJSON(fbdoWrite, pathLog, payloadLog)) {
            Serial.println(" ===> [Firebase: LOG RIWAYAT TEREKAM]");
          } else {
            Serial.printf(" ===> [Firebase LOG ERR: %s]\n", fbdoWrite.errorReason().c_str());
          }
        }
      }
    }
  }

  // ====================================================
  // 10. REKAP HARIAN (SistemAir_Harian — tiap 30 dtk)
  // ====================================================
  if (millis() - timerHarian > 30000) {
    timerHarian = millis();

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        int  currentDay = timeinfo.tm_mday;
        char dateStr[16];
        sprintf(dateStr, "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

        // Reset statistik saat ganti hari
        if (lastDay != -1 && currentDay != lastDay) {
          phDailySum       = 0;
          turbDailySum     = 0;
          tinggiDailySum   = 0;
          dailySampleCount = 0;
          phMax   = finalPH;
          phMin   = finalPH;
          turbMax = ntu;
        }
        lastDay = currentDay;

        phDailySum     += finalPH;
        turbDailySum   += ntu;
        tinggiDailySum += tinggiAir;
        dailySampleCount++;
        if (finalPH > phMax) phMax   = finalPH;
        if (finalPH < phMin) phMin   = finalPH;
        if (ntu > turbMax)   turbMax = ntu;

        FirebaseJson payloadHarian;
        payloadHarian.set("rata_pH",              phDailySum     / dailySampleCount);
        payloadHarian.set("rata_turbidity",        turbDailySum   / dailySampleCount);
        payloadHarian.set("rata_tinggiAir",        tinggiDailySum / dailySampleCount);
        payloadHarian.set("pH_maksimum",           phMax);
        payloadHarian.set("pH_minimum",            phMin);
        payloadHarian.set("turbidity_maksimum",    turbMax);
        payloadHarian.set("total_sampel_hari_ini", dailySampleCount);
        payloadHarian.set("waktu_perbarui",        dateStr);

        String pathHarian = "/SistemAir_Harian/" + String(dateStr);
        if (Firebase.setJSON(fbdoWrite, pathHarian, payloadHarian)) {
          Serial.println(" ===> [Firebase: REKAP HARIAN DIPERBARUI]");
        } else {
          Serial.printf(" ===> [Firebase HARIAN ERR: %s]\n", fbdoWrite.errorReason().c_str());
        }
      }
    }
  }
}
