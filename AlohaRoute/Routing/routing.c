#include <errno.h>     // errno
#include <pthread.h>   // pthread_create
#include <semaphore.h> // sem_init, sem_wait, sem_trywait, sem_timedwait
#include <stdbool.h>   // bool, true, false
#include <stdio.h>     // printf
#include <stdlib.h>    // rand, malloc, free, exit
#include <string.h>    // memcpy, strerror
#include <time.h>
#include "routing.h"
#include "../ALOHA/ALOHA.h"
#include "../GPIO/GPIO.h"
#include "../common.h"
#include "../util.h"
typedef struct RoutingMessage
{
    uint8_t src;
    uint8_t prev;
    uint16_t numHops;
    uint8_t dest;
    uint8_t next;
    uint8_t *data;
    uint16_t len;
    uint8_t ctrl;
} RoutingMessage;
#define RoutingQueueSize 64
typedef struct RoutingQueue
{
    unsigned int begin, end;
    struct RoutingMessage packet[RoutingQueueSize];
    sem_t mutex, full, free;
} RoutingQueue;
static RoutingQueue sendQ, recvQ;
static pthread_t recvT;
static pthread_t sendT;
static MAC mac;
static uint8_t debugFlag;
static const unsigned short maxTrials = 4;
static const unsigned short headerSize = 7;
static void sendQ_init();
static void recvQ_init();
static void sendQ_enqueue(RoutingMessage msg);
static RoutingMessage sendQ_dequeue();
static void recvQ_enqueue(RoutingMessage msg);
static RoutingMessage recvMsgQ_dequeue();
static void *recvPackets_func(void *args);
static RoutingMessage buildRoutingMessage(uint8_t *pkt);
static void *sendPackets_func(void *args);
static int buildRoutingPacket(RoutingMessage msg, uint8_t **routePkt);
static uint8_t getNextHopAddr(uint8_t self);
static MAC initMAC(uint8_t addr, unsigned short debug, unsigned int timeout);
static void setDebug(uint8_t d);
static void recvMsgQ_timed_dequeue(RoutingMessage *msg, struct timespec *ts);
// Initialize the routing layer
int routingInit(uint8_t self, uint8_t debug, unsigned int timeout)
{
    setDebug(debug);
    // mac = initMAC(self, debugFlag, timeout);
    // srand(self * time(NULL));
    MAC_init(&mac, self);
    mac.debug = debug;
    mac.recvTimeout = timeout;
    mac.noiseThreshold = -65;
    sendQ_init();
    recvQ_init();
    // if (self == ADDR_SINK)
    {
        if (pthread_create(&recvT, NULL, recvPackets_func, &mac) != 0)
        {
            printf("Failed to create Routing receive thread");
            exit(1);
        }
    }
    if (self != ADDR_SINK)
    {
        if (pthread_create(&sendT, NULL, sendPackets_func, &mac) != 0)
        {
            printf("Failed to create Routing send thread");
            exit(1);
        }
    }
    return 1;
}
// Send a message via the routing layer
int routingSend(uint8_t dest, uint8_t *data, unsigned int len)
{
    RoutingMessage msg;
    msg.ctrl = CTRL_PKT;
    msg.dest = dest;
    msg.src = mac.addr;
    msg.len = len;
    msg.next = getNextHopAddr(mac.addr);
    msg.numHops = 0;
    msg.data = (uint8_t *)malloc(len);
    if (msg.data != NULL)
    {
        memcpy(msg.data, data, len);
    }
    else
    {
        // Handle memory allocation failure
    }
    memcpy(msg.data, data, len);
    sendQ_enqueue(msg);
    // free(msg.data);
    // MAC_send(&mac, dest, data, len);
    return 1;
}
// Receive a message via the routing layer
int routingReceive(RouteHeader *header, uint8_t *data)
{
    RoutingMessage msg = recvMsgQ_dequeue();
    header->dst = msg.dest;
    header->RSSI = mac.RSSI;
    header->src = msg.src;
    header->numHops = msg.numHops;
    if (msg.data != NULL)
    {
        memcpy(data, msg.data, msg.len);
    }
    else
    {
    }
    return msg.len;
    // MAC_recv(&mac, data);
}
// Receive a message via the routing layer
int routingTimedReceive(RouteHeader *header, uint8_t *data, unsigned int timeout)
{
    RoutingMessage msg;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout;
    recvMsgQ_timed_dequeue(&msg, &ts);
    header->dst = msg.dest;
    header->RSSI = mac.RSSI;
    header->src = msg.src;
    header->numHops = msg.numHops;
    if (msg.data != NULL)
    {
        memcpy(data, msg.data, msg.len);
    }
    else
    {
        // Handle null pointer case
    }
    // *data = *msg.data;
    return msg.len;
}
static void recvMsgQ_timed_dequeue(RoutingMessage *msg, struct timespec *ts)
{
    if (sem_timedwait(&recvQ.full, ts) == -1)
    {
        return;
    }
    sem_wait(&recvQ.mutex);
    *msg = recvQ.packet[recvQ.begin];
    recvQ.begin = (recvQ.begin + 1) % RoutingQueueSize;
    sem_post(&recvQ.mutex);
    sem_post(&recvQ.full);
}
static void sendQ_init()
{
    sendQ.begin = 0;
    sendQ.end = 0;
    sem_init(&sendQ.full, 0, 0);
    sem_init(&sendQ.free, 0, RoutingQueueSize);
    sem_init(&sendQ.mutex, 0, 1);
}
static void recvQ_init()
{
    recvQ.begin = 0;
    recvQ.end = 0;
    sem_init(&recvQ.full, 0, 0);
    sem_init(&recvQ.free, 0, RoutingQueueSize);
    sem_init(&recvQ.mutex, 0, 1);
}
static void sendQ_enqueue(RoutingMessage msg)
{
    sem_wait(&sendQ.free);
    sem_wait(&sendQ.mutex);
    sendQ.packet[sendQ.end] = msg;
    sendQ.end = (sendQ.end + 1) % RoutingQueueSize;
    sem_post(&sendQ.mutex);
    sem_post(&sendQ.full);
}
static RoutingMessage sendQ_dequeue()
{
    sem_wait(&sendQ.full);
    sem_wait(&sendQ.mutex);
    RoutingMessage msg = sendQ.packet[sendQ.begin];
    sendQ.begin = (sendQ.begin + 1) % RoutingQueueSize;
    sem_post(&sendQ.mutex);
    sem_post(&sendQ.free);
    return msg;
}
static void recvQ_enqueue(RoutingMessage msg)
{
    sem_wait(&recvQ.free);
    sem_wait(&recvQ.mutex);
    recvQ.packet[recvQ.end] = msg;
    recvQ.end = (recvQ.end + 1) % RoutingQueueSize;
    sem_post(&recvQ.mutex);
    sem_post(&recvQ.full);
}
static RoutingMessage recvMsgQ_dequeue()
{
    sem_wait(&recvQ.full);
    sem_wait(&recvQ.mutex);
    RoutingMessage msg = recvQ.packet[recvQ.begin];
    recvQ.begin = (recvQ.begin + 1) % RoutingQueueSize;
    sem_post(&recvQ.mutex);
    sem_post(&recvQ.free);
    return msg;
}
static void *recvPackets_func(void *args)
{
    MAC *macTemp = (MAC *)args;
    while (1)
    {
        uint8_t *pkt = (uint8_t *)malloc(240);
        if (pkt == NULL)
        {
            free(pkt);
            continue;
        }
        // Receive a packet
        // int pktSize = MAC_recv(macTemp, pkt);
        int pktSize = MAC_timedrecv(macTemp, pkt, 3);
        if (pktSize == 0)
        {
            free(pkt);
            continue;
        }
        uint8_t ctrl = *pkt;
        if (ctrl == CTRL_PKT)
        {
            RoutingMessage msg = buildRoutingMessage(pkt);
            if (msg.dest == ADDR_BROADCAST || msg.dest == macTemp->addr)
            {
                // Keep
                recvQ_enqueue(msg);
            }
            else
            {
                msg.next = getNextHopAddr(macTemp->addr);
                printf("%s - ## FWD: (%02d) %02X -> %02X msg: %s\n", timestamp(), msg.numHops, msg.src, msg.next, msg.data);
                // Forward
                if (MAC_send(macTemp, msg.next, pkt, pktSize))
                {
                    if (debugFlag)
                    {
                        printf("%s - ## FWD: (%02d) %02X -> %02X msg: %s\n", timestamp(), msg.numHops, msg.src, msg.next, msg.data);
                    }
                }
                else
                {
                    printf("%s - ## Error FWD: (%02d) %02X -> %02X msg: %s\n", timestamp(), msg.numHops, msg.src, msg.next, msg.data);
                }
                if (msg.data != NULL)
                {
                    free(msg.data);
                }
            }
        }
        else
        {
            if (debugFlag)
            {
                printf("%s - ## Routing : Unknown control flag %02X \n", timestamp(), ctrl);
            }
        }
        free(pkt);
    }
}

// Construct RoutingMessage
static RoutingMessage buildRoutingMessage(uint8_t *pkt)
{
    RoutingMessage msg;
    msg.ctrl = *pkt;
    pkt += sizeof(msg.ctrl);

    msg.dest = *pkt;
    pkt += sizeof(msg.dest);

    msg.src = *pkt;
    pkt += sizeof(msg.src);

    msg.prev = mac.recvH.src_addr;

    uint16_t numHops;
    memcpy(&numHops, pkt, sizeof(msg.numHops));
    msg.numHops = ++numHops;
    memcpy(pkt, &numHops, sizeof(msg.numHops));
    pkt += sizeof(msg.numHops);

    memcpy(&msg.len, pkt, sizeof(msg.len));
    pkt += sizeof(msg.len);

    if (msg.len > 0)
    {
        msg.data = (uint8_t *)malloc(msg.len);
    }
    else
    {
        msg.data = NULL;
    }
    memcpy(msg.data, pkt, msg.len);
    return msg;
}
static void *sendPackets_func(void *args)
{
    MAC *macTemp = (MAC *)args;
    while (1)
    {
        RoutingMessage msg = sendQ_dequeue();
        uint8_t *pkt;
        unsigned int pktSize = buildRoutingPacket(msg, &pkt);
        if (pkt == NULL)
        {
            free(pkt);
            continue; // Or handle error appropriately
        }
        // for (unsigned int i = 0; i < pktSize; i++)
        // {
        //     printf("%02X ", pkt[i]);
        // }
        // printf("\n");
        // send
        if (MAC_send(macTemp, msg.next, pkt, pktSize))
        {
        }
        else
        {
        }
        // MAC_Isend(macTemp, msg.next, pkt, pktSize);
        // free(pkt);
        // if (msg.data != NULL)
        // {
        //     free(msg.data);
        // }
    }
}
static int buildRoutingPacket(RoutingMessage msg, uint8_t **routePkt)
{
    uint16_t routePktSize = msg.len + headerSize;
    *routePkt = (uint8_t *)malloc(routePktSize);
    uint8_t *p = *routePkt;
    *p = msg.ctrl;
    p += sizeof(msg.ctrl);
    // Set dest
    *p = msg.dest;
    p += sizeof(msg.dest);
    // Set source as self
    *p = mac.addr;
    p += sizeof(mac.addr);
    // Set numHops
    *p = msg.numHops;
    p += sizeof(msg.numHops);
    // Set actual msg length
    *p = msg.len;
    p += sizeof(msg.len);
    // Set msg
    memcpy(p, msg.data, msg.len);
    // p += msg.len;
    // free(routePkt);
    free(msg.data);
    // for (uint16_t i = 0; i < routePktSize; i++)
    // {
    //     printf("%02X ", (*routePkt)[i]);
    // }
    // printf("\n");
    return routePktSize;
}
static uint8_t getNextHopAddr(uint8_t self)
{
    uint8_t addr;
    unsigned short trial = 0;
    do
    {
        addr = NODE_POOL[rand() % POOL_SIZE];
        trial++;
    } while ((addr >= self || (addr - self) > 1) && trial <= maxTrials);
    return (trial > maxTrials) ? ADDR_SINK : addr;
}
static MAC initMAC(uint8_t addr, unsigned short debug, unsigned int timeout)
{
    GPIO_init();
    MAC macTemp;
    MAC_init(&macTemp, addr);
    macTemp.debug = debug;
    macTemp.recvTimeout = timeout;
    // macTemp.noiseThreshold = -65;
    return macTemp;
}
static void setDebug(uint8_t d)
{
    debugFlag = d;
}
