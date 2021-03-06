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

#include "os.h"
#include "tlog.h"
#include "taoserror.h"
#include "tqueue.h"

typedef struct _taos_qnode {
  int                 type;
  struct _taos_qnode *next;
  char                item[];
} STaosQnode;

typedef struct _taos_q {
  int32_t             itemSize;
  int32_t             numOfItems;
  struct _taos_qnode *head;
  struct _taos_qnode *tail;
  struct _taos_q     *next;    // for queue set
  struct _taos_qset  *qset;    // for queue set
  void               *ahandle; // for queue set
  pthread_mutex_t     mutex;  
} STaosQueue;

typedef struct _taos_qset {
  STaosQueue        *head;
  STaosQueue        *current;
  pthread_mutex_t    mutex;
  int32_t            numOfQueues;
  int32_t            numOfItems;
} STaosQset;

typedef struct _taos_qall {
  STaosQnode   *current;
  STaosQnode   *start;
  int32_t       itemSize;
  int32_t       numOfItems;
} STaosQall; 
  
taos_queue taosOpenQueue() {
  
  STaosQueue *queue = (STaosQueue *) calloc(sizeof(STaosQueue), 1);
  if (queue == NULL) {
    terrno = TSDB_CODE_NO_RESOURCE;
    return NULL;
  }

  pthread_mutex_init(&queue->mutex, NULL);
  return queue;
}

void taosCloseQueue(taos_queue param) {
  STaosQueue *queue = (STaosQueue *)param;
  STaosQnode *pTemp;
  STaosQnode *pNode = queue->head;  
  queue->head = NULL;

  if (queue->qset) taosRemoveFromQset(queue->qset, queue); 

  pthread_mutex_lock(&queue->mutex);

  while (pNode) {
    pTemp = pNode;
    pNode = pNode->next;
    free (pTemp);
  }

  pthread_mutex_unlock(&queue->mutex);

  free(queue);
}

void *taosAllocateQitem(int size) {
  STaosQnode *pNode = (STaosQnode *)calloc(sizeof(STaosQnode) + size, 1);
  if (pNode == NULL) return NULL;
  return (void *)pNode->item;
}

void taosFreeQitem(void *param) {
  if (param == NULL) return;

  //pTrace("item:%p is freed", param);

  char *temp = (char *)param;
  temp -= sizeof(STaosQnode);
  free(temp);
}

int taosWriteQitem(taos_queue param, int type, void *item) {
  STaosQueue *queue = (STaosQueue *)param;
  STaosQnode *pNode = (STaosQnode *)(((char *)item) - sizeof(STaosQnode));
  pNode->type = type;

  pthread_mutex_lock(&queue->mutex);

  if (queue->tail) {
    queue->tail->next = pNode;
    queue->tail = pNode;
  } else {
    queue->head = pNode;
    queue->tail = pNode; 
  }

  queue->numOfItems++;
  if (queue->qset) atomic_add_fetch_32(&queue->qset->numOfItems, 1);

  //pTrace("item:%p is put into queue, type:%d items:%d", item, type, queue->numOfItems);

  pthread_mutex_unlock(&queue->mutex);

  return 0;
}

int taosReadQitem(taos_queue param, int *type, void **pitem) {
  STaosQueue *queue = (STaosQueue *)param;
  STaosQnode *pNode = NULL;
  int         code = 0;

  pthread_mutex_lock(&queue->mutex);

  if (queue->head) {
      pNode = queue->head;
      *pitem = pNode->item;
      *type = pNode->type;
      queue->head = pNode->next;
      if (queue->head == NULL) 
        queue->tail = NULL;
      queue->numOfItems--;
      if (queue->qset) atomic_sub_fetch_32(&queue->qset->numOfItems, 1);
      code = 1;
      //pTrace("item:%p is read out from queue, items:%d", *pitem, queue->numOfItems);
  } 

  pthread_mutex_unlock(&queue->mutex);

  return code;
}

void *taosAllocateQall() {
  void *p = malloc(sizeof(STaosQall));
  return p;
}

void taosFreeQall(void *param) {
  free(param);
}

int taosReadAllQitems(taos_queue param, taos_qall p2) {
  STaosQueue *queue = (STaosQueue *)param;
  STaosQall  *qall = (STaosQall *)p2;
  int         code = 0;

  pthread_mutex_lock(&queue->mutex);

  if (queue->head) {
    memset(qall, 0, sizeof(STaosQall));
    qall->current = queue->head;
    qall->start = queue->head;
    qall->numOfItems = queue->numOfItems;
    qall->itemSize = queue->itemSize;
    code = qall->numOfItems;

    queue->head = NULL;
    queue->tail = NULL;
    queue->numOfItems = 0;
    if (queue->qset) atomic_sub_fetch_32(&queue->qset->numOfItems, qall->numOfItems);
  } 

  pthread_mutex_unlock(&queue->mutex);
  
  return code; 
}

int taosGetQitem(taos_qall param, int *type, void **pitem) {
  STaosQall  *qall = (STaosQall *)param;
  STaosQnode *pNode;
  int         num = 0;

  pNode = qall->current;
  if (pNode)
    qall->current = pNode->next;
 
  if (pNode) {
    *pitem = pNode->item;
    *type = pNode->type;
    num = 1;
    // pTrace("item:%p is fetched, type:%d", *pitem, *type);
  }

  return num;
}

void taosResetQitems(taos_qall param) {
  STaosQall  *qall = (STaosQall *)param;
  qall->current = qall->start;
}

taos_qset taosOpenQset() {

  STaosQset *qset = (STaosQset *) calloc(sizeof(STaosQset), 1);
  if (qset == NULL) {
    terrno = TSDB_CODE_NO_RESOURCE;
    return NULL;
  }

  pthread_mutex_init(&qset->mutex, NULL);

  return qset;
}

void taosCloseQset(taos_qset param) {
  STaosQset *qset = (STaosQset *)param;
  free(qset);
}

int taosAddIntoQset(taos_qset p1, taos_queue p2, void *ahandle) {
  STaosQueue *queue = (STaosQueue *)p2;
  STaosQset  *qset = (STaosQset *)p1;

  if (queue->qset) return -1; 

  pthread_mutex_lock(&qset->mutex);

  queue->next = qset->head;
  queue->ahandle = ahandle;
  qset->head = queue;
  qset->numOfQueues++;

  pthread_mutex_lock(&queue->mutex);
  atomic_add_fetch_32(&qset->numOfItems, queue->numOfItems);
  queue->qset = qset;
  pthread_mutex_unlock(&queue->mutex);

  pthread_mutex_unlock(&qset->mutex);

  return 0;
}

void taosRemoveFromQset(taos_qset p1, taos_queue p2) {
  STaosQueue *queue = (STaosQueue *)p2;
  STaosQset  *qset = (STaosQset *)p1;
 
  STaosQueue *tqueue = NULL;

  pthread_mutex_lock(&qset->mutex);

  if (qset->head) {
    if (qset->head == queue) {
      qset->head = qset->head->next;
      tqueue = queue;
    } else {
      STaosQueue *prev = qset->head;
      tqueue = qset->head->next;
      while (tqueue) {
        if (tqueue== queue) {
          prev->next = tqueue->next;
          break;
        } else {
          prev = tqueue;
          tqueue = tqueue->next;
        }
      }
    }

    if (tqueue) {
      if (qset->current == queue) qset->current = tqueue->next;
      qset->numOfQueues--;

      pthread_mutex_lock(&queue->mutex);
      atomic_sub_fetch_32(&qset->numOfItems, queue->numOfItems);
      queue->qset = NULL;
      pthread_mutex_unlock(&queue->mutex);
    }
  } 
  
  pthread_mutex_unlock(&qset->mutex);
}

int taosGetQueueNumber(taos_qset param) {
  return ((STaosQset *)param)->numOfQueues;
}

int taosReadQitemFromQset(taos_qset param, int *type, void **pitem, void **phandle) {
  STaosQset  *qset = (STaosQset *)param;
  STaosQnode *pNode = NULL;
  int         code = 0;

  for(int i=0; i<qset->numOfQueues; ++i) {
    pthread_mutex_lock(&qset->mutex);
    if (qset->current == NULL) 
      qset->current = qset->head;   
    STaosQueue *queue = qset->current;
    if (queue) qset->current = queue->next;
    pthread_mutex_unlock(&qset->mutex);
    if (queue == NULL) break;

    pthread_mutex_lock(&queue->mutex);

    if (queue->head) {
        pNode = queue->head;
        *pitem = pNode->item;
        *type = pNode->type;
        *phandle = queue->ahandle;
        queue->head = pNode->next;
        if (queue->head == NULL) 
          queue->tail = NULL;
        queue->numOfItems--;
        atomic_sub_fetch_32(&qset->numOfItems, 1);
        code = 1;
    } 

    pthread_mutex_unlock(&queue->mutex);
    if (pNode) break;
  }

  return code; 
}

int taosReadAllQitemsFromQset(taos_qset param, taos_qall p2, void **phandle) {
  STaosQset  *qset = (STaosQset *)param;
  STaosQueue *queue;
  STaosQall  *qall = (STaosQall *)p2;
  int         code = 0;

  for(int i=0; i<qset->numOfQueues; ++i) {
    pthread_mutex_lock(&qset->mutex);
    if (qset->current == NULL) 
      qset->current = qset->head;   
    queue = qset->current;
    if (queue) qset->current = queue->next;
    pthread_mutex_unlock(&qset->mutex);
    if (queue == NULL) break;

    pthread_mutex_lock(&queue->mutex);

    if (queue->head) {
      qall->current = queue->head;
      qall->start = queue->head;
      qall->numOfItems = queue->numOfItems;
      qall->itemSize = queue->itemSize;
      code = qall->numOfItems;
      *phandle = queue->ahandle;
          
      queue->head = NULL;
      queue->tail = NULL;
      queue->numOfItems = 0;
      atomic_sub_fetch_32(&qset->numOfItems, qall->numOfItems);
    } 

    pthread_mutex_unlock(&queue->mutex);

    if (code != 0) break;  
  }

  return code;
}

int taosGetQueueItemsNumber(taos_queue param) {
  STaosQueue *queue = (STaosQueue *)param;
  return queue->numOfItems;
}

int taosGetQsetItemsNumber(taos_qset param) {
  STaosQset *qset = (STaosQset *)param;
  return qset->numOfItems;
}
