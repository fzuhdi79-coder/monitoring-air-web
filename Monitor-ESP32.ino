/* ========================================================
 * SISTEM MONITORING & OTOMASI AKUARIUM IOT (GABUNGAN FINAL)
 * FITUR: Sensor Asli + Cooldown/Warmup + LCD UX + Dual Path Firebase
 * + Log Riwayat Harian + Rekap Statistik Harian + DURASI POMPA (Terpisah)
 * INTERVAL: Realtime 5 dtk | Log & Harian 30 dtk
 * ========================================================
 */

// ==========================================
// SAKELAR MODE SIMULASI / HARDWARE ASLI
// ==========================================
#define MODE_SIMULASI true  // Set TRUE jika tanpa alat (simulasi), set FALSE jika alat asli dipasang!

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_ADS1X15.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include "time.h"

// ==========================================
// KREDENSIAL WIFI & FIREBASE
// ==========================================
#define WIFI_SSID     "zudistecu"
#define WIFI_PASSWORD "nicacantikk"
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
const float BATAS_TURBIDITY = 30.0;

// ==========================================
// VARIABEL SENSOR ULTRASONIK
// ==========================================
const float tinggiMaksimal = 88.0;

// ==========================================
// VARIABEL LOGIKA POMPA
// ==========================================
const float BATAS_PENUH       = 68.0;
const float TARGET_KURAS_TURB = BATAS_PENUH - 28.0;
const float TARGET_KURAS_PH   = BATAS_PENUH - 18.0;

enum StatusSistem { NORMAL, KURAS_KOTOR, KURAS_KIMIA, PENGISIAN, MANUAL_KURAS, MANUAL_ISI };
StatusSistem statusSekarang = NORMAL;
StatusSistem statusSistemSebelumnya = NORMAL; // <-- Variabel baru untuk mendeteksi perubahan status
String modeSistem = "AUTO";

// ==========================================
// Variabel Durasi Pompa & Tracking Status
// ==========================================
bool stateKurasSebelumnya = false;
bool stateIsiSebelumnya = false;

// Waktu transisi terakhir
unsigned long waktuMulaiKuras_ON = 0;
unsigned long waktuMulaiKuras_OFF = 0;
unsigned long waktuMulaiIsi_ON = 0;
unsigned long waktuMulaiIsi_OFF = 0;

// Akumulasi Harian
unsigned long durasiHarianKuras = 0; 
unsigned long durasiHarianIsi = 0;   

// ==========================================
// TIMER FIREBASE (Sesuai Permintaan)
// ==========================================
unsigned long timerKirim  = 5000;  // 5 detik
unsigned long timerBaca   = 0;
unsigned long timerLog    = 30000; // 30 detik
unsigned long timerHarian = 30000; // 30 detik

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
// VARIABEL INTERNAL MEMORI SIMULASI
// ==========================================
float sim_pH = 7.2;
float sim_NTU = 15.0;
float sim_tinggiAir = 68.0;
unsigned long timerSimulasi = 0;

// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);
  
  if (MODE_SIMULASI) {
    Serial.println("\n--- [SISTEM AKUARIUM IOT - MODE SIMULASI AKTIF] ---");
  } else {
    Serial.println("\n--- [SISTEM AKUARIUM IOT - MODE HARDWARE ASLI AKTIF] ---");
  }

  pinMode(pinTrig,         OUTPUT);
  pinMode(pinEcho,         INPUT);
  pinMode(PIN_RELAY_KURAS, OUTPUT);
  pinMode(PIN_RELAY_ISI,   OUTPUT);
  digitalWrite(PIN_RELAY_KURAS, RELAY_MATI);
  digitalWrite(PIN_RELAY_ISI,   RELAY_MATI);
  
  // Catat waktu mati awal untuk perhitungan durasi OFF saat booting
  waktuMulaiKuras_OFF = millis();
  waktuMulaiIsi_OFF = millis();

  /* Wire.begin(LCD_SDA, LCD_SCL);
  lcd.begin(16, 2); 
  lcd.backlight(); */

  if (!MODE_SIMULASI) {
    if (!ads.begin()) {
      Serial.println("[ERROR] ADS1115 tidak terdeteksi!");
      lcd.setCursor(0, 0); lcd.print("Error: ADS1115  ");
      while (1);
    }
  } else {
    Serial.println("[SIMULASI] ADS1115 di-bypass otomatis.");
  }

  for (int i = 0; i < 20; i++) ph_buffer[i] = 7.0;

  lcd.setCursor(0, 0); 
  lcd.print(MODE_SIMULASI ? "Mode Simulasi   " : "Sistem Akuarium ");
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

  float finalPH;
  float ntu;
  float tinggiAir;
  float currentVoltage_pH = 0;
  float voltFinal_Turb = 0;
  float jarakRataRata = 0;

  // ==========================================
  // PERCABANGAN SAKELAR (SIMULASI vs ASLI)
  // ==========================================
  if (MODE_SIMULASI) {
    if (millis() - timerSimulasi > 1000) {
      timerSimulasi = millis();
      float pergerakan_pH = random(-20, 21) / 100.0; 
      sim_pH += pergerakan_pH;
      if (sim_pH > 9.5) sim_pH = 9.5; if (sim_pH < 5.0) sim_pH = 5.0;

      float pergerakan_NTU = random(-5, 8); 
      sim_NTU += pergerakan_NTU;
      if (sim_NTU > 100) sim_NTU = 100; if (sim_NTU < 0) sim_NTU = 0;

      if (statusSekarang == KURAS_KOTOR || statusSekarang == KURAS_KIMIA || statusSekarang == MANUAL_KURAS) {
        sim_tinggiAir -= 1.5;
        if (sim_tinggiAir < 0) sim_tinggiAir = 0;
        sim_NTU -= 3.0; if(sim_NTU < 0) sim_NTU = 0;
        sim_pH += (7.0 - sim_pH) * 0.1;
      } 
      else if (statusSekarang == PENGISIAN || statusSekarang == MANUAL_ISI) {
        sim_tinggiAir += 1.5;
        if (sim_tinggiAir > BATAS_PENUH) sim_tinggiAir = BATAS_PENUH;
        sim_NTU -= 2.0; if(sim_NTU < 0) sim_NTU = 0;
        sim_pH += (7.2 - sim_pH) * 0.1; 
      }
    }
    finalPH = sim_pH;
    ntu = sim_NTU;
    tinggiAir = sim_tinggiAir;
    jarakRataRata = tinggiMaksimal - tinggiAir;

  } else {
    ads.setGain(GAIN_TWOTHIRDS);
    delay(2);
    int raw_samples[totalSamples];
    for (int i = 0; i < totalSamples; i++) { raw_samples[i] = ads.readADC_SingleEnded(2); delay(10); }
    for (int i = 0; i < totalSamples - 1; i++) {
      for (int j = i + 1; j < totalSamples; j++) {
        if (raw_samples[i] > raw_samples[j]) {
          int temp = raw_samples[i]; raw_samples[i] = raw_samples[j]; raw_samples[j] = temp;
        }
      }
    }
    long total_adc = 0;
    for (int i = 10; i < 20; i++) total_adc += raw_samples[i];
    currentVoltage_pH = ads.computeVolts((float)total_adc / 10.0);
    float slope = (6.86 - 4.01) / (V_pH686 - V_pH401);
    float instantPH = 6.86 + ((currentVoltage_pH - V_pH686) * slope);
    if (instantPH < 0.0) instantPH = 0.0; if (instantPH > 14.0) instantPH = 14.0;
    ph_buffer[ph_index] = instantPH; ph_index = (ph_index + 1) % 20;
    float runningPH = 0; for (int i = 0; i < 20; i++) runningPH += ph_buffer[i];
    finalPH = runningPH / 20.0;

    ads.setGain(GAIN_ONE);
    delay(2);
    float bufferVolt[jumlahSampel];
    for (int i = 0; i < jumlahSampel; i++) { bufferVolt[i] = ads.computeVolts(ads.readADC_SingleEnded(1)); delay(5); }
    for (int i = 0; i < jumlahSampel - 1; i++) {
      for (int j = i + 1; j < jumlahSampel; j++) {
        if (bufferVolt[i] > bufferVolt[j]) {
          float temp = bufferVolt[i]; bufferVolt[i] = bufferVolt[j]; bufferVolt[j] = temp;
        }
      }
    }
    float totalTengah = 0; int startIdx = (jumlahSampel / 2) - 2;
    for (int i = startIdx; i < startIdx + 5; i++) totalTengah += bufferVolt[i];
    voltFinal_Turb = totalTengah / 5;
    float ntu_mentah = (voltJernih - voltFinal_Turb) * faktorNTU;
    if (ntu_mentah < 0) ntu_mentah = 0;
    static float ntuPenyimpanan = 0;
    ntuPenyimpanan = (0.40 * ntu_mentah) + (0.60 * ntuPenyimpanan);
    ntu = ntuPenyimpanan;

    float totalJarak = 0;
    for (int i = 0; i < 10; i++) {
      digitalWrite(pinTrig, LOW); delayMicroseconds(2);
      digitalWrite(pinTrig, HIGH); delayMicroseconds(10);
      digitalWrite(pinTrig, LOW);
      long durasi = pulseIn(pinEcho, HIGH, 30000);
      totalJarak += (durasi * 0.034) / 2.0; delay(15);
    }
    jarakRataRata = totalJarak / 10.0;
    tinggiAir = tinggiMaksimal - jarakRataRata;
  }

  // ==============================
  // FIREBASE BACA MODE
  // ==============================
  // PERBAIKAN: Interval dipercepat dari 2000ms ke 500ms agar respons menekan tombol sangat instan
  if (WiFi.status() == WL_CONNECTED && Firebase.ready() && (millis() - timerBaca > 500)) {
    timerBaca = millis();
    if (Firebase.getString(fbdoRead, "/SistemAir/modeSistem")) modeSistem = fbdoRead.stringData();
    if (modeSistem == "MANUAL") {
      if (Firebase.getString(fbdoRead, "/SistemAir/pompaStatus")) {
        String perintahWeb = fbdoRead.stringData();
        if      (perintahWeb == "KURAS") statusSekarang = MANUAL_KURAS;
        else if (perintahWeb == "ISI")   statusSekarang = MANUAL_ISI;
        else                             statusSekarang = NORMAL;
      }
    }
  }

  // ==============================
  // OTOMASI POMPA (INSTAN / REAL-TIME)
  // ==============================
  if (modeSistem == "AUTO" && (statusSekarang == MANUAL_KURAS || statusSekarang == MANUAL_ISI)) {
      statusSekarang = NORMAL;
  }

  if (modeSistem == "AUTO") {
      if (statusSekarang == NORMAL) {
          // Begitu melebihi batas, langsung ganti status (tanpa delay/cooldown)
          if (ntu > BATAS_TURBIDITY) { 
              statusSekarang = KURAS_KOTOR; 
          } 
          else if (finalPH < 6.5 || finalPH > 8.5) { 
              statusSekarang = KURAS_KIMIA; 
          }
      }
  }

  // ==============================
  // EKSEKUSI RELAY & TRACKING STATUS
  // ==============================
  String pmpStatus = "OFF";
  bool isKuras = false;
  bool isIsi = false;

  switch (statusSekarang) {
    case KURAS_KOTOR:
    case KURAS_KIMIA:
      digitalWrite(PIN_RELAY_KURAS, RELAY_NYALA); digitalWrite(PIN_RELAY_ISI, RELAY_MATI); pmpStatus = "KURAS"; isKuras = true;
      if (statusSekarang == KURAS_KOTOR && tinggiAir <= TARGET_KURAS_TURB) statusSekarang = PENGISIAN;
      if (statusSekarang == KURAS_KIMIA && tinggiAir <= TARGET_KURAS_PH)   statusSekarang = PENGISIAN;
      break;

    case PENGISIAN:
      digitalWrite(PIN_RELAY_KURAS, RELAY_MATI); digitalWrite(PIN_RELAY_ISI, RELAY_NYALA); pmpStatus = "ISI"; isIsi = true;
      if (tinggiAir >= BATAS_PENUH) { statusSekarang = NORMAL; }
      break;

    case MANUAL_KURAS:
      digitalWrite(PIN_RELAY_KURAS, RELAY_NYALA); digitalWrite(PIN_RELAY_ISI, RELAY_MATI); pmpStatus = "Manual-KURAS"; isKuras = true; break;

    case MANUAL_ISI:
      digitalWrite(PIN_RELAY_KURAS, RELAY_MATI); digitalWrite(PIN_RELAY_ISI, RELAY_NYALA); pmpStatus = "Manual-ISI"; isIsi = true; break;

    case NORMAL:
    default:
      digitalWrite(PIN_RELAY_KURAS, RELAY_MATI); digitalWrite(PIN_RELAY_ISI, RELAY_MATI); pmpStatus = "OFF"; break;
  }

  // --- PERBAIKAN: PAKSA KIRIM DATA INSTAN KE FIREBASE SAAT STATUS POMPA BERUBAH ---
  // Jika pompa baru saja menyala ATAU baru saja mati, langsung lapor ke website
  // tanpa perlu menunggu timerKirim 5 detik.
  if (statusSekarang != statusSistemSebelumnya) {
    timerKirim = millis() - 5000; // Memotong antrean timer agar eksekusi tulis Firebase langsung jalan
    statusSistemSebelumnya = statusSekarang;
  }
  // --------------------------------------------------------------------------------

  // --- LOGIKA DURASI DAN STATUS KURAS ---
  if (isKuras && !stateKurasSebelumnya) {
    waktuMulaiKuras_ON = millis(); 
  } else if (!isKuras && stateKurasSebelumnya) {
    durasiHarianKuras += (millis() - waktuMulaiKuras_ON); 
    waktuMulaiKuras_OFF = millis();
  }
  stateKurasSebelumnya = isKuras;

  // --- LOGIKA DURASI DAN STATUS ISI ---
  if (isIsi && !stateIsiSebelumnya) {
    waktuMulaiIsi_ON = millis();
  } else if (!isIsi && stateIsiSebelumnya) {
    durasiHarianIsi += (millis() - waktuMulaiIsi_ON);
    waktuMulaiIsi_OFF = millis();
  }
  stateIsiSebelumnya = isIsi;


  // ==============================
  // SERIAL & LCD
  // ==============================
  Serial.print("pH: "); Serial.print(finalPH, 2);
  Serial.print(" | NTU: "); Serial.print(ntu, 0);
  Serial.print(" | Air: "); Serial.print(tinggiAir, 1);
  Serial.print(" | Mode: "); Serial.println(modeSistem);

  static unsigned long lcdMillis = 0;
  static bool lcdPage = false;
  if (millis() - lcdMillis > 3000) { lcdMillis = millis(); lcdPage = !lcdPage; lcd.clear(); }
  if (lcdPage == false) {
    lcd.setCursor(0, 0); lcd.print("pH SENSOR: "); lcd.print(finalPH, 2);
    lcd.setCursor(0, 1); lcd.print("TURB: "); lcd.print((int)ntu); lcd.print(" NTU");
  } else {
    lcd.setCursor(0, 0); lcd.print("TINGGI: "); lcd.print((int)tinggiAir); lcd.print(" cm");
    lcd.setCursor(0, 1); lcd.print("POMPA: "); lcd.print(pmpStatus);
  }

  // ==============================
  // KIRIM REALTIME FIREBASE (5s)
  // ==============================
  if (millis() - timerKirim > 5000) {
    timerKirim = millis();
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      time_t now; time(&now);
      String fbPompa = "OFF";
      if      (isKuras) fbPompa = "KURAS";
      else if (isIsi)   fbPompa = "ISI";

      FirebaseJson payload;
      payload.set("pH", finalPH); 
      payload.set("turbidity", ntu); 
      payload.set("tinggiAir", tinggiAir);
      payload.set("timestamp", (double)now); 
      payload.set("statusSistem", fbPompa);
      
      if (modeSistem == "AUTO") payload.set("pompaStatus", fbPompa);

      // Status Terpisah
      payload.set("pompa_kuras", isKuras ? "ON" : "OFF");
      payload.set("pompa_isi", isIsi ? "ON" : "OFF");

      // Perhitungan durasi real-time untuk ON/OFF (dalam detik)
      unsigned long curTime = millis();
      int durasiKurasON_detik = isKuras ? (curTime - waktuMulaiKuras_ON) / 1000 : 0;
      int durasiKurasOFF_detik = !isKuras ? (curTime - waktuMulaiKuras_OFF) / 1000 : 0;
      
      int durasiIsiON_detik = isIsi ? (curTime - waktuMulaiIsi_ON) / 1000 : 0;
      int durasiIsiOFF_detik = !isIsi ? (curTime - waktuMulaiIsi_OFF) / 1000 : 0;

      payload.set("durasi_kuras_ON_saat_ini_detik", durasiKurasON_detik);
      payload.set("durasi_kuras_OFF_saat_ini_detik", durasiKurasOFF_detik);
      payload.set("durasi_isi_ON_saat_ini_detik", durasiIsiON_detik);
      payload.set("durasi_isi_OFF_saat_ini_detik", durasiIsiOFF_detik);

      Firebase.updateNode(fbdoWrite, "/SistemAir", payload);
    }
  }

  // ==============================
  // LOG RIWAYAT 30s
  // ==============================
  static unsigned long lastLogMillis = 0;
  if (millis() - lastLogMillis > timerLog) {
    lastLogMillis = millis();
    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      time_t now; time(&now);
      if (now > 1600000000) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
          char dateStr[16]; sprintf(dateStr, "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
          
          FirebaseJson payloadLog;
          payloadLog.set("pH", finalPH); 
          payloadLog.set("turbidity", ntu); 
          payloadLog.set("tinggiAir", tinggiAir);
          payloadLog.set("timestamp", (double)now);
          
          // --- TAMBAHAN LOG STATUS & DURASI POMPA PADA HISTORICAL DATA ---
          payloadLog.set("pompa_kuras", isKuras ? "ON" : "OFF");
          payloadLog.set("pompa_isi", isIsi ? "ON" : "OFF");

          unsigned long curTimeLog = millis();
          payloadLog.set("durasi_kuras_ON_detik", isKuras ? (curTimeLog - waktuMulaiKuras_ON) / 1000 : 0);
          payloadLog.set("durasi_kuras_OFF_detik", !isKuras ? (curTimeLog - waktuMulaiKuras_OFF) / 1000 : 0);
          payloadLog.set("durasi_isi_ON_detik", isIsi ? (curTimeLog - waktuMulaiIsi_ON) / 1000 : 0);
          payloadLog.set("durasi_isi_OFF_detik", !isIsi ? (curTimeLog - waktuMulaiIsi_OFF) / 1000 : 0);
          // --------------------------------------------------------------

          String pathLog = "/SistemAir_Log/" + String(dateStr) + "/" + String(now);
          Firebase.setJSON(fbdoWrite, pathLog, payloadLog);
        }
      }
    }
  }

  // ==============================
  // REKAP HARIAN 30s
  // ==============================
  static unsigned long lastHarianMillis = 0;
  if (millis() - lastHarianMillis > timerHarian) {
    lastHarianMillis = millis();

    if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        int currentDay = timeinfo.tm_mday;
        char dateStr[16]; sprintf(dateStr, "%04d-%02d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

        // Reset jika ganti hari
        if (lastDay != -1 && currentDay != lastDay) {
          phDailySum = 0; turbDailySum = 0; tinggiDailySum = 0; dailySampleCount = 0;
          phMax = finalPH; phMin = finalPH; turbMax = ntu;
          durasiHarianKuras = 0; 
          durasiHarianIsi = 0;   
        }
        lastDay = currentDay;

        phDailySum += finalPH; turbDailySum += ntu; tinggiDailySum += tinggiAir; dailySampleCount++;
        if (finalPH > phMax) phMax = finalPH; if (finalPH < phMin) phMin = finalPH;
        if (ntu > turbMax) turbMax = ntu;

        unsigned long durasiKurasTampil = durasiHarianKuras;
        if (isKuras) durasiKurasTampil += (millis() - waktuMulaiKuras_ON);
        
        unsigned long durasiIsiTampil = durasiHarianIsi;
        if (isIsi) durasiIsiTampil += (millis() - waktuMulaiIsi_ON);

        FirebaseJson payloadHarian;
        payloadHarian.set("rata_pH", phDailySum / dailySampleCount);
        payloadHarian.set("rata_turbidity", turbDailySum / dailySampleCount);
        payloadHarian.set("rata_tinggiAir", tinggiDailySum / dailySampleCount);
        payloadHarian.set("pH_maksimum", phMax); payloadHarian.set("pH_minimum", phMin); payloadHarian.set("turbidity_maksimum", turbMax);
        payloadHarian.set("total_sampel_hari_ini", dailySampleCount);
        payloadHarian.set("waktu_perbarui", dateStr);
        
        payloadHarian.set("durasi_kuras_total_harian_detik", (int)(durasiKurasTampil / 1000));
        payloadHarian.set("durasi_isi_total_harian_detik", (int)(durasiIsiTampil / 1000));

        String pathHarian = "/SistemAir_Harian/" + String(dateStr);
        Firebase.setJSON(fbdoWrite, pathHarian, payloadHarian);
      }
    }
  }
}s
