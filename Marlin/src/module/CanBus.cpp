#include "../inc/MarlinConfig.h"

#if ENABLED(CANBUS_SUPPORT)

#include "../../HAL/HAL_GD32F1/HAL_can_STM32F1.h"
#include "../Marlin.h"
#include "temperature.h"
#include "configuration_store.h"
#include "ExecuterManager.h"
#include "Periphdevice.h"
#include "CanBus.h"

CanBus CanBusControlor;

typedef int(CanModule::*FunPointer)(uint8_t*);
typedef struct
{
  FunPointer pFun;
  uint16_t FuncID;
}strFuncID2Fun;

typedef struct
{
  uint16_t MsgID;
  uint16_t FuncID;
  uint16_t Priority;
}strCanMsgMap;

const strCanMsgMap CanMsgTable[] = {
  {0, 0, 0},  //X Min
  {1, 0, 0},  //Y Min
  {2, 0, 0},  //Z Min
  {3, 0, 0},  //X Max
  {4, 0, 0},  //Y Max
  {5, 0, 0},  //Z Max
  {6, 0, 0},  //X2 Min
  {7, 0, 0},  //Y2 Min
  {8, 0, 0},  //Z2 Min
  {9, 0, 0},  //X2 Max
  {10, 0, 0}, //Y2 Max
  {11, 0, 0}, //Z2 Max
  {12, 0, 0}, //X3 Min
  {13, 0, 0}, //Y3 Min
  {14, 0, 0}, //Z3 Min
  {15, 0, 0}, //X3 Max
  {16, 0, 0}, //Y3 Max
  {17, 0, 0}, //Z3 Max
  {18, 6, 0}, //Probe Z
  {19, 7, 0}, //3Dprint HEATER
  {20, 8, 0}, //3Dprint FAN1
  {21, 9, 0}, //3Dprint FAN2
  {20, 6, 0}, //CNC
  {21, 6, 0}, //Laser
};

uint32_t CanBus::CurCommunicationID = 0xffffffff;
uint8_t CanBus::ReadRingBuff[2048] = {0};
uint8_t CanBus::ProcessBuff[524] = {0};
uint16_t CanBus::ReadHead = 0;
uint16_t CanBus::ReadTail = 0;

#define ProtocalParse(tmplen, srctail, tmptail, srcbuff, destbuff, commandlen) do { \
    commandlen = -1; \
    while(tmplen > 8) { \
      if(srcbuff[tmptail] != 0xAA) { \
        tmptail = (tmptail + 1) % sizeof(srcbuff); \
        tmplen--; \
        srctail = tmptail; \
        continue; \
      } \
      if(srcbuff[(tmptail + 1) % sizeof(srcbuff)] != 0x55) { \
        tmptail = (tmptail + 2) % sizeof(srcbuff); \
        tmplen = tmplen - 2; \
        srctail = tmptail; \
        continue; \
      } \
      uint8_t cmdLen0 = srcbuff[(tmptail + 2) % sizeof(srcbuff)]; \
      uint8_t cmdLen1 = srcbuff[(tmptail + 3) % sizeof(srcbuff)]; \
      uint16_t myindex; \
      uint32_t checksum; \
      commandlen = (uint16_t) ((cmdLen0 << 8) | cmdLen1); \
      if ((((commandlen >> 8) & 0xff) ^ (commandlen & 0xff)) != srcbuff[(tmptail + 5) % sizeof(srcbuff)]) { \
        tmptail = (tmptail + 2) % sizeof(srcbuff); \
        tmplen = tmplen - 2; \
        srctail = tmptail; \
        continue; \
      } \
      if (commandlen <= (tmplen - 8)) { \
        for (myindex = 0; myindex < (commandlen + 8); myindex++) { \
          destbuff[myindex] = srcbuff[tmptail]; \
          tmptail = (tmptail + 1) % sizeof(srcbuff); \
        } \
        srctail = tmptail; \
        checksum = 0; \
        for (myindex = 0; myindex < (commandlen - 1); myindex = myindex + 2) checksum += (destbuff[myindex + 8] << 8) | destbuff[myindex + 9]; \
        if (commandlen % 2) checksum += destbuff[commandlen + 8 - 1]; \
        while (checksum > 0xffff) checksum = ((checksum >> 16) & 0xffff) + (checksum & 0xffff); \
        checksum = ~checksum; \
        if ((uint16_t)checksum != (uint16_t) ((destbuff[6] << 8) | destbuff[7])) commandlen = -1; \
        else commandlen = commandlen; \
        break; \
      } \
    } \
  } \
	while(0)

/**
 * Init:Initialize the Can controlor
 */
void CanBus::Init() {
  CanBusControlor.ModuleCount = 0;
  CanInit();
}

/**
 * ProcessExtentPacks:Process the extid packs, treat it as Can-Usart
 * Para pBuff:The pointer to the receive buffer
 * para MaxLen:The Max length to receive
 * return: The real length have received
 */
uint16_t CanBus::ProcessLongPacks(uint8_t *pBuff, uint16_t MaxLen) {
  uint16_t tmptail;
  uint16_t tmphead;
  uint16_t tmplen;
  int16_t DataLen;
  uint16_t i;
  tmptail = ReadTail;
  tmphead = ReadHead;
  tmplen = (tmphead + sizeof(ReadRingBuff) - tmptail) % sizeof(ReadRingBuff);
  ProtocalParse(tmplen, ReadTail, tmptail, ReadRingBuff, ProcessBuff, DataLen);
  if(DataLen > 0) {
    DataLen = DataLen>MaxLen?MaxLen : DataLen;
    for(i=0;i<DataLen;i++)
      pBuff[i] = ProcessBuff[i + 8];
    return i;
    /*
    #define EVID_BYTE_INDEX  8
    #define OPCODE_BYTE_INDEX 9
    #define EventID ProcessBuff[EVID_BYTE_INDEX]
    #define OpCode  ProcessBuff[OPCODE_BYTE_INDEX]
    switch(EventID) {
      //Modules report Axis
      case 0x01:
      break;

      //Modules report support function index
      case 0x02:
      break;

      //Modules report Executer Number
      case 0x03:
      break;

      //Module General Reack
      case 0x99:
      break;
    }
    */
  }
  return 0;
}

/**
 * WaitReply:Wait reply from the specific ID
 * para CanNum:Can port number, 1 or 2
 * para ID:The specific ID
 * para pData:Datas to be send
 * para Len:How many datas to be send, max is 8
 * para timeout:How long to wait, in millisecond
 * return : true if reply has been received, or else false
 */
bool CanBus::WaitReply(uint8_t CanNum, uint32_t ID, uint8_t *pData, uint32_t Len, millis_t timeout) {
  millis_t tmptick;
  RequestReplied = false;
  RequestReplyCAN = CanNum;
  RequestReplyID = ID;
  CanSendPacked(ID, IDTYPE_STDID, CanNum, FRAME_DATA, Len, pData);
  tmptick = millis() + timeout;
  while(tmptick > millis()) {
    if(RequestReplied == true)
      return true;
  }
  return false;
}

/**
 * SendData:Send data frame to the specific ID
 * para CanNum:Can port number, 1 or 2
 * para ID:The specific ID
 * para pData:Datas to be send
 * para Len:How many datas to be send, max is 8
 * return : true if success, or else false
 */
bool CanBus::SendData(uint8_t CanNum, uint32_t ID, uint8_t *pData, int16_t Len) {
  return CanSendPacked(ID, IDTYPE_STDID, CanNum, FRAME_DATA, Len, pData);
}

/**
 * SendData:Send data frame to the specific ID
 * para CanNum:Can port number, 1 or 2
 * para ID:The specific ID
 * para pData:Datas to be send
 * para Len:How many datas to be send, max is 8
 * return : true if success, or else false
 */
bool CanBus::SendLongData(uint8_t CanNum, uint32_t ID, uint8_t *pData, int16_t Len) {
  uint32_t checksum;
  int i;
  int j;
  int sendlen;
  uint8_t packlen;
  bool Err;

  ID &= ~(1<<0);
  
  Err = false;
  i = 0;
  //Pack head
  ProcessBuff[i++] = 0xAA;
  ProcessBuff[i++] = 0x55;
  //Pack length
  ProcessBuff[i++] = 0x00;
  ProcessBuff[i++] = 0x00;
  //Version
  ProcessBuff[i++] = 0x00;
  //Length check
  ProcessBuff[i++] = 0x00;
  //Checksum
  ProcessBuff[i++] = 0x00;
  ProcessBuff[i++] = 0x00;
  while(Len--)
    ProcessBuff[i++] = *pData++;
  //重填包长
  ProcessBuff[2] = (uint8_t) ((i - 8) >> 8);
  ProcessBuff[3] = (uint8_t) (i - 8);
  ProcessBuff[5] = ProcessBuff[2] ^ProcessBuff[3];

  //校验
  checksum = 0;
  for (j = 8; j < (i - 1); j = j + 2) checksum += (uint32_t) (((uint8_t) ProcessBuff[j] << 8) | (uint8_t) ProcessBuff[j + 1]);
  if ((i - 8) % 2) checksum += ProcessBuff[i - 1];
  while (checksum > 0xffff) checksum = ((checksum >> 16) & 0xffff) + (checksum & 0xffff);
  checksum = ~checksum;
  ProcessBuff[6] = checksum >> 8;
  ProcessBuff[7] = checksum;

  sendlen = i;
  i = 0;
  while(sendlen > 0)
  {
    if(sendlen >= 8) packlen = 8;
    else packlen = sendlen;
    Err = CanSendPacked(ID, IDTYPE_EXTID, CanNum, FRAME_DATA, packlen, &ProcessBuff[i]);
    if(Err == false) return false;
    i = i + packlen;
    sendlen = sendlen - packlen;
  }
  return true;
}

/**
 * CheckReplay:Send data frame to the specific ID
 * para CanNum:Can port number, 1 or 2
 * para ID:The specific ID
 */
void CanBus::CheckReplay(uint8_t CanNum, uint32_t ID) {
  RequestReplyCAN = CanNum;
  if(ID == RequestReplyID)
    RequestReplied = true;
  RequestReplyID = 0xffffffff;
}

extern "C"
{
void __irq_can1_tx(void) {
}

void __irq_can1_rx0(void) {

}

void __irq_can1_rx1(void) {
}

void __irq_can1_sce(void) {
}

void __irq_can2_tx(void) {
}

void __irq_can2_rx0(void) {
  strCanData tmpData;
  uint8_t Buff[8];
  uint8_t Len;
  uint8_t FMI;
  FMI = Canbus2ParseData(&tmpData.ID, &tmpData.IDType, &tmpData.FrameType, tmpData.Data, &Len, 0);  
  if(FMI == 0) {
    tmpData.ID &= 0x1ff;
    //SERIAL_ECHOLN(tmpData.ID);
    if(tmpData.ID <= 18) { //For Axis endstop
      Buff[0] = (uint8_t)(tmpData.ID >> 8);
      Buff[1] = (uint8_t)(tmpData.ID);
      Buff[2] = tmpData.Data[0];
      CanModules.UpdateEndstops(Buff);
    } else {
      #if(0)
      for(int i=0;i<sizeof(CanFunctionTable) / sizeof(CanFunctionTable[0]);i++) {
        if(CanFunctionTable[i].FuncID == tmpData.ID) {
          CanFunctionTable[i].pFun(tmpData.Data);
          break;
        }
      }
      #endif
    }
  } else {
  }
}

void __irq_can2_rx1(void) {
  uint8_t Len;
  uint8_t FMI;
  strCanData tmpData;
  FMI = Canbus2ParseData(&tmpData.ID, &tmpData.IDType, &tmpData.FrameType, tmpData.Data, &Len, 1);
  if(FMI == 0) { //Brocast for enuming modules
    CanBusControlor.ModuleMacList[CanBusControlor.ModuleCount++] = tmpData.ID;
  } else if(FMI == 1) { //Nor communication datas
    if(Len > 0)
    {
      if(((CanBusControlor.ReadHead + sizeof(CanBusControlor.ReadRingBuff) + Len - CanBusControlor.ReadTail) % sizeof(CanBusControlor.ReadRingBuff)) < sizeof(CanBusControlor.ReadRingBuff)) {
        for(int i=0;i<Len;i++) {
          CanBusControlor.ReadRingBuff[CanBusControlor.ReadHead] = tmpData.Data[i];
          CanBusControlor.ReadHead = (CanBusControlor.ReadHead + 1) % sizeof(CanBusControlor.ReadRingBuff);
        }
      }
    }
  }
}

void __irq_can2_sce(void) {
}

}

#endif // ENABLED CANBUS_SUPPORT