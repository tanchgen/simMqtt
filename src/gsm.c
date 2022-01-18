/*
 * gsm.c
 *
 *  Created on: 3 окт. 2021 г.
 *      Author: jet
 */
#include <stdio.h>
#include <string.h>

#include "my_ntp.h"
#include "mqtt.h"
#include "logger.h"
#include "flash.h"
#include "usart_arch.h"
#include "gpio_arch.h"
#include "uspd.h"
#include "events.h"
#include "gsm.h"

extern SIM800_t SIM800;
//extern const uint32_t baudrate[BAUD_NUM];

static uint32_t tmpTick;

eGsmState gsmState = GSM_OFF;

FlagStatus gsmRun = SET;
eGsmRunPhase gsmRunPhase = PHASE_OFF_OK;

eGsmState gsmStRestart = GSM_OFF;

FlagStatus ntpFlag;

uint8_t reconnCount = 0;
uint16_t gprsConnTout[] = {
  2000,
  3000,
  3000,
  3000
};


struct timer_list gsmOnToutTimer;

// -------------- Function prototype ------------------------------------------
//void simUartBaud( uint32_t baudrate );
void simUartHwFlow( void );

int mqttSubProcess(void);

int simWaitReady( void );
int gprsConnTest( void );
int simStartInit(void);
int gprsConnTest( void );
int gprsConnBreak( void );
int gprsConn( void );
int ntpInit(void);
int TCP_Connect(void);
int clkSet( void );
// ----------------------------------------------------------------------------

/**
 * Обработка таймаута процесса включения/подключения GSM/GPRS/NTP/TCP/MQTT
 * @param arg состояние GSM-машины
 * @return non
 */
static void gsmOnTout( uintptr_t arg ){
  eGsmState gsmst = (eGsmState)arg;

  trace_printf( "gsmTout %d", gsmst);

  switch( gsmst){
    case GSM_INIT:
      evntFlags.gsmFault = SET;
      break;
    case GSM_START_INIT:
      evntFlags.gprsFault = SET;
      gsmStRestart = GSM_OFF;
      gsmRun = RESET;
      break;
    case GSM_GPRS_CONN:
      evntFlags.ntpFault = SET;
      break;
    case GSM_NTP_INIT:
      break;
    case GSM_MQTT_START:
      if( SIM800.mqttServer.tcpconn == 0 ){
        evntFlags.tcpFault = SET;
      }
      else if( SIM800.mqttServer.mqttconn == 0 ){
        evntFlags.mqttFault = SET;
      }
      gsmStRestart = GSM_OFF;
      gsmRun = RESET;
      break;
    case GSM_MQTT_CONN:
      break;
    case GSM_SERV_CONN:
      break;
    case GSM_CFG_ON:
      break;
    case GSM_WORK:
      break;
    case GSM_OFF:
    case GSM_SIM_ON:
    default:
      break;

  }
}

/**
 * Send AT command to SIM800 over UART.
 * @param command the command to be used the send AT command
 * @param reply to be used to set the correct answer to the command
 * @param delay to be used to the set pause to the reply
 * @return error, 0 is OK
 */
int gsmSendCommand(char *command, char *reply, uint16_t delay, void (*simreplycb)( sUartRxHandle *) ){
  uint32_t tmptick;
  tmptick = mTick + delay;
  uint8_t rc = 1;

  simHnd.rxh->replyCb = simreplycb;

  simHnd.rxh->reply = reply;
  simHnd.rxh->replyFlag = RESET;
  trace_write( command, strlen(command) );
  if( uartSend(simHnd.txh, (uint8_t*)command, (uint16_t)strlen(command) ) == 0 ){
    trace_puts( "uart err" );
  }

  if( reply == NULL ){
    mDelay(delay);
    return 0;
  }

  while( tmptick >= mTick ) {
    if( simHnd.rxh->replyFlag ) {
      rc = 0;
      break;
    }
  }
  return rc;
}

// Ввод/смена PIN-кода
int simPinEnter( char * pin, char * newpin ){
  char * str;

  if( pin == NULL ){
    return -1;
  }

  if( (str = malloc( 20 )) == NULL ){
    return -1;
  }

//  trace_printf( "a_buf_%x\n", str );

  memcpy( str, "AT+CPIN=", 8 );
  memcpy(str+8, pin, 4);

  if( newpin != NULL ){
    // Добавляем новый PIN-код
    str[12] = ',';
    memcpy( str+13, newpin, 4 );
    memcpy( str+17, "\r\n\0", 3 );
  }
  else {
    memcpy( str+12, "\r\n\0", 3 );
  }

  if( gsmSendCommand( str, "OK\r\n", CMD_DELAY_5, NULL ) == 0 ){
    return 0;
  }

  return -1;
}


// Процесс готовности SIM-карты
int simReadyProcess( void ){
  switch( SIM800.sim.ready ){
    case SIM_NOT_READY:
      // Отправка команды CPIN
      simHnd.rxh->replyBuf = mqtt_buffer;
      *mqtt_buffer = '\0';
      if( gsmSendCommand("AT+CPIN?\r\n", "+CPIN:", CMD_DELAY_5, saveSimReply ) == 0 ){
        if( strstr( mqtt_buffer + 7, "READY" ) != NULL ){
          // Пин не нужен
          SIM800.sim.ready = SIM_PIN_READY;
        }
        else if( strstr( mqtt_buffer + 5, "SIM_PIN" ) != NULL ){
          char str[5];
          simPinEnter( utoa( SIM800.sim.pin, str, 10 ), NULL );
        }
      }
      else {
        return -1;
      }
      break;
    case SIM_PIN_READY:
      simHnd.rxh->replyBuf = mqtt_buffer;
      *mqtt_buffer = '\0';
      mDelay(1000);
      if( gsmSendCommand("AT+CREG?\r\n", "+CREG:", CMD_DELAY_10, saveSimReply ) == 0 ){
        if( mqtt_buffer[9] == '1' ){
          SIM800.sim.ready = SIM_GSM_READY;
        }
//        else if( mqtt_buffer[9] == '3' ){
//          gsmStRestart = GSM_OFF;
//          gsmRun = RESET;
//        }
        else {
          return -1;
        }
      }
      break;
    case SIM_GSM_READY:
    default:
      break;
  }

  return 0;
}


// Определяем урвень сигнала
int simImeiProcess( void ){
  static char * num = "0";
  simHnd.rxh->replyBuf = SIM800.sim.imei;
  *mqtt_buffer = '\0';
  if( num[0] <= '9' ){
    if( gsmSendCommand("AT+GSN\r\n", num, CMD_DELAY_5, saveImeiReply ) == 0){
      return 0;
    }
    else {
      return -1;
    }
  }
  else {
    ErrHandler( STOP );
  }

  return 0;
}


// Определяем урвень сигнала
int simCsqProcess( void ){
  simHnd.rxh->replyBuf = mqtt_buffer;
  *mqtt_buffer = '\0';
  if( gsmSendCommand("AT+CSQ\r\n", "+CSQ:", CMD_DELAY_5, saveSimReply ) == 0){
    SIM800.sim.csq = strtol( &(mqtt_buffer[6]), NULL, 10 );
    return 0;
  }
  else {
    return -1;
  }
}


int simWaitReady( void ){
  if( SIM800.sim.ready == SIM_GSM_READY ){
    clearRxBuffer( (char *)(simHnd.rxh->rxFrame), &(simHnd.rxh->frame_offset) );
    return RESET;
  }

  return SET;
}


// ----------------  GSM PROCCESS FUNCTIONS -----------------------------------
// Включение питания SIM800
void gsmOffFunc( void ){
  if( gsmRun ){
    switch( gsmRunPhase ){
      case PHASE_NON:
        if( timerPending( &sb2Timer ) ){
          return;
        }
        mqttCfgInit( &uspd.defCfgFlag );
        // On SIM800 power if use
        gpioPinResetNow( &gpioPinSimPwr );
        gpioPinSetNow( &gpioPinPwrKey );
        gsmRunPhase = PHASE_ON;
        // TODO: MCU засыпает на 2с, пока sim800 Сбрасывается.
        mDelay(2000);
        break;
      case PHASE_ON:
        gpioPinResetNow( &gpioPinPwrKey );
        gsmRunPhase = PHASE_ON_OK;
        // TODO: MCU засыпает на 10с, пока sim800 включается.
        mDelay(10000);
        break;
      case PHASE_ON_OK:
        // Две вспышки красного цвета с интервалом в 3 сек
        ledToggleSet( LED_R, LED_BLINK_ON_TOUT, LED_TOGGLE_TOUT, TOUT_3000, 2);
        gsmState++;
        gsmRunPhase = PHASE_NON;
        break;
      default:
        break;
    }
  }
  else {
    // Выключаем питание GSM
    switch( gsmRunPhase ){
      case PHASE_NON:
//        gpioPinSetNow( &gpioPinPwrKey );
//        gpioPinResetNow( &gpioPinSimPwr );
        gsmRunPhase = PHASE_OFF;
        break;
      case PHASE_OFF:
        // TODO: Выставляем будильник, если надо. Усыпляем контроллер
        if( mcuReset ){
          if( (logRdBufFill == 0) && (flashDev.state == FLASH_READY) ){
            // Все записи в журнал сделаны, операции с флеш закончены
            NVIC_SystemReset();
          }
        }
        else {
          gsmRunPhase = PHASE_OFF_OK;
          tmpTick = mTick + 10000;
        }
        break;
      case PHASE_OFF_OK:
        if( tmpTick < mTick ){
          gsmRun = SET;
          gsmRunPhase = PHASE_NON;
        }
        break;
      default:
        break;
    }
  }
}


// Состояние "GSM PWR ON": Установка сохраненной конфигурации
void gsmSimOnFunc( void ){
  if( gsmRun ){
    switch( gsmRunPhase ){
      case PHASE_NON:
//        mqttInit();
        ntpFlag = RESET;
        gsmState++;
        break;
      default:
        break;
    }
  }
  else {
    // Выключаем питание GSM
    gsmState--;
    gsmRunPhase = PHASE_NON;
  }
}


// Состояние "GSM PWR ON": Установка сохраненной конфигурации
void gsmInitFunc( void ){
  if( gsmRun ){
    switch( gsmRunPhase ){
      case PHASE_NON:
        simStartInit();
        timerModArg( &gsmOnToutTimer, (TOUT_1000 * 60), gsmState );
        tmpTick = mTick + (TOUT_1000 * 30);
        gsmRunPhase = PHASE_ON;
        tmpTick = 0;
        break;
      case PHASE_ON:
        if( tmpTick > mTick ){
          return;
        }

        if( SIM800.sim.ready != SIM_GSM_READY ){
          tmpTick = ( simReadyProcess() != 0 )? mTick + 500 : mTick + 1000;
        }
        else {
          if( simImeiProcess() != 0 ){
            tmpTick = mTick + 30;
          }
          else {
            gsmRunPhase = PHASE_ON_OK;
          }
        }
        break;
      case PHASE_ON_OK:
        if( simCsqProcess() != 0 ){
          tmpTick = mTick + 100;
        }
        else {
          timerDel( &gsmOnToutTimer );
          gsmRunPhase = PHASE_NON;
          gsmState++;
        }
        break;
      default:
        break;
    }
  }
  else {
    // Выключаем питание GSM
    if( gsmStRestart == GSM_INIT ){
      gsmRun = SET;
      gsmRunPhase = PHASE_NON;
    }
    else {
      gsmState--;
    }
  }
}


// Состояние "GSM PWR ON": Установка сохраненной конфигурации
void gsmStartInitFunc( void ){
  if( gsmRun ){
    switch( gsmRunPhase ){
      case PHASE_NON:
        switch( gprsConnTest() ){
          case '0':   // Bearer is connecting
          case '2':   // Bearer is closing
            break;
          case '1':   // Bearer is connect
            gsmRunPhase = PHASE_ON_OK;
            break;
          case '3':   // Bearer is closed
            gsmRunPhase = PHASE_ON;
            break;
          default:
//            ErrHandler( NON_STOP );
            mDelay(50);
            timerModArg( &gsmOnToutTimer, (TOUT_1000 * 30), gsmState );
            break;
        }
        break;
      case PHASE_ON:
        if( gprsConn() != RESET ){
          gprsConnBreak();
        }
        gsmRunPhase = PHASE_NON;
        break;
      case PHASE_ON_OK:
        // Есть соединение GPRS;
        timerDel( &gsmOnToutTimer );

        SIM800.mqttServer.tcpconn = 0;

        // Две вспышки оранжевого цвета с интервалом в 3 сек
        ledOff( LED_R, 0 );
        ledToggleSet( LED_R, LED_BLINK_ON_TOUT, TOUT_3000, 0, 0);
        ledToggleSet( LED_G, LED_BLINK_ON_TOUT, TOUT_3000, 0, 0);
        gsmRunPhase = PHASE_NON;
        gsmState++;
        break;
      default:
        break;
    }
  }
  else {
    // Выключаем питание GSM
    if( gsmStRestart == GSM_START_INIT ){
      // Пробуем переподключить
      if( gsmRunPhase == PHASE_NON) {
        tmpTick = 0;
        if( gprsConnBreak() == 0) {
          gsmRun = SET;
          gsmRunPhase = PHASE_NON;
        }
        else {
          tmpTick = mTick + TOUT_1000;
          gsmRunPhase = PHASE_OFF;
        }
      }
      else if (tmpTick < mTick ){
        gsmRunPhase = PHASE_NON;
        gsmState--;
      }
    }
    else {
      // Продолжаем выключать
      gprsConnBreak();
      gsmState--;
    }


  }
}


// Состояние "GSM PWR ON": Установка сохраненной конфигурации
void gsmGprsConnFunc( void ){
  if( gsmRun ){
    switch( gsmRunPhase ){
      case PHASE_NON:
        timerModArg( &gsmOnToutTimer, (TOUT_1000 * 30), gsmState );
        ntpInit();
        gsmRunPhase = PHASE_ON;
        break;
      case PHASE_ON:
        if( clkSet() == RESET ){
          // Время установлено - включаем интефейс Терминала и отправляем время
#if TERM_UART_ENABLE
          gpioPinSetNow( &gpioPinTermOn );
          termSendTime();
#endif //TERM_UART_ENABLE
          timerDel( &gsmOnToutTimer );

          gsmRunPhase = PHASE_NON;
          gsmState++;
        }
        break;
      default:
        break;
    }
  }
  else {
    // Выключаем питание GSM
    gsmState--;
  }
}


// Состояние "GSM PWR ON": Установка сохраненной конфигурации
void gsmNtpInitFunc( void ){
  if( gsmRun ){
    if( mqttStart() == RESET ){
      gsmRunPhase = PHASE_NON;
      gsmState++;
    }
  }
  else {
    // Продолжаем выключать
    if( gsmRunPhase == PHASE_NON ){
      mDelay(1010);
      gsmSendCommand("+++", "OK\r\n", CMD_DELAY_10 + 10, NULL );
      gsmRunPhase = PHASE_OFF;
    }
    else {
      if( MQTT_Deinit() == 0){
        gsmState--;
        gsmRunPhase = PHASE_NON;
      }
    }
  }
}


// Состояние "GSM MQTT_START": Установка сохраненной конфигурации
void gsmMqttStartFunc( void ){
  if( gsmRun ){
    switch( gsmRunPhase ){
      case PHASE_NON:
        if( SIM800.mqttServer.tcpconn == 0 ){
          timerModArg( &gsmOnToutTimer, (TOUT_1000 * 20), gsmState );
          TCP_Connect();
        }
        else if( SIM800.mqttServer.mqttconn == 0 ){
          timerModArg( &gsmOnToutTimer, (TOUT_1000 * 60), gsmState );
          MQTT_Connect();
          gsmRunPhase = PHASE_ON;
          tmpTick = mTick + 10000;
        }
        else {
          gsmRunPhase = PHASE_ON_OK;
        }
        break;
      case PHASE_ON:
        if( SIM800.mqttServer.mqttconn == 1 ){
          gsmRunPhase = PHASE_ON_OK;
        }
        else if(tmpTick < mTick ){
          // Не дождались соединения MQTT
          trace_puts( "mqtt_deinit" );
//          MQTT_Deinit();
          mDelay(TOUT_1000 * 15);
          gsmRunPhase = PHASE_NON;
        }
        break;
      case PHASE_ON_OK:
        timerDel( &gsmOnToutTimer );
        gsmState++;
        gsmRunPhase = PHASE_NON;
        break;
      default:
        break;
    }
  }
  else {
    // Выключаем питание GSM
    if( gsmStRestart == GSM_MQTT_START ){
      gsmRun = SET;
      gsmRunPhase = PHASE_NON;
    }
    else {
      gsmState--;
    }
  }
}


// Состояние "MQTT_CONN": Подписка SUBSCIPTION
void gsmMqttConnFunc( void ){
  if( gsmRun ){
    switch( gsmRunPhase ){
      case PHASE_NON:
        if( mqttSubProcess() ){
          break;
        }
        trace_puts( "mqtt sub" );
        SIM800.mqttClient.pubFlags.uspdAnnounce = SET;
        SIM800.mqttClient.pubReady = 0;
        gsmRunPhase = PHASE_ON;
        break;
      case PHASE_ON:
        mqttSubFlag = RESET;
        gsmState++;
        gsmRunPhase = PHASE_NON;
        break;
      default:
        break;
    }
  }
  else {
    // Выключаем питание GSM
    gsmState--;
  }
}


// Состояние "GSM PWR ON": Установка сохраненной конфигурации
void gsmServConnFunc( void ){
  if( gsmRun ){
    if( uspdCfg.updateFlag ){
      gsmState++;
      gsmRunPhase = PHASE_NON;
    }
  }
  else {
    // Выключаем питание GSM
    gsmState--;
  }
}


// Состояние "GSM PWR ON": Установка сохраненной конфигурации
void gsmCfgOnFunc( void ){
  if( gsmRun ){
    gsmState++;
    gsmRunPhase = PHASE_NON;
  }
  else {
    // Выключаем питание GSM
    gsmState--;
  }
}


// Состояние "GSM PWR ON": Установка сохраненной конфигурации
void gsmWorkFunc( void ){
  if( gsmRun ){
    gsmRunPhase = PHASE_NON;
    if( SIM800.mqttServer.tcpconn == 0 ){
      // Закрыто соединение с MQTT-сервером - откатываемся до стадии GSM_MQTT_START
      gsmRun = RESET;
    }
  }
  else {
    // Выключаем питание GSM
    gsmState--;
    gsmRunPhase = PHASE_NON;
  }
}


/**
 * initialization SNTP.
 * @param NONE
 * @return error status, 0 - OK
 */
int clkSet( void ) {
  uint8_t errCount;

  for( errCount = 0; errCount < 4; errCount++ ){
    simHnd.rxh->replyBuf = mqtt_buffer;
    *mqtt_buffer = '\0';
    if( gsmSendCommand("AT+CCLK?\r\n", "+CCLK: \"", CMD_DELAY_30, saveSimReply ) == 0){
      int8_t tz;
      // Получили дату-время
      sscanf(mqtt_buffer, "+CCLK: \"%d/%d/%d,%d:%d:%d%d\"", \
                        (int*)&rtc.year, (int*)&rtc.month, (int*)&rtc.date, \
                         (int*)&rtc.hour, (int*)&rtc.min, (int*)&rtc.sec, (int *)&tz );
//      tz /= 4;
      // Переходим в Локальное время
      setRtcTime( xTm2Utime( &rtc ) + tz * 3600 );
      return RESET;
    }
    else {
      if( errCount >= 2) {
        // Никак не получается включить GPRS
        gsmSendCommand("AT+CFUN=1,1\r\n", "OK\r\n", CMD_DELAY_50, NULL );
        mDelay(15000);
      }
    }
  }

  return SET;
}


/**
 * initialization SIM800.
 * @param NONE
 * @return error status, 0 - OK
 */
int simStartInit(void) {
    int error = SET;
    eBaudrate baud = BAUD_NUM;

//    HAL_UART_Receive_IT(UART_SIM800, &rx_data, 1);

//    simWaitReady( NON_STOP );
    for( uint8_t k = 0; k < 3; k++ ){
      for( eBaudrate i = BAUD_9600; baud == BAUD_NUM; ){
        for( uint8_t j = 0; j < 3; j++ ){
          if( gsmSendCommand("AT\r\n", "OK\r\n", CMD_DELAY_2, NULL ) == 0 ){
            // Есть контакт!
            baud = i;
            break;
          }
        }
        if( i != baud ){
          // Нет отклика от GSM
          mDelay(1000);
          // Увиличиваем скорость порта
//          i++;
//          if(i < BAUD_NUM){
//            simUartBaud( baudrate[i] );
//          }
//          else {
//            break;
//          }
        }
      }
      if( baud != BAUD_NUM ){
        break;
      }
      mDelay(10000);
    }

    if( baud == BAUD_NUM ){
      ErrHandler( STOP );
    }

    if( gsmSendCommand("AT+IFC=2,2\r\n", "OK\r\n", CMD_DELAY_2, NULL ) == 0){
      simUartHwFlow();
    }
//    if( baud != BAUD_115200 ){
//      if( gsmSendCommand("AT+IPR=115200\r\n", "OK\r\n", CMD_DELAY_2, NULL) == 0){
//        simUartBaud(115200);
//      }
//    }

    if( gsmSendCommand("AT\r\n", "OK\r\n", CMD_DELAY_2, NULL ) != 0 ){
      ErrHandler( STOP );
    }

    gsmSendCommand("ATE1\r\n", "OK\r\n", CMD_DELAY_2, NULL );

    return error;
}

/*
int getClk( void ){
  int8_t tz;
  int rc = 0;

  simHnd.rxh->replyBuf = mqtt_buffer;
  if( gsmSendCommand("AT+CCLK?\r\n", "+CCLK: \"", CMD_DELAY_30) == 0, saveSimReply ){
    // Получили дату-время
    sscanf(mqtt_buffer, "[^:]*: \"%u/%u/%u,%u:%u:%u%d\"", \
                       (unsigned int*)&rtc.date, (unsigned int*)&rtc.month, (unsigned int*)&rtc.year, \
                       (unsigned int*)&rtc.hour, (unsigned int*)&rtc.min, (unsigned int*)&rtc.sec, (int *)&tz );
    tz /= 4;
    // Переходим в Локальное время
    setRtcTime( xTm2Utime( &rtc ) + tz * 3600 );
    rc = 1;
  }

  return rc;
}
*/


int gprsConnTest( void ){
  int rc = -1;
  simHnd.rxh->replyBuf = mqtt_buffer;
  *mqtt_buffer = '\0';
    if( gsmSendCommand("AT+SAPBR=2,1\r\n", "+SAPBR:", CMD_DELAY_5, saveSimReply ) == 0){
      rc = mqtt_buffer[10];
    }
//    else {
//      ErrHandler( NON_STOP );
//    }

  return rc;
}


int gprsConnBreak( void ){
  if( SIM800.mqttServer.tcpconn ){
    return MQTT_Deinit();
  }
  else {
    return gsmSendCommand("ATE1\r\n", "OK\r\n", CMD_DELAY_2, NULL );
  }
}


int gprsConn( void ){
  char * str;

  gsmSendCommand("AT+SAPBR=3,1,\"Contype\",\"GPRS\"\r\n", "OK\r\n", CMD_DELAY_5, NULL );
//  mDelay( 2000 );
  if((str = malloc( 256 )) == NULL ){
    ErrHandler( NON_STOP );
  }
  else {
    sprintf(str, "AT+SAPBR=3,1,\"APN\",\"%s\"\r\n", SIM800.sim.apn);
//    trace_printf( "a_buf_%x\n", str );
    gsmSendCommand(str, "OK\r\n", CMD_DELAY_5, NULL);
  }
//  mDelay( 2000 );

  if( gsmSendCommand("AT+SAPBR=1,1\r\n", "OK\r\n", CMD_DELAY_50, NULL) == 0){
    // Есть соединение GPRS;
    // TODO: Получение IP
  }
  return 0;
}


int ntpInit(void) {
  if( ntpFlag == RESET ){

    if( gsmSendCommand("AT+CNTPCID=1\r\n", "OK\r\n", CMD_DELAY_5, NULL) ){
      return ntpFlag;
    }
    while( ntpFlag == RESET ){

      if( gsmSendCommand("AT+CNTP=\""NTP_SERVER"\",12\r\n", "OK\r\n", CMD_DELAY_5, NULL) == 0){;
        if( gsmSendCommand("AT+CNTP\r\n", "+CNTP: 1\r\n", CMD_DELAY_50 * 2, NULL) == 0 ){
          ntpFlag = SET;
        }
        else {
          ntpFlag = SET;
          ErrHandler( NON_STOP );
        }
      }
    }
  }

  return ntpFlag;
}



// ----------------------------------------------------------------------------
// Машина состояния break;
void gsmProcess( void ){
  switch( gsmState ){
    case GSM_OFF:
      gsmOffFunc();
      break;
    case GSM_SIM_ON:
      gsmSimOnFunc();
      break;
    case GSM_INIT:
      gsmInitFunc();
      break;
    case GSM_START_INIT:
      gsmStartInitFunc();
      break;
    case GSM_GPRS_CONN:
      gsmGprsConnFunc();
      break;
    case GSM_NTP_INIT:
      gsmNtpInitFunc();
      break;
    case GSM_MQTT_START:
      gsmMqttStartFunc();
      break;
    case GSM_MQTT_CONN:
      gsmMqttConnFunc();
      break;
    case GSM_SERV_CONN:
      gsmServConnFunc();
      break;
    case GSM_CFG_ON:
      gsmCfgOnFunc();
      break;
    case GSM_WORK:
      gsmWorkFunc();
      break;
    default:
      break;
  }
}


/**
 * Инициализация GSM-процесса.
 * @param NONE
 * @return error status, 0 - OK
 */
void gsmInit(void) {
  timerSetup( &gsmOnToutTimer, gsmOnTout, (uintptr_t)NULL );
}
