#ifndef _PACKET_H_
#define _PACKET_H_

typedef struct wu2packet {
    int num;
    int arrive_time;
    int Q1_start;
    int Q2_start;

    int inter_arrival_time;
    int service_time;
    int request;

} packet;

#endif /*_PACKET_H_*/
