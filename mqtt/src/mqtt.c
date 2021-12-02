/*
 * mqtt.c
 *
 *  Created on: 21 окт. 2021 г.
 *      Author: jet
 */
#include <string.h>

#include "gpio_arch.h"
#include "uart.h"
#include "MQTTSim800.h"
#include "adc.h"
#include "fw.h"
#include "gsm.h"
#include "isens.h"
#include "logger.h"
#include "uspd.h"
#include "mqtt.h"

extern sFwHandle fwHandle;

extern struct timer_list mqttPubTimer;
extern struct timer_list mqttSubTimer;
extern uint16_t logRdBufFill;
extern const sUartHnd simHnd;

const char * tpcTempl[TOPIC_NUM] = {
  "r/device",     		      //  TOPIC_DEVID,
  "d/%d",                 //  TOPIC_DEV_IMEI,
  "%s/info",         		  //  TOPIC_INFO,
  "%s/temp",       		    //  TOPIC_TEMP,
  "%s/volt",       		    //  TOPIC_VOLT,
  "%s/cmdi",           		//  TOPIC_CMD_I,      *
  "%s/cmdo",           		//  TOPIC_CMD_O,
  "%s/cfi",       	    	//  TOPIC_CFG_I,      *
  "%s/cfo",       		    //  TOPIC_CFG_O,
  "%s/alrm",       		    //  TOPIC_ALRM,
  "%s/log",       		    //  TOPIC_LOG,
  "%s/i/%d",       		    //  TOPIC_ISENS,
  "%s/i/%d/arx",      		  //  TOPIC_ISENS_ARX,
  "%s/i/%d/state",     		//  TOPIC_ISENS_STATE,
  "%s/i/%d/adc",       		//  TOPIC_ISENS_ADC,
  "%s/i/%d/temp",     		  //  TOPIC_ISENS_TEMP,
  "%s/o/%d",       		    //  TOPIC_OUT,        *
  "%s/o/%d/state",       	//  TOPIC_OUT_STATE,
  "%s/fw",       		      //  TOPIC_FW,
  "%s/fw/man",       		  //  TOPIC_FW_MAN,     *
  "%s/fw/bin",       		  //  TOPIC_FW_BIN,     *
  "%s/rs",       		      //  TOPIC_RS,
  "%s/rs/tx",       		  //  TOPIC_RS_TX,      *
  "%s/rs/rx",       		  //  TOPIC_RS_RX,      *
  "%s/gsm",       		    //  TOPIC_GSM,
  //  TOPIC_NUM
};

SIM800_t SIM800;

// Список для Подписки
const struct {
  char * subtpc;
  eTopicId tpid;
  uint8_t qos;
} subList[] = {
  { "%s/cmdi", TOPIC_CMD_I, 2 },
  { "%s/cfi", TOPIC_CFG_I, 2 },
  { "%s/o/1", TOPIC_OUT, 1 },
  { "%s/fw/man", TOPIC_FW_MAN, 2 },
  { "%s/fw/bin", TOPIC_FW_BIN, 2 },
  { "%s/rs/tx", TOPIC_RS_TX, 1 },
  { "%s/rs/rx", TOPIC_RS_RX, 1 },
};

void mqttConnectCb( FlagStatus conn );


void mqttPubTout( uintptr_t arg ){
  (void)arg;

  if( SIM800.mqttServer.mqttconn == 1 ) {
    mqttPubFlag = SET;
  }
}


void mqttSubTout( uintptr_t arg ){
  (void)arg;

  if( SIM800.mqttServer.mqttconn == 1 ) {
    mqttSubFlag = SET;
  }
  timerMod( &mqttSubTimer, MQTT_SUB_TOUT );
}


int mqttSubProcess(void){
  uint8_t sc = SIM800.mqttClient.subCount;

  if( mqttSubFlag ){
    char topicstr[23];
    if( sc == ARRAY_SIZE(subList) ){
      timerDel( &mqttSubTimer );
      return 0;
    }
    sprintf(topicstr, subList[sc].subtpc, SIM800.sim.imei);
    // Подписываемся, пока не получим подтверждение на ВСЕ подписки
    MQTT_Sub( topicstr, subList[sc].qos );
  }

  mqttSubFlag = RESET;
  return 1;
}

// Получение  флагов сообщения
void msgFlagSet( uint8_t flags, mqttReceive_t * receive ){
  receive->dup = flags & 0x08;
  receive->qos = (flags >> 1) & 0x06;
  receive->retained = flags & 0x01;
}


// Обработка принятого контрольного пакета (НЕ PUBLISH)
void mqttCtlProc( SIM800_t * sim ){
  uint16_t pktid = sim->mqttReceive.pktId;

  switch( sim->mqttReceive.msgType ) {
    case MQTT_CONNACK:
      trace_printf( "CONACK: %d\n", pktid );
      sim->mqttServer.mqttconn = SET;
      sim->mqttClient.subCount = 0;
      mqttConnectCb( SIM800.mqttServer.mqttconn );
      break;
    case MQTT_SUBACK:
      // TODO: Сбросить таймер сообветствующей подписки
      sim->mqttClient.subCount++;
      timerMod( &mqttSubTimer, 0 );
      trace_printf( "SUBACK: %d\n", pktid );
      break;
    case MQTT_PUBACK:
      // Отклик на отправку пакета с QOS1
      trace_printf( "PUBACK: %d\n", pktid );
      // Отключить таймер для пакета с этим PKT_ID
      break;
    case MQTT_PUBREC:
      // Отклик на получение пакета с QOS2 (фаза1 QOS2)
      trace_printf( "PUBREC: %d\n", pktid );
      MQTT_Pubrel( pktid );
      break;
    case MQTT_PUBREL:
      // Отклик на отправку пакета PUBREC (фаза2 QOS2)
      trace_printf( "PUBREL: %d\n", pktid );
      MQTT_Pubcomp( pktid );
      if( fwHandle.fwUp.fwUpOk ){
        // TODO: Запуск выключения и перезагрузки
        mDelay(50);
        gsmFinal = SET;
        mcuReset = SET;
        gsmRun = RESET;
      }
      break;
    case MQTT_PUBCOMP:
      trace_printf( "PUBCOMP: %d\n", pktid );
      // Отключить таймер для пакета с этим PKT_ID (QOS 2)
      break;
    case MQTT_PINGRESP:
      trace_puts( "PINGRESP" );
      break;
    case MQTT_DISCONNECT:
      sim->mqttServer.mqttconn = RESET;
      mqttConnectCb( SIM800.mqttServer.mqttconn );
      break;
    default:
      return;
  }
}


void mqttMsgProc( sUartRxHandle * handle, SIM800_t * sim ){
  uint8_t * msgptr;
  uint16_t len0;
  uint32_t len;

  msgptr = sim->mqttReceive.mqttData;
  len  = ((handle->rxFrame + handle->frame_offset) - msgptr);
  len0 = len;

  while ( len0 ){
    switch ( sim->mqttReceive.msgState ){
      case MSG_NULL:
        msgptr = handle->rxFrame;

        sim->mqttReceive.msgType = (*msgptr & 0xF0) >> 4;
        msgFlagSet( *msgptr, &(sim->mqttReceive) );

        switch( sim->mqttReceive.msgType ) {
          case MQTT_PINGRESP:
          case MQTT_PUBLISH:
          case MQTT_CONNACK:
          case MQTT_PUBACK:
          case MQTT_PUBREC:
          case MQTT_PUBREL:
          case MQTT_PUBCOMP:
          case MQTT_SUBACK:
            break;
          default:
            // Топик не наш - Не сохраняем payload
            goto msg_reset;
        }
        sim->mqttReceive.msgState = MSG_TYPE;
        sim->mqttReceive.remLenMp = 1;
        sim->mqttReceive.remLen = 0;
        len0--;
        msgptr++;
        break;
      case MSG_TYPE: {
        // Длина пакета (2097152 = 128*128*128)
        for( ; len0 ; len0-- ){
          // Получаем длину данных
          sim->mqttReceive.remLen += (*msgptr % 128) * sim->mqttReceive.remLenMp;
          if( (*msgptr++ & 0x80) == 0 ){
            // Принята вся Remaining Lenght
            if( (sim->mqttReceive.remLen > 268435455)
                || (sim->mqttReceive.remLen == 0) ){
              // Ошибка длины или Нулевая длина
              trace_printf( "REMLEN = 0, PKT_ID: %X\n", sim->mqttReceive.msgType );
              goto msg_reset;
            }
            else {
              sim->mqttReceive.msgState = MSG_REMAINING_LEN;
            }
            len0--;
            break;
          }
          sim->mqttReceive.remLenMp *= 128;
          if( sim->mqttReceive.remLenMp > 2097152){
            goto msg_reset;
          }
        }

        break;
      }
      case MSG_REMAINING_LEN:
        if( sim->mqttReceive.remLen <= 3 ) {
          if( len0 == sim->mqttReceive.remLen ){
            // Принят контрольный пакет
            mqttCtlProc( sim );
            goto msg_reset;
          }
          len0 = 0;
        }
        else {
          if( len0 >= 2 ){
            // Принята длина топика
            assert_param( msgptr != NULL );
            sim->mqttReceive.topicLen = *msgptr << 8;
            msgptr++;
            sim->mqttReceive.topicLen |= *msgptr;
            msgptr++;
            len0 -= 2;
            sim->mqttReceive.msgState = MSG_TOP_LEN;
          }
          else {
            len0 = 0;
          }
        }
        break;
      case MSG_TOP_LEN: {
        uint8_t size = sim->mqttReceive.topicLen;

        if( len >= size ){
          // Приняn топик
          // (15бит<IMEI> + '\0' - 1 + 1бит'/'
          uint8_t offset = sizeof(IMEI) - 1 + 1;
          eTopicId tp;
          uint8_t * tpc = msgptr + offset;
          uint16_t tpsize = size - offset;

          // Сохраняем только значимую часть топика, без "<IMEI>/"
          for( tp = 0; tp < ARRAY_SIZE(subList); tp++ ){
            if( *tpc == 'o' ){
              // Топик "o" - проверяем без продолжения
              tpsize = 1;
            }
            if( memcmp( tpc, (subList[tp].subtpc+3), tpsize ) == 0 ){
              memcpy(sim->mqttReceive.topic, tpc, tpsize );
              break;
            }
          }

          sim->mqttReceive.topicId = subList[tp].tpid;
          len0 -= size;
          msgptr += size;
          if( sim->mqttReceive.qos ){
            sim->mqttReceive.msgState = MSG_TOPIC;
          }
          else {
            sim->mqttReceive.msgState = MSG_PKT_ID;
          }
        }
        else {
          len0 = 0;
        }
        break;
      }
      case MSG_TOPIC:
        if( len0 >= 2 ){
          // Принят PACKET_ID
          assert_param( msgptr != NULL );
          sim->mqttReceive.pktId = *msgptr << 8;
          msgptr++;
          sim->mqttReceive.pktId |= *msgptr;
          msgptr++;
          len0 -= 2;
          sim->mqttReceive.msgState = MSG_PKT_ID;
        }
        else {
          len0 = 0;
        }
        break;
      case MSG_PKT_ID:
        if( sim->mqttReceive.qos ){
          // Учтем размер полей topicLen + pktId
          sim->mqttReceive.payloadLen = sim->mqttReceive.remLen - (2 + 2) - sim->mqttReceive.topicLen;
        }
        else {
          // Учтем размер поля topicLen
          sim->mqttReceive.payloadLen = sim->mqttReceive.remLen - 2 - sim->mqttReceive.topicLen;
        }

        if( sim->mqttReceive.payloadLen == 0 ){
          goto msg_reset;
        }
        else {
          sim->mqttReceive.msgState = MSG_PAY_LEN;
        }
        break;
      case MSG_PAY_LEN:
        if( len >= sim->mqttReceive.payloadLen ){
          // Payload получен в полном объеме
          if( sim->mqttReceive.topicId == TOPIC_NUM ){
            // Топик не наш - Не сохраняем payload
            goto msg_reset;
          }
          else {
            ledOn( LED_G, 100 );
            // Получено сообщение целиком
            // TODO: Обработка полученых данных - сделать не в ПРЕРЫВАНИИ
            handle->rxProcFlag = SET;
            sim->mqttReceive.payOffset = msgptr - handle->rxFrame;
            if( sim->mqttReceive.qos == 1 ){
              // TODO: Send PUBACK packet (QOS 1)
              MQTT_Puback( sim->mqttReceive.pktId );
            }
            else if( sim->mqttReceive.qos == 2 ) {
              trace_printf( "PUB rec: %d\n", sim->mqttReceive.pktId );
              MQTT_Pubrec( sim->mqttReceive.pktId );
            }
          }
          len0 = 0;
          sim->mqttReceive.msgState = MSG_NULL;
        }
        else if( len == 1024 ){
          if( sim->mqttReceive.topicId == TOPIC_FW_BIN ){
            // Прием прошивки - запись во флеш
            // TODO: Запись во Флеш сделать не в ПРЕРЫВАНИИ
            sim->mqttReceive.payOffset = msgptr - handle->rxFrame;
            handle->rxProcFlag = SET;
            len0 = 0;
          }
          else {
            goto msg_reset;
          }
        }
        else {
          assert_param( len <= 1024);
          len0 = 0;
        }

        break;
      case MSG_PAYLOAD:
        break;
      default:
        break;
    }
  }

  sim->mqttReceive.mqttData = msgptr;
  return;

msg_reset:
  mqttMsgReset( handle, &SIM800 );
  return;
}


void mqttPubProc( sUartRxHandle * handle ){
  if( handle->rxProcFlag == RESET ){
    return;
  }

  // Надо обработать полученное PUBLISH
  switch( SIM800.mqttReceive.topicId ){
    case TOPIC_DEVID:
      mqttMsgReset( handle, &SIM800 );
      break;
    case TOPIC_DEV_IMEI:
			break;
    case TOPIC_INFO:
			break;
    case TOPIC_TEMP:
      mqttMsgReset( handle, &SIM800 );
      break;
    case TOPIC_VOLT:
      mqttMsgReset( handle, &SIM800 );
			break;
    case TOPIC_CMD_I:
			break;
    case TOPIC_CMD_O:
			break;
    case TOPIC_CFG_I:
      uspdCfgProc( handle, &SIM800 );
			break;
    case TOPIC_CFG_O:
			break;
    case TOPIC_ALRM:
			break;
    case TOPIC_LOG:
			break;
    case TOPIC_ISENS:
			break;
    case TOPIC_ISENS_ARX:
			break;
    case TOPIC_ISENS_STATE:
			break;
    case TOPIC_ISENS_ADC:
			break;
    case TOPIC_ISENS_TEMP:
			break;
    case TOPIC_OUT:
			break;
    case TOPIC_OUT_STATE:
			break;
    case TOPIC_FW:
			break;
    case TOPIC_FW_MAN:
      fwManProc(  handle, &(SIM800.mqttReceive) );
			break;
    case TOPIC_FW_BIN:
      // Приняли/принимаем Обновление прошивки
      fwUpProc( handle, &(SIM800.mqttReceive) );
			break;
    case TOPIC_RS:
			break;
    case TOPIC_RS_TX:
			break;
    case TOPIC_RS_RX:
			break;
    case TOPIC_GSM:
			break;
    default:
      //Сюда не должны попасть
      break;
  }

}


/**
 * initialization SIM800.
 * @param NONE
 * @return error status, 0 - OK
 */
void mqttInit(void) {
  SIM800.mqttServer.tcpconn = RESET;
    SIM800.mqttServer.mqttconn = RESET;
//    char str[32] = {0};

    // MQQT settings
    SIM800.sim.ready = SIM_NOT_READY;
    SIM800.sim.pin = uspdCfg.simcfg[0].pin = UID_0 % 10000;
    SIM800.sim.apn = uspdCfg.simcfg[0].gprsApn;
    SIM800.sim.apn_user = uspdCfg.simcfg[0].gprsUser;
    SIM800.sim.apn_pass = uspdCfg.simcfg[0].gprsPass;
    SIM800.mqttServer.host = uspdCfg.mqttHost;
    SIM800.mqttServer.port = &(uspdCfg.mqttPort);
    SIM800.mqttReceive.mqttData = simHnd.rxh->rxFrame;
    SIM800.mqttClient.username = uspdCfg.mqttUser;
    SIM800.mqttClient.pass = uspdCfg.mqttPass;
    SIM800.mqttClient.clientID = "";
    SIM800.mqttClient.keepAliveInterval = 60;

    timerSetup( &mqttPubTimer, mqttPubTout, (uintptr_t)NULL );
    timerSetup( &mqttSubTimer, mqttSubTout, (uintptr_t)NULL );
}


/**
 * Starting MQTT process.
 * @param NONE
 * @return error status, 0 - OK
 */
int mqttStart(void) {
  gsmSendCommand("ATE0\r\n", "OK\r\n", CMD_DELAY_2, NULL );
  return gsmSendCommand("AT+CIPMODE=1\r\n", "OK\r\n", CMD_DELAY_2, NULL );
}


void mqttProcess( void ){
  sPubFlags * pubfl;
  char str[64];
  char * cfgimsg;

  if(SIM800.mqttServer.mqttconn == RESET){
    return;
  }

  pubfl = &(SIM800.mqttClient.pubFlags);
  if( mqttPubFlag ) {
    mqttPubFlag = RESET;
    if( pubfl->cfgoPub ){
      sprintf( str, tpcTempl[TOPIC_CFG_I], SIM800.sim.imei );
      cfgimsg = cfgiMsgCreate();
      if( MQTT_Pub( str, cfgimsg )){
        cfgUpdateFinal = SET;
        Error_Handler( NON_STOP );
      }
      // Передали на отправку в UART. Удачно-нет - освобождаем;
      ta_free( cfgimsg );
    }
    if( iSens[ISENS_1].isensFlag ){
      uint32_t ut = getRtcTime();

      sprintf( str, "{\"state\":[{time\":%ul,\"pls\":%ul}]}", (unsigned int)ut, (unsigned int)iSens[ISENS_1].isensCount );
      MQTT_Pub( "imei/i/1", str );
      iSens[ISENS_1].isensFlag = RESET;
    }
    else {
      // Считываем из ЛОГа
      if( logRdBufFill == 0 ){
        logQueryProcess();
      }
      if( logRdBufFill ){
        for( uint8_t i = 0; i < logRdBufFill; i++ ){
          char str[64];
          sLogRec * logrec = &(logRdBuf[i]);

          sprintf( str, "{\"arx\":[{time\":%ul,\"pls\":%ul}]}", (unsigned int)logrec->utime, (unsigned int)logrec->data );
          MQTT_Pub( "imei/i/1", str );
          logRdBufFill = 0;
        }
      }
      else {
        MQTT_PingReq();
//        char str[64];
//        int tu, td;
//        uint32_t ut = getRtcTime();
//
//        tu = adcHandle.adcVbat / 1000;
//        td = adcHandle.adcVbat - (tu * 1000);
//        if( td < 0 ){
//          td = -td;
//        }
//        sprintf( str, "{time\":%u,\"volt\":%d.%d}", (unsigned int)ut, tu, td );
//        MQTT_Pub( "imei/volt", str );
//
//        tu = adcHandle.adcTemp / 10;
//        td = adcHandle.adcTemp - (tu * 10);
//        if( td < 0 ){
//          td = -td;
//        }
//        sprintf( str, "{time\":%u,\"temp\":%d.%d}", (unsigned int)ut, tu, td );
//        MQTT_Pub( "imei/temp", str );
      }
    }
    timerMod( &mqttPubTimer, MQTT_PUB_TOUT );
  }

  // Обработка принятых сообщений
  mqttPubProc( simHnd.rxh );

}
