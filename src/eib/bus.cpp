/*
 *  bus.cpp - Low level EIB bus access.
 *
 *  Copyright (c) 2014 Stefan Taferner <stefan.taferner@gmx.at>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3 as
 *  published by the Free Software Foundation.
 */

#include <sblib/eib/bus.h>

#include <sblib/core.h>
#include <sblib/interrupt.h>
#include <sblib/platform.h>
#include <sblib/eib/addr_tables.h>
#include <sblib/eib/user_memory.h>


// The EIB bus access object
Bus bus(timer16_1, PIO1_8, PIO1_9, CAP0, MAT0);

// The interrupt handler for the EIB bus access object
BUS_TIMER_INTERRUPT_HANDLER(TIMER16_1_IRQHandler, bus);


/*
 * The timer16_1 is used as follows:
 *
 * Capture register CR0 is used for receiving
 * Match register MR0 or MR1 is used as PWM for sending, depending on which output pin is used
 * Match register MR3 is used for timeouts while sending / receiving
 *
 */

// Enable debug statements for debugging the bus access in this file
//#define D(x) x
#define D(x)


// Telegram repeat flag in byte #0 of the telegram: 1=not repeated, 0=repeated
#define SB_TEL_REPEAT_FLAG 0x20

static int debugLine = 0;


static LPC_GPIO_TypeDef (* const LPC_GPIO[4]) = { LPC_GPIO0, LPC_GPIO1, LPC_GPIO2, LPC_GPIO3 };


// Mask for the timer flag of the capture channel
#define CAPTURE_FLAG (8 << captureChannel)

// Mask for the timer flag of the time channel
#define TIME_FLAG (8 << timeChannel)

// Default time between two bits (104 usec)
#define BIT_TIME 104

// Time between two bits (69 usec)
#define BIT_WAIT_TIME 69

// Pulse duration of a bit (35 usec)
#define BIT_PULSE_TIME 35

// Maximum time from start bit to stop bit, including a safety extra: BIT_TIME*10 + BIT_TIME/2
#define BYTE_TIME 1090

// Time to wait before sending an ACK: approximately BIT_TIME * 11 + (BIT_TIME / 4)
#define SEND_ACK_WAIT_TIME 1177

// Time to wait before starting to send: BIT_TIME * 50
#define SEND_WAIT_TIME 5200

// Time to listen for bus activity before sending starts: BIT_TIME * 1
#define PRE_SEND_TIME 104

// The value for the prescaler
#define TIMER_PRESCALER (SystemCoreClock / 1000000 - 1)


Bus::Bus(Timer& aTimer, int aRxPin, int aTxPin, TimerCapture aCaptureChannel, TimerMatch aPwmChannel)
:timer(aTimer)
,rxPin(aRxPin)
,txPin(aTxPin)
,captureChannel(aCaptureChannel)
,pwmChannel(aPwmChannel)
{
    timeChannel = (TimerMatch) ((pwmChannel + 2) & 3);  // +2 to be compatible to old code during refactoring
    state = Bus::IDLE;
}

void Bus::begin()
{
    ownAddr = (userEeprom.addrTab[0] << 8) | userEeprom.addrTab[1];

    telegramLen = 0;

    state = Bus::IDLE;
    sendAck = 0;
    sendCurTelegram = 0;
    sendNextTel = 0;
    collision = false;

    timer.begin();


    pinMode(rxPin, INPUT_CAPTURE);  // Configure bus input
    pinMode(txPin, OUTPUT_MATCH);   // Configure bus output
    digitalWrite(txPin, 0);

    timer.pwmEnable(pwmChannel);
    timer.captureMode(captureChannel, FALLING_EDGE | INTERRUPT);
    timer.start();
    timer.interrupts();
    timer.prescaler(TIMER_PRESCALER);

    timer.match(timeChannel, 0xfffe);
    timer.matchMode(timeChannel, RESET);
    timer.match(pwmChannel, 0xffff);

    //
    // Init GPIOs for debugging
    //
    D(pinMode(PIO3_0, OUTPUT));
    D(pinMode(PIO3_1, OUTPUT));
    D(pinMode(PIO3_2, OUTPUT));
    D(pinMode(PIO3_3, OUTPUT));
    D(pinMode(PIO0_6, OUTPUT));
    D(pinMode(PIO0_7, OUTPUT));
    D(pinMode(PIO2_8, OUTPUT));
    D(pinMode(PIO2_9, OUTPUT));
    D(pinMode(PIO2_10, OUTPUT));

    D(digitalWrite(PIO3_0, 0));
    D(digitalWrite(PIO3_1, 0));
    D(digitalWrite(PIO3_2, 0));
    D(digitalWrite(PIO3_3, 0));
    D(digitalWrite(PIO0_6, 0));
    D(digitalWrite(PIO0_7, 0));
    D(digitalWrite(PIO2_8, 0));
    D(digitalWrite(PIO2_9, 0));
    D(digitalWrite(PIO2_10, 0));
}

void Bus::idleState()
{
    timer.captureMode(captureChannel, FALLING_EDGE | INTERRUPT);

    timer.matchMode(timeChannel, RESET);
    timer.match(timeChannel, 0xfffe);
    timer.match(pwmChannel, 0xffff);

    state = Bus::IDLE;
    sendAck = 0;

//    digitalWrite(txPin, 0); // Set bus-out pin to 0
//    pinMode(txPin, INPUT);
}

void Bus::handleTelegram(bool valid)
{
//    D(digitalWrite(PIO3_3, 1));         // purple: end of telegram
    sendAck = 0;

    if (collision) // A collision occurred. Ignore the received bytes
    {
    }
    else if (nextByteIndex >= 8 && valid) // Received a valid telegram with correct checksum
    {
        int destAddr = (telegram[3] << 8) | telegram[4];
        bool processTel = false;

        // We ACK the telegram only if it's for us
        if (telegram[5] & 0x80)
        {
            if (destAddr == 0 || indexOfAddr(destAddr) >= 0)
                processTel = true;
        }
        else if (destAddr == ownAddr)
        {
            processTel = true;
        }

        // Only process the telegram if it is for us or if we want to get all telegrams
        if (!(userRam.status & BCU_STATUS_TL))
        {
            telegramLen = nextByteIndex;
        }
        else if (processTel)
        {
            telegramLen = nextByteIndex;
            sendAck = SB_BUS_ACK;
        }
    }
    else if (nextByteIndex == 1)   // Received a spike or a bus acknowledgment
    {
        currentByte &= 0xff;

        if ((currentByte == SB_BUS_ACK || sendTries > 3) && sendCurTelegram)
            sendNextTelegram();
    }
    else // Received wrong checksum, or more than one byte but too short for a telegram
    {
        telegramLen = 0;
        sendAck = SB_BUS_NACK;
    }

    // Wait before sending. In SEND_INIT we will cancel if there is nothing to be sent.
    // We need to wait anyways to avoid triggering sending from the application code when
    // the bus is in cooldown. This could happen if we set state to Bus::IDLE here.
    timer.match(timeChannel, sendAck ? SEND_ACK_WAIT_TIME - PRE_SEND_TIME : SEND_WAIT_TIME - PRE_SEND_TIME);
    timer.matchMode(timeChannel, INTERRUPT | RESET);

    timer.captureMode(captureChannel, FALLING_EDGE | INTERRUPT);

    collision = false;
    state = Bus::SEND_INIT;
    debugLine = __LINE__;
}

void Bus::sendNextTelegram()
{
    sendCurTelegram[0] = 0;
    sendCurTelegram = sendNextTel;
    sendNextTel = 0;
    sendTries = 0;
    sendTelegramLen = 0;
}

void Bus::timerInterruptHandler()
{
    D(static unsigned short tick = 0);
    bool timeout;
    int time;

    // Debug output
    D(digitalWrite(PIO0_6, ++tick & 1));  // brown: interrupt tick
    D(digitalWrite(PIO3_0, state==Bus::SEND_BIT_0)); // red
    D(digitalWrite(PIO3_1, 0));           // orange
    D(digitalWrite(PIO3_2, 0));           // yellow
    D(digitalWrite(PIO3_3, 0));           // purple
    D(digitalWrite(PIO2_8, 0));           // blue
//    D(digitalWrite(PIO2_9, 0));           //

STATE_SWITCH:
    switch (state)
    {
    // The bus is idle. Usually we come here when there is a capture event on bus-in.
    case Bus::IDLE:
        if (!timer.flag(captureChannel)) // Not a bus-in signal: do nothing
            break;
        nextByteIndex = 0;
        collision = false;
        checksum = 0xff;
        sendAck = 0;
        valid = 1;
        // no break here

    // A start bit is expected to arrive here. If we have a timeout instead, the
    // transmission is over.
    case Bus::RECV_START:
        //D(digitalWrite(PIO3_1, 1));   // orange
        if (!timer.flag(captureChannel))  // No start bit: then it is a timeout
        {
            handleTelegram(valid && !checksum);
            break;
        }

        timer.match(timeChannel, BYTE_TIME);
        timer.restart();
        timer.matchMode(timeChannel, INTERRUPT | RESET);

        state = Bus::RECV_BYTE;
        currentByte = 0;
        bitTime = 0;
        bitMask = 1;
        parity = 1;
        break;

    case Bus::RECV_BYTE:
        timeout = timer.flag(timeChannel);

        if (timeout) time = BYTE_TIME;
        else time = timer.capture(captureChannel);

        if (time >= bitTime + BIT_WAIT_TIME)
        {
            bitTime += BIT_TIME;
            while (time >= bitTime + BIT_WAIT_TIME && bitMask <= 0x100)
            {
                currentByte |= bitMask;
                parity = !parity;

                bitTime += BIT_TIME;
                bitMask <<= 1;
            }

            bitMask <<= 1;
        }

        if (timeout)  // Timer timeout: end of byte
        {
            D(digitalWrite(PIO3_2, 1));     // yellow: end of byte
            D(digitalWrite(PIO3_1, parity));// orange: parity bit ok

            valid &= parity;
            if (nextByteIndex < SB_TELEGRAM_SIZE)
            {
                telegram[nextByteIndex++] = currentByte;
                checksum ^= currentByte;
            }

            state = Bus::RECV_START;   // wait for the next byte's start bit
            timer.match(timeChannel, BIT_TIME * 4);
        }
        break;

    // SEND_INIT is entered some usec before sending the start bit of the first byte. It
    // is always entered after receiving or sending is done, even if nothing is to be sent.
    case Bus::SEND_INIT:
        D(digitalWrite(PIO3_2, 1)); // yellow: prepare transmission

        if (timer.flag(captureChannel))  // Bus input, enter receive mode
        {
            state = Bus::IDLE;
            goto STATE_SWITCH;
        }

        if (sendAck)  // Send an acknowledgement?
        {
            time = PRE_SEND_TIME;
            sendTelegramLen = 0;
        }
        else
        {
            if (sendTries > 3)
                sendNextTelegram();

            if (sendCurTelegram)  // Send a telegram?
            {
                time = PRE_SEND_TIME + ((sendCurTelegram[0] >> 2) & 3) * BIT_TIME;
                sendTelegramLen = telegramSize(sendCurTelegram) + 1;

                if (sendTries == 1)
                {
                    // If it is the first repeat, then mark the telegram as being repeated and correct the checksum
                    sendCurTelegram[0] &= ~SB_TEL_REPEAT_FLAG;
                    sendCurTelegram[sendTelegramLen - 1] ^= SB_TEL_REPEAT_FLAG;

                    // We increase sendTries here to avoid inverting the repeat flag again
                    // if sending fails due to collision.
                    ++sendTries;
                }
            }
            else  // Send nothing
            {
                idleState();
                break;
            }
        }

        timer.match(pwmChannel, time);
        timer.match(timeChannel, time + BIT_PULSE_TIME);
        timer.matchMode(timeChannel, RESET | INTERRUPT);
        timer.captureMode(captureChannel, FALLING_EDGE | INTERRUPT);

        nextByteIndex = 0;
        state = Bus::SEND_START_BIT;
        break;

    // The start bit of the first byte is being sent. We should come here when the flank
    // of the start bit is captured by bus-in. We might come here when somebody else started
    // sending before us, or if a timeout occurred. In case of a timeout, we have a hardware
    // problem as receiving our sent signal does not work.
    case Bus::SEND_START_BIT:
        if (timer.flag(captureChannel))
        {
            // Abort sending if we receive a start bit early enough to abort.
            // We will receive our own start bit here too.
            if (timer.value() < timer.match(pwmChannel) - 10)
            {
                timer.match(pwmChannel, 0xffff);
                state = Bus::RECV_START;
                goto STATE_SWITCH;
            }

            state = Bus::SEND_BIT_0;
            break;
        }
        else if (timer.flag(timeChannel))
        {
            // Timeout: we have a hardware problem as receiving our sent signal does not work.
            // for now we will just continue
            D(digitalWrite(PIO2_8, 1));  // blue: sending bits does not work
        }
        // No break here

    case Bus::SEND_BIT_0:
        if (sendAck)
            currentByte = sendAck;
        else currentByte = sendCurTelegram[nextByteIndex++];

        // Calculate the parity bit
        for (bitMask = 1; bitMask < 0x100; bitMask <<= 1)
        {
            if (currentByte & bitMask)
                currentByte ^= 0x100;
        }

        bitMask = 1;
        // no break here

    case Bus::SEND_BIT:
        D(digitalWrite(PIO3_2, 1));    // yellow: send next bits

        // Search for the next zero bit and count the one bits for the wait time
        time = BIT_TIME;
        while ((currentByte & bitMask) && bitMask <= 0x100)
        {
            bitMask <<= 1;
            time += BIT_TIME;
        }
        bitMask <<= 1;

        if (time <= BIT_TIME)
            state = Bus::SEND_BIT;
        else state = Bus::SEND_BIT_WAIT; // detect collisions while sending one bits

        if (bitMask > 0x200)
        {
            time += BIT_TIME * 3; // Stop bit + inter-byte timeout

            if (nextByteIndex < sendTelegramLen && !sendAck)
            {
                state = Bus::SEND_BIT_0;
            }
            else
            {
                state = Bus::SEND_END;
            }
        }

        if (state == Bus::SEND_BIT_WAIT)
            timer.captureMode(captureChannel, FALLING_EDGE | INTERRUPT);
        else timer.captureMode(captureChannel, FALLING_EDGE);

        if (state == Bus::SEND_END)
            timer.match(pwmChannel, 0xffff);
        else timer.match(pwmChannel, time - BIT_PULSE_TIME);

        timer.match(timeChannel, time);
        break;

    // Wait for a capture event from bus-in. This should be from us sending a zero bit, but it
    // might as well be from somebody else in case of a collision.
    case Bus::SEND_BIT_WAIT:
        if (timer.capture(captureChannel) < timer.match(pwmChannel) - BIT_WAIT_TIME)
        {
            // A collision. Stop sending and ignore the current transmission.
            D(digitalWrite(PIO3_3, 1));  // purple
            timer.match(pwmChannel, 0xffff);
            state = Bus::RECV_BYTE;
            collision = true;
            break;
        }
        state = Bus::SEND_BIT;
        break;

    case Bus::SEND_END:
        D(digitalWrite(PIO2_9, 1));
        timer.match(timeChannel, SEND_WAIT_TIME);
        timer.captureMode(captureChannel, FALLING_EDGE | INTERRUPT);

        if (sendAck) sendAck = 0;
        else ++sendTries;

        state = Bus::SEND_WAIT;
        break;

    // Wait for ACK or resend / send next telegram
    case Bus::SEND_WAIT:
        if (timer.flag(captureChannel) && timer.capture(captureChannel) < SEND_ACK_WAIT_TIME)
        {
            // Ignore bits that arrive too early
            break;
        }
        state = Bus::SEND_INIT;  // Receiving will be handled there too
        goto STATE_SWITCH;

    default:
        idleState();
        break;
    }

    timer.resetFlags();
}

/**
 * Prepare the telegram for sending. Set the sender address to our own
 * address, and calculate the checksum of the telegram.
 * Stores the checksum at telegram[length].
 *
 * @param telegram - the telegram to process
 * @param length - the length of the telegram
 */
void Bus::prepareTelegram(unsigned char* telegram, unsigned short length) const
{
    unsigned char checksum = 0xff;
    unsigned short i;

    // Set the sender address
    telegram[1] = ownAddr >> 8;
    telegram[2] = ownAddr;

    // Calculate the checksum
    for (i = 0; i < length; ++i)
        checksum ^= telegram[i];
    telegram[length] = checksum;
}

/**
 * Send a telegram. The checksum byte will be added at the end of telegram[].
 * Ensure that there is at least one byte space at the end of telegram[].
 *
 * @param telegram - the telegram to be sent.
 * @param length - the length of the telegram in sbSendTelegram[], without the checksum
 */
void Bus::sendTelegram(unsigned char* telegram, unsigned short length)
{
    prepareTelegram(telegram, length);

    // Wait until there is space in the sending queue
    while (sendNextTel)
    {
    }

    if (!sendCurTelegram) sendCurTelegram = telegram;
    else if (!sendNextTel) sendNextTel = telegram;
    else fatalError();   // soft fault: send buffer overflow

    // Start sending if the bus is idle
    noInterrupts();
    if (state == IDLE)
    {
        sendTries = 0;
        state = Bus::SEND_INIT;
        D(debugLine = __LINE__);

        timer.match(timeChannel, 1);
        timer.matchMode(timeChannel, INTERRUPT | RESET);
        timer.value(0);
    }
    interrupts();
}
