#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define WIFI_SSID "Muh Japri"
#define WIFI_PASSWORD "bismillah"

// Ganti IP sesuai alamat IP laptop/PC Anda di jaringan Wi-Fi (Wajib sertakan http://)
#define LARAVEL_URL "http://10.183.199.141:8000"

// --- PEMETAAN PIN ESP32 ---
// Pintu Masuk
#define TRIG_MASUK 23
#define ECHO_MASUK 22
#define SERVO_MASUK_PIN 21
#define LED_HIJAU_MASUK 19  // LED Hijau Pintu Masuk
#define LED_MERAH_MASUK 18  // LED Merah Pintu Masuk

// Pintu Keluar
#define TRIG_KELUAR 13
#define ECHO_KELUAR 14
#define SERVO_KELUAR_PIN 27
#define LED_HIJAU_KELUAR 33 // LED Hijau Pintu Keluar
#define LED_MERAH_KELUAR 32 // LED Merah Pintu Keluar

// Buzzer Bersama
#define BUZZER 5

// LCD I2C
#define LCD_SDA 4
#define LCD_SCL 15

// ID Gate untuk Masuk & Keluar
const char* GATE_MASUK  = "gate_masuk";
const char* GATE_KELUAR = "gate_keluar";

const int JARAK_DETEKSI = 15;
const int SUDUT_TUTUP = 0;
const int SUDUT_BUKA = 90;      // Sudut buka aman
const int LANGKAH_MS = 15;      // Kecepatan gerak servo pelan (smooth)

const unsigned long TIMEOUT_MENUNGGU = 30000;
const unsigned long JEDA_TUTUP_AMAN = 2000;   // Waktu tunggu setelah mobil bersih dari sensor
const unsigned long MAKS_BUKA = 20000;
const unsigned long JEDA_SETELAH = 3000;
const unsigned long POLL_MS = 1500;

LiquidCrystal_I2C lcd(0x27, 16, 2);

enum Keadaan {
  SIAGA,
  MENUNGGU,
  TERBUKA,
  TUNGGU_KOSONG,
  JEDA
};

// State Pintu Masuk
Keadaan keadaanMasuk = SIAGA;

// State Pintu Keluar
Keadaan keadaanKeluar = SIAGA;

Servo servoMasuk;
Servo servoKeluar;

bool mobilAdaMasuk = false;
bool mobilAdaKeluar = false;
int deteksiBerturutMasuk = 0;
int deteksiBerturutKeluar = 0;

unsigned long terakhirUkurMasuk = 0;
unsigned long terakhirUkurKeluar = 0;

// Timer Pintu Masuk
unsigned long tMenungguMasuk = 0;
unsigned long tBukaMasuk = 0;
unsigned long tKosongMasuk = 0;
unsigned long terakhirPollMasuk = 0;

// Timer Pintu Keluar
unsigned long tMenungguKeluar = 0;
unsigned long tBukaKeluar = 0;
unsigned long tKosongKeluar = 0;
unsigned long terakhirPollKeluar = 0;

// ============================================================
// FUNGSI BANTU
// ============================================================
void tampilLCD(String baris1, String baris2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(baris1);
  lcd.setCursor(0, 1);
  lcd.print(baris2);
}

void bipBlocking(int kali, int lama) {
  for (int i = 0; i < kali; i++) {
    digitalWrite(BUZZER, HIGH);
    delay(lama);
    digitalWrite(BUZZER, LOW);
    delay(lama);
  }
}

void gerakServoPelan(Servo &servoObj, int dari, int ke) {
  int arah = (ke > dari) ? 1 : -1;
  for (int sudut = dari; sudut != ke; sudut += arah) {
    servoObj.write(sudut);
    delay(LANGKAH_MS);
  }
  servoObj.write(ke);
}

long ukurJarak(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long d = pulseIn(echoPin, HIGH, 30000);
  if (d == 0) return 999;
  return d * 0.034 / 2;
}

// ============================================================
// WIFI & HTTP
// ============================================================
bool pastikanWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println("\nWiFi terputus, mencoba reconnect...");
  WiFi.disconnect();
  delay(200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 10000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi tersambung, IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  return false;
}

int postLaravel(String endpoint, String body, String &response) {
  if (!pastikanWiFi()) {
    Serial.println("[HTTP] WiFi belum tersambung!");
    return -1;
  }

  WiFiClient client;
  HTTPClient http;
  String url = String(LARAVEL_URL) + endpoint;

  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    Serial.printf("[HTTP] Gagal inisialisasi URL: %s (Pastikan ada http://)\n", url.c_str());
    return -2;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");

  int httpCode = http.POST(body);
  if (httpCode > 0) {
    response = http.getString();
  } else {
    Serial.printf("[HTTP ERROR] POST ke %s gagal! Code: %d (%s)\n",
                  url.c_str(), httpCode, http.errorToString(httpCode).c_str());
  }
  http.end();
  client.stop();
  return httpCode;
}

bool kirimTrigger(const char* gateId) {
  String response;
  String body = "{\"gate\":\"" + String(gateId) + "\"}";

  Serial.printf("[TRIGGER] Mengirim trigger untuk gate: %s ke %s ...\n", gateId, LARAVEL_URL);
  for (int percobaan = 1; percobaan <= 2; percobaan++) {
    int httpCode = postLaravel("/api/parking/trigger", body, response);
    if (httpCode >= 200 && httpCode < 300) {
      Serial.printf("[TRIGGER] Berhasil! HTTP %d, Response: %s\n", httpCode, response.c_str());
      return true;
    }
    Serial.printf("[TRIGGER] Percobaan %d gagal (HTTP %d).\n", percobaan, httpCode);
    if (percobaan < 2) delay(1000);
  }
  return false;
}

String bacaIzin(const char* gateId) {
  if (!pastikanWiFi()) return "";

  WiFiClient client;
  HTTPClient http;
  String url = String(LARAVEL_URL) + "/api/parking/command?gate=" + String(gateId);

  http.setTimeout(10000);
  if (!http.begin(client, url)) return "";

  http.addHeader("Connection", "close");
  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    client.stop();
    return "";
  }

  String response = http.getString();
  http.end();
  client.stop();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) return "";

  const char* izin = doc["data"]["izin"];
  if (izin == nullptr) return "";

  return String(izin);
}

bool resetGate(const char* gateId) {
  String response;
  String body = "{\"gate\":\"" + String(gateId) + "\"}";
  int httpCode = postLaravel("/api/parking/reset", body, response);
  return (httpCode >= 200 && httpCode < 300);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();

  tampilLCD("SMART PARKING", "MEMULAI SISTEM");

  pinMode(TRIG_MASUK, OUTPUT);
  pinMode(ECHO_MASUK, INPUT);
  pinMode(TRIG_KELUAR, OUTPUT);
  pinMode(ECHO_KELUAR, INPUT);

  pinMode(LED_HIJAU_MASUK, OUTPUT);
  pinMode(LED_MERAH_MASUK, OUTPUT);
  pinMode(LED_HIJAU_KELUAR, OUTPUT);
  pinMode(LED_MERAH_KELUAR, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  // Set Kondisi Awal LED (Hijau Nyala, Merah Mati)
  digitalWrite(LED_HIJAU_MASUK, HIGH);
  digitalWrite(LED_MERAH_MASUK, LOW);
  digitalWrite(LED_HIJAU_KELUAR, HIGH);
  digitalWrite(LED_MERAH_KELUAR, LOW);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  
  servoMasuk.setPeriodHertz(50);
  servoKeluar.setPeriodHertz(50);
  
  servoMasuk.attach(SERVO_MASUK_PIN, 500, 2400);
  servoKeluar.attach(SERVO_KELUAR_PIN, 500, 2400);
  
  servoMasuk.write(SUDUT_TUTUP);
  servoKeluar.write(SUDUT_TUTUP);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  tampilLCD("SMART PARKING", "CONNECT WIFI...");
  unsigned long mulaiWiFi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulaiWiFi < 20000) {
    delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {
    tampilLCD("WIFI GAGAL", "CEK KONEKSI");
    return;
  }

  tampilLCD("CONNECT SERVER", "HARAP TUNGGU");
  resetGate(GATE_MASUK);
  resetGate(GATE_KELUAR);

  tampilLCD("SELAMAT DATANG", "SILAKAN MAJU");
  Serial.println("=== SISTEM SIAGA (MASUK & KELUAR) ===");
}

// ============================================================
// LOOP UTAMA
// ============================================================
void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    pastikanWiFi();
  }

  // ============================================================
  // 1. PINTU MASUK (Ultrasonik Masuk & Servo Masuk)
  // ============================================================
  if (now - terakhirUkurMasuk >= 100) {
    terakhirUkurMasuk = now;
    long jarakMasuk = ukurJarak(TRIG_MASUK, ECHO_MASUK);
    mobilAdaMasuk = (jarakMasuk < JARAK_DETEKSI);

    if (keadaanMasuk == SIAGA) {
      if (mobilAdaMasuk) deteksiBerturutMasuk++;
      else deteksiBerturutMasuk = 0;

      if (deteksiBerturutMasuk >= 3) {
        deteksiBerturutMasuk = 0;
        Serial.println("[MASUK] Kendaraan terdeteksi -> Trigger verifikasi");

        tampilLCD("GATE MASUK", "CEK WAJAH & PLAT");
        digitalWrite(LED_HIJAU_MASUK, LOW);
        digitalWrite(LED_MERAH_MASUK, HIGH);
        bipBlocking(1, 60);

        bool berhasil = kirimTrigger(GATE_MASUK);
        if (berhasil) {
          tMenungguMasuk = millis();
          terakhirPollMasuk = millis();
          keadaanMasuk = MENUNGGU;
        } else {
          tampilLCD("SISTEM OFFLINE", "HUBUNGI PETUGAS");
          bipBlocking(3, 150);
          resetGate(GATE_MASUK);
          digitalWrite(LED_HIJAU_MASUK, HIGH);
          digitalWrite(LED_MERAH_MASUK, LOW);
          keadaanMasuk = SIAGA;
        }
      }
    }
  }

  // State MENUNGGU (Pintu Masuk)
  if (keadaanMasuk == MENUNGGU) {
    now = millis();
    if (now - tMenungguMasuk > TIMEOUT_MENUNGGU) {
      tampilLCD("WAKTU HABIS", "AKSES DITOLAK");
      bipBlocking(3, 150);
      resetGate(GATE_MASUK);
      digitalWrite(LED_HIJAU_MASUK, HIGH);
      digitalWrite(LED_MERAH_MASUK, LOW);
      tampilLCD("SELAMAT DATANG", "SILAKAN MAJU");
      keadaanMasuk = SIAGA;
    }
    else if (now - terakhirPollMasuk >= POLL_MS) {
      terakhirPollMasuk = now;
      String izin = bacaIzin(GATE_MASUK);

      if (izin == "BUKA") {
        tampilLCD("AKSES DITERIMA", "SILAKAN MASUK");
        gerakServoPelan(servoMasuk, SUDUT_TUTUP, SUDUT_BUKA);
        bipBlocking(2, 70);
        tBukaMasuk = millis();
        keadaanMasuk = TERBUKA;
      }
      else if (izin == "TOLAK") {
        tampilLCD("AKSES DITOLAK", "SILAKAN ULANGI");
        bipBlocking(3, 150);
        resetGate(GATE_MASUK);
        digitalWrite(LED_HIJAU_MASUK, HIGH);
        digitalWrite(LED_MERAH_MASUK, LOW);
        tampilLCD("SELAMAT DATANG", "SILAKAN MAJU");
        keadaanMasuk = SIAGA;
      }
    }
  }

  // State TERBUKA & TUNGGU_KOSONG (Pintu Masuk)
  if (keadaanMasuk == TERBUKA) {
    long jarakMasuk = ukurJarak(TRIG_MASUK, ECHO_MASUK);
    if (jarakMasuk >= JARAK_DETEKSI) {
      tKosongMasuk = millis();
      keadaanMasuk = TUNGGU_KOSONG;
    }
  }

  if (keadaanMasuk == TUNGGU_KOSONG) {
    now = millis();
    long jarakMasuk = ukurJarak(TRIG_MASUK, ECHO_MASUK);

    if (jarakMasuk < JARAK_DETEKSI) {
      keadaanMasuk = TERBUKA; 
    }
    else if (now - tKosongMasuk >= JEDA_TUTUP_AMAN) {
      gerakServoPelan(servoMasuk, SUDUT_BUKA, SUDUT_TUTUP);
      bipBlocking(1, 50);
      resetGate(GATE_MASUK);
      digitalWrite(LED_HIJAU_MASUK, HIGH);
      digitalWrite(LED_MERAH_MASUK, LOW);
      tampilLCD("SELAMAT DATANG", "SILAKAN MAJU");
      keadaanMasuk = SIAGA;
    }
  }

  // ============================================================
  // 2. PINTU KELUAR (Ultrasonik Keluar & Servo Keluar)
  // ============================================================
  if (now - terakhirUkurKeluar >= 100) {
    terakhirUkurKeluar = now;
    long jarakKeluar = ukurJarak(TRIG_KELUAR, ECHO_KELUAR);
    mobilAdaKeluar = (jarakKeluar < JARAK_DETEKSI);

    if (keadaanKeluar == SIAGA) {
      if (mobilAdaKeluar) deteksiBerturutKeluar++;
      else deteksiBerturutKeluar = 0;

      if (deteksiBerturutKeluar >= 3) {
        deteksiBerturutKeluar = 0;
        Serial.println("[KELUAR] Kendaraan terdeteksi -> Trigger verifikasi");

        tampilLCD("GATE KELUAR", "CEK WAJAH & PLAT");
        digitalWrite(LED_HIJAU_KELUAR, LOW);
        digitalWrite(LED_MERAH_KELUAR, HIGH);
        bipBlocking(1, 60);

        bool berhasil = kirimTrigger(GATE_KELUAR);
        if (berhasil) {
          tMenungguKeluar = millis();
          terakhirPollKeluar = millis();
          keadaanKeluar = MENUNGGU;
        } else {
          tampilLCD("SISTEM OFFLINE", "HUBUNGI PETUGAS");
          bipBlocking(3, 150);
          resetGate(GATE_KELUAR);
          digitalWrite(LED_HIJAU_KELUAR, HIGH);
          digitalWrite(LED_MERAH_KELUAR, LOW);
          keadaanKeluar = SIAGA;
        }
      }
    }
  }

  // State MENUNGGU (Pintu Keluar)
  if (keadaanKeluar == MENUNGGU) {
    now = millis();
    if (now - tMenungguKeluar > TIMEOUT_MENUNGGU) {
      tampilLCD("WAKTU HABIS", "AKSES DITOLAK");
      bipBlocking(3, 150);
      resetGate(GATE_KELUAR);
      digitalWrite(LED_HIJAU_KELUAR, HIGH);
      digitalWrite(LED_MERAH_KELUAR, LOW);
      tampilLCD("SELAMAT DATANG", "SILAKAN MAJU");
      keadaanKeluar = SIAGA;
    }
    else if (now - terakhirPollKeluar >= POLL_MS) {
      terakhirPollKeluar = now;
      String izin = bacaIzin(GATE_KELUAR);

      if (izin == "BUKA") {
        tampilLCD("AKSES DITERIMA", "SILAKAN KELUAR");
        gerakServoPelan(servoKeluar, SUDUT_TUTUP, SUDUT_BUKA);
        bipBlocking(2, 70);
        tBukaKeluar = millis();
        keadaanKeluar = TERBUKA;
      }
      else if (izin == "TOLAK") {
        tampilLCD("AKSES DITOLAK", "HUBUNGI PETUGAS");
        bipBlocking(3, 150);
        resetGate(GATE_KELUAR);
        digitalWrite(LED_HIJAU_KELUAR, HIGH);
        digitalWrite(LED_MERAH_KELUAR, LOW);
        tampilLCD("SELAMAT DATANG", "SILAKAN MAJU");
        keadaanKeluar = SIAGA;
      }
    }
  }

  // State TERBUKA & TUNGGU_KOSONG (Pintu Keluar)
  if (keadaanKeluar == TERBUKA) {
    long jarakKeluar = ukurJarak(TRIG_KELUAR, ECHO_KELUAR);
    if (jarakKeluar >= JARAK_DETEKSI) {
      tKosongKeluar = millis();
      keadaanKeluar = TUNGGU_KOSONG;
    }
  }

  if (keadaanKeluar == TUNGGU_KOSONG) {
    now = millis();
    long jarakKeluar = ukurJarak(TRIG_KELUAR, ECHO_KELUAR);

    if (jarakKeluar < JARAK_DETEKSI) {
      keadaanKeluar = TERBUKA;
    }
    else if (now - tKosongKeluar >= JEDA_TUTUP_AMAN) {
      gerakServoPelan(servoKeluar, SUDUT_BUKA, SUDUT_TUTUP);
      bipBlocking(1, 50);
      resetGate(GATE_KELUAR);
      digitalWrite(LED_HIJAU_KELUAR, HIGH);
      digitalWrite(LED_MERAH_KELUAR, LOW);
      tampilLCD("SELAMAT JALAN", "TERIMA KASIH");
      delay(1500);
      tampilLCD("SELAMAT DATANG", "SILAKAN MAJU");
      keadaanKeluar = SIAGA;
    }
  }
}
