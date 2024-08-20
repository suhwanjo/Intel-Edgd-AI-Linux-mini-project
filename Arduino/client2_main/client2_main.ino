#define DEBUG // 디버깅 모드

#include <WiFiEsp.h>
#include <SoftwareSerial.h>
#include <MsTimer2.h> // 서보모터가 Timer1을 써서 2를 사용
#include <Wire.h> // I2C통신모듈
#include <LiquidCrystal_I2C.h>

#define AP_SSID "iot1"
#define AP_PASS "iot10000"
#define SERVER_NAME "10.10.141.67"
#define SERVER_PORT 5000
#define LOGID "KJD_ARD"
#define PASSWD "PASSWD"

#define BUZZER_PIN 9
#define WIFIRX 8  //8:RX-->ESP8266 TX
#define WIFITX 7  //7:TX -->ESP8266 RX

#define CMD_SIZE 50
#define ARR_CNT 5
bool timerIsrFlag = false;
bool buzzFlag = false;
unsigned long buzzDuration = 0;

char sendId[10] = "KJD_ARD";
char sendBuf[CMD_SIZE];
char lcdLine1[17] = "Smart IoT By KJD";
char lcdLine2[17] = "WiFi Connecting!";

char getSensorId[10];
int sensorTime; // 요청 시간을 저장하기 위한 변수 
unsigned int secCount;
bool updateTimeFlag = false;
bool updateDistanceFlag = false;
bool wakeupFlag = false;
bool previousWakeupFlag = false;
bool firstWakeupFlag = false;

typedef struct {
  int year;
  int month;
  int day;
  int hour;
  int min;
  int sec;
} DATETIME;
DATETIME dateTime = {0, 0, 0, 12, 0, 0};
DATETIME lastWakeup = {0, 0, 0, 0, 0, 0}; // 마지막 wakeup 시간을 저장
DATETIME firstWakeup = {0, 0, 0, 0, 0, 0}; // 최초 wakeup 시간을 저장

SoftwareSerial wifiSerial(WIFIRX, WIFITX);
WiFiEspClient client;
LiquidCrystal_I2C lcd(0x27, 16, 2); // I2C 통신 -> 주소를 바꾸면 여러개의 LCD도 사용 가능 

int temp, humid, distance;

int alarmHour = 7;
int alarmMinute = 0;
int valTime, valAlarm;

#define NORMAL_STATE 0
#define ALARM_STATE 1
int clock_state = NORMAL_STATE;
int previous_clock_state = NORMAL_STATE;
int reference = 61;
volatile int distance_count = 0;
int again_count = 0;

void setup() {
  lcd.init();
  lcd.backlight();
  lcdDisplay(0, 0, lcdLine1);
  lcdDisplay(0, 1, lcdLine2);

  pinMode(BUZZER_PIN, OUTPUT);

#ifdef DEBUG
  Serial.begin(115200); //DEBUG
#endif
  wifi_Setup();

  MsTimer2::set(1000, timerIsr); // 1000ms(1초) 주기로 타이머 인터럽트
  MsTimer2::start();
}

void loop() {
  if (client.available()) {
    socketEvent();
  }
  
  if (timerIsrFlag) //1초에 한번씩 실행
  {
    timerIsrFlag = false;
    if (!(secCount % 5)) //5초에 한번씩 실행
    {
      sprintf(lcdLine2, "A:%02d:%02d T:%02dH:%02d", alarmHour, alarmMinute, temp, humid);
      lcdDisplay(0, 1, lcdLine2);

      if (!client.connected()) {
        lcdDisplay(0, 1, "Server Down");
        server_Connect();
      }
    }

    valTime = dateTime.min + dateTime.hour*100;
    sprintf(lcdLine1, "%02d.%02d  %02d:%02d:%02d", dateTime.month, dateTime.day, dateTime.hour, dateTime.min, dateTime.sec );
    lcdDisplay(0, 0, lcdLine1);

    if (clock_state) {
      if (reference - distance >= 10) {
        if (distance_count >= 5)
          wakeupFlag = true;
        else
          distance_count++;
      } 
      else {
        distance_count = 0;
        wakeupFlag = false;
      }
    }

    if (updateTimeFlag)
    {
      client.print("[GETTIME]\n");
      if(distance != 0)
        reference = distance;
      updateTimeFlag = false;
    }

    if (buzzFlag) {
      buzzDuration++;
      if (buzzDuration >= 10) { // 1초 후에 부저를 끄기
        stopBuzzer();
        buzzFlag = false;
        buzzDuration = 0;
        Serial.println("Buzzer turned off");
      }
    }
  }

  valAlarm = alarmMinute + alarmHour*100;
  if((valTime >= valAlarm)&&(valTime < valAlarm + 30))
  {
    clock_state = ALARM_STATE;
  }
  else
  {
    clock_state = NORMAL_STATE;
  }
  if(clock_state != previous_clock_state)
  {
    if(clock_state)
    {
      client.print("[KJD_UBT]SERVO\n");
    }
    previous_clock_state = clock_state;
  }

  if(clock_state)
  {
    if(wakeupFlag != previousWakeupFlag) { // wakeupFlag의 상태가 변경되었을 때만 실행
      if(wakeupFlag)
      {
        if(!firstWakeupFlag)
        {
          firstWakeup = dateTime; // 최초 wakeup 시간을 저장
          firstWakeupFlag = true;
        }
        lastWakeup = dateTime;
        if (!buzzFlag) {
          startBuzzer(261); // 261Hz로 부저 울리기
          buzzFlag = true;
          buzzDuration = 0;
          Serial.println("Buzzer turned on");
        }
        again_count++;
        sprintf(sendBuf,"[KJD_SQL]ALARM@%02d%02d@%02d%02d@%d\n",firstWakeup.hour,firstWakeup.min,lastWakeup.hour,lastWakeup.min,again_count);
        client.print(sendBuf);
      }
      else
      {
        stopBuzzer();
      }
      previousWakeupFlag = wakeupFlag; // 이전 상태를 업데이트
    }
    else{
      if(wakeupFlag)
      {
        lcdDisplay(0, 1, "WAKE UP!!!!!!!!");
      }
      else
      {
        lcdDisplay(0, 1, "ALARM TIME");
      }
    }
  }
}


// 수신한 data의 소켓이벤트 처리
void socketEvent()
{
  int i = 0;
  char * pToken;
  char * pArray[ARR_CNT] = {0};
  char recvBuf[CMD_SIZE] = {0};
  int len;
  int distanceTemp;

  sendBuf[0] = '\0';
  len = client.readBytesUntil('\n', recvBuf, CMD_SIZE);
  client.flush();
#ifdef DEBUG
  //Serial.print("recv : ");
  //Serial.print(recvBuf);
#endif
  pToken = strtok(recvBuf, "[@]");
  while (pToken != NULL)
  {
    pArray[i] =  pToken;
    if (++i >= ARR_CNT)
      break;
    pToken = strtok(NULL, "[@]");
  }
  if (!strncmp(pArray[1], " New", 4)) // New Connected
  {
#ifdef DEBUG
    Serial.write('\n');
#endif
    strcpy(lcdLine2, "Server Connected");
    lcdDisplay(0, 1, lcdLine2);
    // 서버에서 getTime을 요청하기 위한 플래그
    updateTimeFlag = true;
    return ;
  }
  else if (!strncmp(pArray[1], " Alr", 4)) //Already logged
  {
#ifdef DEBUG
    Serial.write('\n');
#endif
    client.stop();
    server_Connect();
    updateTimeFlag = true;
    return ;
  }
  // GETTIME 
  else if(!strcmp(pArray[0],"GETTIME")) {  
    dateTime.year = (pArray[1][0]-0x30) * 10 + pArray[1][1]-0x30 ;
    dateTime.month =  (pArray[1][3]-0x30) * 10 + pArray[1][4]-0x30 ;
    dateTime.day =  (pArray[1][6]-0x30) * 10 + pArray[1][7]-0x30 ;
    dateTime.hour = (pArray[1][9]-0x30) * 10 + pArray[1][10]-0x30 ;
    // 11번이 요일
    dateTime.min =  (pArray[1][12]-0x30) * 10 + pArray[1][13]-0x30 ;
    dateTime.sec =  (pArray[1][15]-0x30) * 10 + pArray[1][16]-0x30 ;
//#ifdef DEBUG
    sprintf(sendBuf,"\nTime %02d.%02d.%02d %02d:%02d:%02d\n\r",dateTime.year,dateTime.month,dateTime.day,dateTime.hour,dateTime.min,dateTime.sec );
    Serial.println(sendBuf);
//#endif
    return;
  } 
  // 알람 시간을 설정하는 명령 처리
  else if(!strcmp(pArray[1],"HOUR")) {  
    alarmHour = atoi(pArray[2]); 
    sprintf(sendBuf, "Alarm hour set to %02d", alarmHour);
    Serial.println(sendBuf);
    return;
  }
  else if(!strcmp(pArray[1],"MINUTE")) {  
    alarmMinute = atoi(pArray[2]); 
    sprintf(sendBuf, "Alarm minute set to %02d", alarmMinute);
    Serial.println(sendBuf);
    return;
  }
  else if(!strcmp(pArray[1],"DHT")) {  
    humid = atoi(pArray[2]);
    temp = atoi(pArray[3]);
    //sprintf(sendBuf, "humid: %02d, temp: %02d", humid, temp);
    //Serial.println(sendBuf);
    return;
  }
  else if(!strcmp(pArray[1],"ULTR")) {  
    distance = atoi(pArray[2]); 
    //sprintf(sendBuf, "distance: %02d", distance);
    //Serial.println(sendBuf);
    return;
  }
  else
    return;

  client.write(sendBuf, strlen(sendBuf));
  client.flush();

#ifdef DEBUG
  Serial.print(", send : ");
  Serial.print(sendBuf);
#endif
}
void timerIsr()
{
  timerIsrFlag = true;
  secCount++;
  clock_calc(&dateTime);
}

// 시간 24시간 단위로 getTime을 요청함
void clock_calc(DATETIME *dateTime)
{
  int ret = 0;
  dateTime->sec++;          // increment second

  if(dateTime->sec >= 60)                              // if second = 60, second = 0
  { 
      dateTime->sec = 0;
      dateTime->min++; 
             
      if(dateTime->min >= 60)                          // if minute = 60, minute = 0
      { 
          dateTime->min = 0;
          dateTime->hour++;                               // increment hour
          if(dateTime->hour == 24) 
          {
            dateTime->hour = 0;
            updateTimeFlag = true;
          }
       }
    }
}

void wifi_Setup() {
  wifiSerial.begin(38400);
  wifi_Init();
  server_Connect();
}
void wifi_Init()
{
  do {
    WiFi.init(&wifiSerial);
    if (WiFi.status() == WL_NO_SHIELD) {
#ifdef DEBUG_WIFI
      Serial.println("WiFi shield not present");
#endif
    }
    else
      break;
  } while (1);

#ifdef DEBUG_WIFI
  Serial.print("Attempting to connect to WPA SSID: ");
  Serial.println(AP_SSID);
#endif
  while (WiFi.begin(AP_SSID, AP_PASS) != WL_CONNECTED) {
#ifdef DEBUG_WIFI
    Serial.print("Attempting to connect to WPA SSID: ");
    Serial.println(AP_SSID);
#endif
  }
  sprintf(lcdLine1, "ID:%s", LOGID);
  lcdDisplay(0, 0, lcdLine1);
  sprintf(lcdLine2, "%d.%d.%d.%d", WiFi.localIP()[0], WiFi.localIP()[1], WiFi.localIP()[2], WiFi.localIP()[3]);
  lcdDisplay(0, 1, lcdLine2);

#ifdef DEBUG_WIFI
  Serial.println("You're connected to the network");
  printWifiStatus();
#endif
}
int server_Connect()
{
#ifdef DEBUG_WIFI
  Serial.println("Starting connection to server...");
#endif

  if (client.connect(SERVER_NAME, SERVER_PORT)) {
#ifdef DEBUG_WIFI
    Serial.println("Connect to server");
#endif
    client.print("["LOGID":"PASSWD"]");
  }
  else
  {
#ifdef DEBUG_WIFI
    Serial.println("server connection failure");
#endif
  }
}
void printWifiStatus()
{
  // print the SSID of the network you're attached to

  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your WiFi shield's IP address
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength
  long rssi = WiFi.RSSI();
  Serial.print("Signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
}
void lcdDisplay(int x, int y, char * str)
{
  int len = 16 - strlen(str);
  lcd.setCursor(x, y);
  lcd.print(str);
  for (int i = len; i > 0; i--)
    lcd.write(' ');
}

void startBuzzer(int frequency) {
  // Timer1 설정 (CTC 모드)
  TCCR1A = 0; // Timer1 제어 레지스터 A 초기화
  TCCR1B = 0; // Timer1 제어 레지스터 B 초기화
  TCNT1 = 0;  // Timer1 카운터 초기화

  // 비교 일치 레지스터 설정 (주파수에 따라)
  OCR1A = 16000000 / (2 * 8 * frequency) - 1; // 8분주 사용 (16000000은 CPU 클록 속도)

  // CTC 모드와 8분주 설정
  TCCR1B |= (1 << WGM12) | (1 << CS11);

  // 비교 일치 인터럽트 활성화
  TIMSK1 |= (1 << OCIE1A);

  // 부저 핀을 출력 모드로 설정
  pinMode(BUZZER_PIN, OUTPUT);
}

void stopBuzzer() {
  // Timer1 정지
  TCCR1B = 0;
  // 부저 핀을 LOW로 설정
  digitalWrite(BUZZER_PIN, LOW);
}

ISR(TIMER1_COMPA_vect) {
  // 부저 핀 토글 (PWM 신호 생성)
  digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN));
}
