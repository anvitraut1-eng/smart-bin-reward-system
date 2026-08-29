/* Smart Bin ESP32 - account-aware reward firmware
 * Pins: HC-SR04 5/4, SW-420 27, button 21, RC522 18/19/23/15/22, LED 2.
 * WiFi + Supabase key belong in arduino_secrets.h.
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include "time.h"
#include "arduino_secrets.h"

const char* WIFI_SSID=WIFI_SSID_VALUE;
const char* WIFI_PASSWORD=WIFI_PASSWORD_VALUE;
const char* SUPABASE_ANON_KEY=SUPABASE_ANON_KEY_VALUE;
const char* SUPABASE_URL="https://bpecehlmvzuirxmruvyt.supabase.co/rest/v1";
String DEVICE_ID="BIN_ESP32_001";
#define TRIG_PIN 5
#define ECHO_PIN 4
#define VIBRATION_PIN 27
#define CALIBRATION_BUTTON_PIN 21
#define RC522_RST_PIN 22
#define RC522_SS_PIN 15
#define ONBOARD_LED_PIN 2

const float DEFAULT_BASELINE=25.0f;
const unsigned long READ_INTERVAL=30000, QUIET_MS=3000, SETTLE_MS=1000, BUTTON_MS=75, WIFI_RETRY_MS=30000;
const unsigned long REWARD_WINDOW_MS=10000, RATE_LIMIT_MS=300000;
const float MIN_RISE=2.0f;
const int POINTS=10;

Preferences prefs;
MFRC522 rfid(RC522_SS_PIN,RC522_RST_PIN);
float baseline=DEFAULT_BASELINE,currentFill=0;
bool calibrated=false,inMotion=false;
unsigned long lastRead=0,lastWifiRetry=0,motionStart=0,lastMotion=0,buttonChanged=0;
uint32_t motionStartPulses=0,processedPulses=0;
float fillBeforeMotion=0;
volatile uint32_t pulses=0;
volatile unsigned long lastPulse=0;
volatile bool pulsePending=false;
bool lastButton=HIGH;
struct Session{bool active;String uid;unsigned long started;float fillAtTap;}; Session session={false,"",0,0};
struct Limit{String uid;unsigned long time;}; Limit limits[10]; int limitCount=0;
struct Event{String endpoint,payload;}; Event buffer[20]; int bufferCount=0;

void IRAM_ATTR vibrationISR(){pulses++;lastPulse=millis();pulsePending=true;}
void led(bool on){digitalWrite(ONBOARD_LED_PIN,on?HIGH:LOW);}
void blink(int n,int on=150,int off=150){for(int i=0;i<n;i++){led(true);delay(on);led(false);if(i<n-1)delay(off);}}

float ultrasonic(){float a[5];int n=0;for(int i=0;i<5;i++){digitalWrite(TRIG_PIN,LOW);delayMicroseconds(2);digitalWrite(TRIG_PIN,HIGH);delayMicroseconds(10);digitalWrite(TRIG_PIN,LOW);unsigned long d=pulseIn(ECHO_PIN,HIGH,30000);if(d){float cm=d*.0343f/2.0f;if(cm>=2&&cm<=400)a[n++]=cm;}delay(60);}if(n<3)return NAN;for(int i=0;i<n-1;i++)for(int j=i+1;j<n;j++)if(a[i]>a[j]){float t=a[i];a[i]=a[j];a[j]=t;}return a[n/2];}
float fillPct(float d){if(!calibrated||isnan(d)||baseline<=0)return NAN;return constrain(((baseline-d)/baseline)*100.0f,0.0f,100.0f);}
String timestamp(){struct tm t;if(!getLocalTime(&t,1000))return "1970-01-01T00:00:00Z";char b[30];strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&t);return String(b);}

void wifi(){Serial.print("Connecting to WiFi: ");Serial.println(WIFI_SSID);WiFi.mode(WIFI_STA);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);for(int i=0;i<20&&WiFi.status()!=WL_CONNECTED;i++){delay(500);Serial.print(".");}Serial.println();if(WiFi.status()==WL_CONNECTED){Serial.println("WiFi connected!");Serial.print("IP address: ");Serial.println(WiFi.localIP());configTime(0,0,"pool.ntp.org","time.nist.gov");Serial.println("NTP synchronization started.");}else Serial.println("WiFi connection failed - will retry later.");}

bool post(const String& path,const String& body){if(WiFi.status()!=WL_CONNECTED)return false;HTTPClient h;if(!h.begin(String(SUPABASE_URL)+"/"+path))return false;h.setTimeout(6000);h.addHeader("Content-Type","application/json");h.addHeader("apikey",SUPABASE_ANON_KEY);h.addHeader("Authorization",String("Bearer ")+SUPABASE_ANON_KEY);h.addHeader("Prefer","return=minimal");int code=h.POST(body);bool ok=code>=200&&code<300;if(!ok){Serial.print("Supabase request failed: ");Serial.println(code);if(code>0)Serial.println(h.getString());}h.end();return ok;}
void sendOrBuffer(const String& path,const String& body){if(!post(path,body)){if(bufferCount>=20){for(int i=0;i<19;i++)buffer[i]=buffer[i+1];bufferCount=19;}buffer[bufferCount++]={path,body};Serial.print("Event buffered. Total: ");Serial.println(bufferCount);}}
void flushBuffer(){while(WiFi.status()==WL_CONNECTED&&bufferCount){if(!post(buffer[0].endpoint,buffer[0].payload))return;for(int i=0;i<bufferCount-1;i++)buffer[i]=buffer[i+1];bufferCount--;}}

void loadCalibration(){if(!prefs.begin("smartbin",true))return;float x=prefs.getFloat("baseline",NAN);prefs.end();if(!isnan(x)&&x>5&&x<400){baseline=x;calibrated=true;Serial.print("Loaded saved calibration: ");Serial.print(x,1);Serial.println(" cm");}else Serial.println("No valid saved calibration. Press button once with EMPTY bin.");}
bool calibrate(){Serial.println("CALIBRATION START - keep bin empty and still");blink(2,100,100);delay(1000);float d=ultrasonic();if(isnan(d)||d<=5||d>=400){Serial.println("CALIBRATION FAILED");blink(5,100,100);return false;}baseline=d;calibrated=true;currentFill=0;if(prefs.begin("smartbin",false)){prefs.putFloat("baseline",d);prefs.end();}Serial.print("Calibration successful: ");Serial.print(d,1);Serial.println(" cm");blink(3,180,180);Serial.println("CALIBRATION END");return true;}
void button(){bool s=digitalRead(CALIBRATION_BUTTON_PIN);unsigned long n=millis();if(s!=lastButton&&n-buttonChanged>=BUTTON_MS){buttonChanged=n;lastButton=s;if(s==LOW)calibrate();}}

void reading(){float d=ultrasonic();if(isnan(d)){Serial.println("Ultrasonic read failed.");return;}float f=fillPct(d);if(isnan(f)){Serial.println("No valid calibration.");return;}currentFill=f;Serial.print("Fill: ");Serial.print(f,1);Serial.print("% | Distance: ");Serial.print(d,1);Serial.println(" cm");String p="{\"device_id\":\""+DEVICE_ID+"\",\"fill_pct\":"+String(f,1)+",\"timestamp\":\""+timestamp()+"\"}";sendOrBuffer("bin_readings",p);}

bool limited(const String& uid){unsigned long n=millis();for(int i=0;i<limitCount;i++)if(limits[i].uid==uid)return n-limits[i].time<RATE_LIMIT_MS;return false;}
void rewardRecorded(const String& uid){unsigned long n=millis();for(int i=0;i<limitCount;i++)if(limits[i].uid==uid){limits[i].time=n;return;}if(limitCount<10){limits[limitCount++]={uid,n};return;}int old=0;for(int i=1;i<10;i++)if(limits[i].time<limits[old].time)old=i;limits[old]={uid,n};}

// The backend decides whether this UID belongs to an account. Linked cards get
// confirmed/10 points; unlinked cards become pending_link/0 points.
bool recordReward(const String& uid,float before,float after){String p="{\"p_card_uid\":\""+uid+"\",\"p_device_id\":\""+DEVICE_ID+"\",\"p_fill_before\":"+String(before,1)+",\"p_fill_after\":"+String(after,1)+",\"p_points\":"+String(POINTS)+"}";return post("rpc/record_reward",p);}

void motion(){unsigned long now=millis();uint32_t p;unsigned long lp;bool pending;noInterrupts();p=pulses;lp=lastPulse;pending=pulsePending;pulsePending=false;interrupts();if(p!=processedPulses||pending){processedPulses=p;if(!inMotion){inMotion=true;motionStart=now;motionStartPulses=p;fillBeforeMotion=currentFill;Serial.println("Motion window started.");}lastMotion=lp;}if(inMotion&&now-lastMotion>QUIET_MS){uint32_t count=p-motionStartPulses;unsigned long dur=lastMotion-motionStart;if(!session.active&&count>3&&dur>1000){delay(SETTLE_MS);float d=ultrasonic();float after=fillPct(d);if(!isnan(after)){float drop=fillBeforeMotion-after;String type=drop>10?"emptied":drop>3?"emptied_unconfirmed":"handling_no_empty";String q="{\"device_id\":\""+DEVICE_ID+"\",\"event_type\":\""+type+"\",\"fill_pct_before\":"+String(fillBeforeMotion,1)+",\"fill_pct_after\":"+String(after,1)+",\"pulse_count\":"+String(count)+",\"active_duration_ms\":"+String(dur)+",\"timestamp\":\""+timestamp()+"\"}";sendOrBuffer("empty_events",q);currentFill=after;}}inMotion=false;motionStartPulses=p;}}

void rfidTap(){if(!rfid.PICC_IsNewCardPresent()||!rfid.PICC_ReadCardSerial())return;String uid="";for(byte i=0;i<rfid.uid.size;i++){if(rfid.uid.uidByte[i]<16)uid+="0";uid+=String(rfid.uid.uidByte[i],HEX);}uid.toUpperCase();rfid.PICC_HaltA();rfid.PCD_StopCrypto1();Serial.print("RFID tap detected: ");Serial.println(uid);if(limited(uid)){Serial.println("Card rate limited.");blink(1,500,100);return;}if(session.active){Serial.println("Reward session already active.");return;}session={true,uid,millis(),currentFill};Serial.println("Reward session started - dispose waste within 10 seconds.");}

void rewardCheck(){if(!session.active)return;unsigned long now=millis();if(now-session.started>REWARD_WINDOW_MS){Serial.println("Disposal window expired.");session.active=false;return;}if(!inMotion)return;noInterrupts();uint32_t p=pulses;interrupts();uint32_t count=p-motionStartPulses;unsigned long dur=now-motionStart;if(count<3||dur<500)return;delay(SETTLE_MS);float d=ultrasonic();if(isnan(d))return;float after=fillPct(d);if(isnan(after))return;float rise=after-session.fillAtTap;Serial.print("Fill at tap: ");Serial.print(session.fillAtTap,1);Serial.print("% | Fill after: ");Serial.print(after,1);Serial.print("% | Rise: ");Serial.print(rise,1);Serial.println("%");if(rise>=MIN_RISE){Serial.println("DISPOSAL CONFIRMED - checking account");if(recordReward(session.uid,session.fillAtTap,after)){Serial.println("Reward processed by backend (linked or pending card).");rewardRecorded(session.uid);currentFill=after;session.active=false;blink(2,100,100);}else Serial.println("Reward NOT processed. No local points awarded.");}}

void setup(){Serial.begin(115200);delay(1000);Serial.println("\n================================\nSMART BIN ESP32 STARTING\n================================");pinMode(ONBOARD_LED_PIN,OUTPUT);led(false);pinMode(TRIG_PIN,OUTPUT);pinMode(ECHO_PIN,INPUT);digitalWrite(TRIG_PIN,LOW);pinMode(CALIBRATION_BUTTON_PIN,INPUT_PULLUP);lastButton=digitalRead(CALIBRATION_BUTTON_PIN);pinMode(VIBRATION_PIN,INPUT);noInterrupts();pulses=0;lastPulse=0;pulsePending=false;interrupts();attachInterrupt(digitalPinToInterrupt(VIBRATION_PIN),vibrationISR,RISING);SPI.begin();rfid.PCD_Init();Serial.println("RC522 initialized.");loadCalibration();wifi();if(calibrated)reading();lastRead=millis();blink(1,150,100);Serial.println("\nSMART BIN READY.\n");}
void loop(){unsigned long n=millis();if(WiFi.status()!=WL_CONNECTED&&n-lastWifiRetry>WIFI_RETRY_MS){wifi();lastWifiRetry=n;}else if(WiFi.status()==WL_CONNECTED&&bufferCount)flushBuffer();button();if(n-lastRead>READ_INTERVAL){reading();lastRead=n;}motion();rfidTap();rewardCheck();delay(10);}
