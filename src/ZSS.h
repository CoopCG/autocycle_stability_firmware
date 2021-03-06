//
// Created by Misha on 5/5/2021.
//

#ifndef AUTOCYCLE_STABILITY_FIRMWARE_ZSS_H
#define AUTOCYCLE_STABILITY_FIRMWARE_ZSS_H

#include <Arduino.h>


class ZSS {
public:
    ZSS(uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4, uint8_t l1, uint8_t l2);

    void start();
    void deploy();
    void retract();
    void halt();
    bool retracted();

private:
    uint8_t p1, p2, p3, p4;
    uint8_t l1, l2;
    bool down;
};


#endif //AUTOCYCLE_STABILITY_FIRMWARE_ZSS_H
