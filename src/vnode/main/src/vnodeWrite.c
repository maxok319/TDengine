/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taosmsg.h"
#include "taoserror.h"
#include "tlog.h"
#include "tqueue.h"
#include "trpc.h"
#include "tsdb.h"
#include "twal.h"
#include "dataformat.h"
#include "vnode.h"
#include "vnodeInt.h"

static int32_t (*vnodeProcessWriteMsgFp[TSDB_MSG_TYPE_MAX])(SVnodeObj *, void *, SRspRet *);
static int32_t  vnodeProcessSubmitMsg(SVnodeObj *pVnode, void *pMsg, SRspRet *);
static int32_t  vnodeProcessCreateTableMsg(SVnodeObj *pVnode, void *pMsg, SRspRet *);
static int32_t  vnodeProcessDropTableMsg(SVnodeObj *pVnode, void *pMsg, SRspRet *);
static int32_t  vnodeProcessAlterTableMsg(SVnodeObj *pVnode, void *pMsg, SRspRet *);
static int32_t  vnodeProcessDropStableMsg(SVnodeObj *pVnode, void *pMsg, SRspRet *);

void vnodeInitWriteFp(void) {
  vnodeProcessWriteMsgFp[TSDB_MSG_TYPE_SUBMIT]          = vnodeProcessSubmitMsg;
  vnodeProcessWriteMsgFp[TSDB_MSG_TYPE_MD_CREATE_TABLE] = vnodeProcessCreateTableMsg;
  vnodeProcessWriteMsgFp[TSDB_MSG_TYPE_MD_DROP_TABLE]   = vnodeProcessDropTableMsg;
  vnodeProcessWriteMsgFp[TSDB_MSG_TYPE_MD_ALTER_TABLE]  = vnodeProcessAlterTableMsg;
  vnodeProcessWriteMsgFp[TSDB_MSG_TYPE_MD_DROP_STABLE]  = vnodeProcessDropStableMsg;
}

int32_t vnodeProcessWrite(void *param, int qtype, SWalHead *pHead, void *item) {
  int32_t    code = 0;
  SVnodeObj *pVnode = (SVnodeObj *)param;

  if (vnodeProcessWriteMsgFp[pHead->msgType] == NULL) 
    return TSDB_CODE_MSG_NOT_PROCESSED; 

  if (pVnode->status == VN_STATUS_DELETING || pVnode->status == VN_STATUS_CLOSING) 
    return TSDB_CODE_NOT_ACTIVE_VNODE; 

  if (pHead->version == 0) { // from client 
    if (pVnode->status != VN_STATUS_READY) 
      return TSDB_CODE_NOT_ACTIVE_VNODE;

    // if (pVnode->replica > 1 && pVnode->role != TAOS_SYNC_ROLE_MASTER)

    // assign version
    pVnode->version++;
    pHead->version = pVnode->version;
  } else {  
    // for data from WAL or forward, version may be smaller
    if (pHead->version <= pVnode->version) return 0;
  }
   
  // more status and role checking here

  pVnode->version = pHead->version;

  // write into WAL
  code = walWrite(pVnode->wal, pHead);
  if ( code < 0) return code;
 
  code = (*vnodeProcessWriteMsgFp[pHead->msgType])(pVnode, pHead->cont, item);
  if (code < 0) return code;

/* forward
  if (pVnode->replica > 1 && pVnode->role == TAOS_SYNC_ROLE_MASTER) {
    code = syncForwardToPeer(pVnode->sync, pHead, item);
  }
*/

  return code;
}

static int32_t vnodeProcessSubmitMsg(SVnodeObj *pVnode, void *pCont, SRspRet *pRet) {
  int32_t code = 0;

  // save insert result into item

  dTrace("pVnode:%p vgId:%d, submit msg is processed", pVnode, pVnode->vgId);
  code = tsdbInsertData(pVnode->tsdb, pCont);

  pRet->len = sizeof(SShellSubmitRspMsg);
  pRet->rsp = rpcMallocCont(pRet->len);
  SShellSubmitRspMsg *pRsp = pRet->rsp;

  pRsp->code              = 0;
  pRsp->numOfRows         = htonl(1);
  pRsp->affectedRows      = htonl(1);
  pRsp->numOfFailedBlocks = 0;

  return code;
}

static int32_t vnodeProcessCreateTableMsg(SVnodeObj *pVnode, void *pCont, SRspRet *pRet) {
  SMDCreateTableMsg *pTable = pCont;
  int32_t code = 0;

  dTrace("pVnode:%p vgId:%d, table:%s, start to create", pVnode, pVnode->vgId, pTable->tableId);
  pTable->numOfColumns  = htons(pTable->numOfColumns);
  pTable->numOfTags     = htons(pTable->numOfTags);
  pTable->sid           = htonl(pTable->sid);
  pTable->sversion      = htonl(pTable->sversion);
  pTable->tagDataLen    = htonl(pTable->tagDataLen);
  pTable->sqlDataLen    = htonl(pTable->sqlDataLen);
  pTable->uid           = htobe64(pTable->uid);
  pTable->superTableUid = htobe64(pTable->superTableUid);
  pTable->createdTime   = htobe64(pTable->createdTime);
  SSchema *pSchema = (SSchema *) pTable->data;

  int totalCols = pTable->numOfColumns + pTable->numOfTags;
  for (int i = 0; i < totalCols; i++) {
    pSchema[i].colId = htons(pSchema[i].colId);
    pSchema[i].bytes = htons(pSchema[i].bytes);
  }

  STableCfg tCfg;
  tsdbInitTableCfg(&tCfg, pTable->tableType, pTable->uid, pTable->sid);

  STSchema *pDestSchema = tdNewSchema(pTable->numOfColumns);
  for (int i = 0; i < pTable->numOfColumns; i++) {
    tdSchemaAppendCol(pDestSchema, pSchema[i].type, pSchema[i].colId, pSchema[i].bytes);
  }
  tsdbTableSetSchema(&tCfg, pDestSchema, false);

  if (pTable->numOfTags != 0) {
    STSchema *pDestTagSchema = tdNewSchema(pTable->numOfTags);
    for (int i = pTable->numOfColumns; i < totalCols; i++) {
      tdSchemaAppendCol(pDestTagSchema, pSchema[i].type, pSchema[i].colId, pSchema[i].bytes);
    }
    tsdbTableSetTagSchema(&tCfg, pDestTagSchema, false);

    char *pTagData = pTable->data + totalCols * sizeof(SSchema);
    int accumBytes = 0;
    SDataRow dataRow = tdNewDataRowFromSchema(pDestTagSchema);

    for (int i = 0; i < pTable->numOfTags; i++) {
      tdAppendColVal(dataRow, pTagData + accumBytes, pDestTagSchema->columns + i);
      accumBytes += pSchema[i + pTable->numOfColumns].bytes;
    }
    tsdbTableSetTagValue(&tCfg, dataRow, false);
  }

  void *pTsdb = vnodeGetTsdb(pVnode);
  code = tsdbCreateTable(pTsdb, &tCfg);

  dTrace("pVnode:%p vgId:%d, table:%s is created, result:%x", pVnode, pVnode->vgId, pTable->tableId, code);
  return code; 
}

static int32_t vnodeProcessDropTableMsg(SVnodeObj *pVnode, void *pCont, SRspRet *pRet) {
  SMDDropTableMsg *pTable = pCont;
  int32_t code = 0;

  dTrace("pVnode:%p vgId:%d, table:%s, start to drop", pVnode, pVnode->vgId, pTable->tableId);
  STableId tableId = {
    .uid = htobe64(pTable->uid),
    .tid = htonl(pTable->sid)
  };

  code = tsdbDropTable(pVnode->tsdb, tableId);

  return code;
}

static int32_t vnodeProcessAlterTableMsg(SVnodeObj *pVnode, void *pCont, SRspRet *pRet) {
  SMDCreateTableMsg *pTable = pCont;
  int32_t code = 0;

  dTrace("pVnode:%p vgId:%d, table:%s, start to alter", pVnode, pVnode->vgId, pTable->tableId);
  pTable->numOfColumns  = htons(pTable->numOfColumns);
  pTable->numOfTags     = htons(pTable->numOfTags);
  pTable->sid           = htonl(pTable->sid);
  pTable->sversion      = htonl(pTable->sversion);
  pTable->tagDataLen    = htonl(pTable->tagDataLen);
  pTable->sqlDataLen    = htonl(pTable->sqlDataLen);
  pTable->uid           = htobe64(pTable->uid);
  pTable->superTableUid = htobe64(pTable->superTableUid);
  pTable->createdTime   = htobe64(pTable->createdTime);
  SSchema *pSchema = (SSchema *) pTable->data;

  int totalCols = pTable->numOfColumns + pTable->numOfTags;
  for (int i = 0; i < totalCols; i++) {
    pSchema[i].colId = htons(pSchema[i].colId);
    pSchema[i].bytes = htons(pSchema[i].bytes);
  }

  STableCfg tCfg;
  tsdbInitTableCfg(&tCfg, pTable->tableType, pTable->uid, pTable->sid);

  STSchema *pDestSchema = tdNewSchema(pTable->numOfColumns);
  for (int i = 0; i < pTable->numOfColumns; i++) {
    tdSchemaAppendCol(pDestSchema, pSchema[i].type, pSchema[i].colId, pSchema[i].bytes);
  }
  tsdbTableSetSchema(&tCfg, pDestSchema, false);

  if (pTable->numOfTags != 0) {
    STSchema *pDestTagSchema = tdNewSchema(pTable->numOfTags);
    for (int i = pTable->numOfColumns; i < totalCols; i++) {
      tdSchemaAppendCol(pDestTagSchema, pSchema[i].type, pSchema[i].colId, pSchema[i].bytes);
    }
    tsdbTableSetSchema(&tCfg, pDestTagSchema, false);

    char *pTagData = pTable->data + totalCols * sizeof(SSchema);
    int accumBytes = 0;
    SDataRow dataRow = tdNewDataRowFromSchema(pDestTagSchema);

    for (int i = 0; i < pTable->numOfTags; i++) {
      tdAppendColVal(dataRow, pTagData + accumBytes, pDestTagSchema->columns + i);
      accumBytes += pSchema[i + pTable->numOfColumns].bytes;
    }
    tsdbTableSetTagValue(&tCfg, dataRow, false);
  }

  code = tsdbAlterTable(pVnode->tsdb, &tCfg);
  dTrace("pVnode:%p vgId:%d, table:%s, alter table result:%d", pVnode, pVnode->vgId, pTable->tableId, code);

  return code;
}

static int32_t vnodeProcessDropStableMsg(SVnodeObj *pVnode, void *pCont, SRspRet *pRet) {
  SMDDropSTableMsg *pTable = pCont;
  int32_t code = 0;

  dTrace("pVnode:%p vgId:%d, stable:%s, start to drop", pVnode, pVnode->vgId, pTable->tableId);
  pTable->uid = htobe64(pTable->uid);

  // TODO: drop stable in vvnode
  //void *pTsdb = dnodeGetVnodeTsdb(pMsg->pVnode);
  //rpcRsp.code = tsdbDropSTable(pTsdb, pTable->uid);

  code = TSDB_CODE_SUCCESS;
  dTrace("pVnode:%p vgId:%d, stable:%s, drop stable result:%x", pVnode, pTable->tableId, code);
 
  return code;
}

int vnodeWriteToQueue(void *param, SWalHead *pHead, int type) {
  SVnodeObj *pVnode = param;

  int size = sizeof(SWalHead) + pHead->len;
  SWalHead *pWal = (SWalHead *)taosAllocateQitem(size);
  memcpy(pWal, pHead, size);

  taosWriteQitem(pVnode->wqueue, type, pHead);

  return 0;
}

