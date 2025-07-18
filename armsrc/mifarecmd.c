//-----------------------------------------------------------------------------
// Copyright (C) Gerhard de Koning Gans - May 2008
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

#include "mifarecmd.h"

#include "pmflash.h"
#include "proxmark3_arm.h"
#include "string.h"
#include "mifareutil.h"
#include "protocols.h"
#include "parity.h"
#include "BigBuf.h"
#include "cmd.h"
#include "flashmem.h"
#include "fpgaloader.h"
#include "iso14443a.h"
#include "mifaredesfire.h"
#include "util.h"
#include "commonutil.h"
#include "crc16.h"
#include "dbprint.h"
#include "ticks.h"
#include "usb_cdc.h"  // usb_poll_validate_length
#include "spiffs.h"   // spiffs
#include "appmain.h"  // print_stack_usage

#ifndef HARDNESTED_AUTHENTICATION_TIMEOUT
# define HARDNESTED_AUTHENTICATION_TIMEOUT  848     // card times out 1ms after wrong authentication (according to NXP documentation)
#endif
#ifndef HARDNESTED_PRE_AUTHENTICATION_LEADTIME
# define HARDNESTED_PRE_AUTHENTICATION_LEADTIME 400 // some (non standard) cards need a pause after select before they are ready for first authentication
#endif

// send an incomplete dummy response in order to trigger the card's authentication failure timeout
#ifndef CHK_TIMEOUT
# define CHK_TIMEOUT(void) { \
        ReaderTransmit(&dummy_answer, 1, NULL); \
        uint32_t timeout = GetCountSspClk() + HARDNESTED_AUTHENTICATION_TIMEOUT; \
        while (GetCountSspClk() < timeout) {}; \
    }
#endif

static uint8_t dummy_answer = 0;

// magic uid card generation 1 commands
static uint8_t wupC1[] = { MIFARE_MAGICWUPC1 };
static uint8_t wupC2[] = { MIFARE_MAGICWUPC2 };
static uint8_t wipeC[] = { MIFARE_MAGICWIPEC };
// GDM alt magic wakeup
static uint8_t wupGDM1[] = { MIFARE_MAGIC_GDM_WUPC1 };
static uint8_t wupGDM2[] = { MIFARE_MAGIC_GDM_WUPC2 };

static bool mifare_wakeup_auth(struct Crypto1State *pcs, MifareWakeupType wakeup, uint8_t key_auth_cmd, uint8_t *key, uint8_t block_no) {
    uint32_t cuid = 0;
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t receivedAnswerPar[MAX_MIFARE_PARITY_SIZE] = {0x00};

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    switch (wakeup) {
        case MF_WAKE_NONE: {
            break;
        }
        case MF_WAKE_WUPA: {
            if (iso14443a_select_cardEx(NULL, NULL, &cuid, true, 0, true, &WUPA_POLLING_PARAMETERS, false) == 0) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
                return false;
            };
            break;
        }
        case MF_WAKE_REQA: {
            if (iso14443a_select_cardEx(NULL, NULL, &cuid, true, 0, true, &REQA_POLLING_PARAMETERS, false) == 0) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
                return false;
            };
            break;
        }
        case MF_WAKE_GEN1A: {
            ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("wupC1 error");
                return false;
            }

            ReaderTransmit(wupC2, sizeof(wupC2), NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Assuming Magic Gen 1B tag. [wupC2 failed]");
            }
            break;
        }
        case MF_WAKE_GEN1B: {
            ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("wupC1 error");
                return false;
            }
            break;
        }
        case MF_WAKE_GDM_ALT: {
            ReaderTransmitBitsPar(wupGDM1, 7, NULL, NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("wupGDM1 error");
                return false;
            }
            ReaderTransmit(wupGDM2, sizeof(wupGDM2), NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("wupGDM2 error");
                // maybe this is fine on some tags?
            }
            break;
        }
    }

    if (key_auth_cmd != 0) {
        uint64_t ui64key = bytes_to_num(key, 6);
        if (mifare_classic_authex_cmd(pcs, cuid, block_no, key_auth_cmd, ui64key, AUTH_FIRST, NULL, NULL, NULL, NULL, false, false)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Auth error");
            return false;
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
// Select, Authenticate, Read a MIFARE tag.
// wakeup determines the type of wakeup
// key_auth_cmd is usually one of MIFARE_AUTH_KEYA, MIFARE_AUTH_KEYB, MIFARE_MAGIC_GDM_AUTH_KEY or 0 to disable auth
// read_cmd is usually one of ISO14443A_CMD_READBLOCK, MIFARE_MAGIC_GDM_READBLOCK, or MIFARE_MAGIC_GDM_READ_CFG
// block_data must be 16*count bytes large
// block_no through block_no+count-1 normally needs to be within the same sector
//-----------------------------------------------------------------------------
int16_t mifare_cmd_readblocks(MifareWakeupType wakeup, uint8_t key_auth_cmd, uint8_t *key, uint8_t read_cmd, uint8_t block_no, uint8_t count, uint8_t *block_data) {
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs = (key_auth_cmd == 0) ? NULL : &mpcs;;

    uint32_t timeout = iso14a_get_timeout();

    int retval = PM3_SUCCESS;

    if (mifare_wakeup_auth(pcs, wakeup, key_auth_cmd, key, block_no) == false) {
        retval = PM3_ESOFT;
        goto OUT;
    }

    // frame waiting time (FWT) in 1/fc   (524288)
    uint32_t fwt = 256 * 16 * (1 << 7);
    iso14a_set_timeout(fwt / (8 * 16)); // 4096

    for (uint8_t i = 0; i < count; i++) {
        if (mifare_classic_readblock_ex(pcs, block_no + i, block_data + (i * 16), read_cmd)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Read block error");
            retval = PM3_ESOFT;
            goto OUT;
        };
    }

    if (mifare_classic_halt(pcs)) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
        retval = PM3_ESOFT;
        goto OUT;
    };

OUT:
    crypto1_deinit(pcs);

    iso14a_set_timeout(timeout);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
    BigBuf_free();
    return retval;
}

//-----------------------------------------------------------------------------
// Select, Authenticate, Write a MIFARE tag.
// wakeup determines the type of wakeup
// key_auth_cmd is usually one of MIFARE_AUTH_KEYA, MIFARE_AUTH_KEYB, MIFARE_MAGIC_GDM_AUTH_KEY or 0 to disable auth
// write_cmd is usually one of ISO14443A_CMD_WRITEBLOCK, MIFARE_MAGIC_GDM_WRITEBLOCK, or MIFARE_MAGIC_GDM_WRITE_CFG
// block_data must be 16*count bytes large
// block_no through block_no+count-1 normally needs to be within the same sector
//-----------------------------------------------------------------------------
int16_t mifare_cmd_writeblocks(MifareWakeupType wakeup, uint8_t key_auth_cmd, uint8_t *key, uint8_t write_cmd, uint8_t block_no, uint8_t count, uint8_t *block_data) {
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs = (key_auth_cmd == 0) ? NULL : &mpcs;;

    int retval = PM3_SUCCESS;

    if (mifare_wakeup_auth(pcs, wakeup, key_auth_cmd, key, block_no) == false) {
        retval = PM3_ESOFT;
        goto OUT;
    }

    for (uint8_t i = 0; i < count; i++) {
        int res = mifare_classic_writeblock_ex(pcs, block_no + i, block_data + (i * 16), write_cmd);
        if (res == PM3_ETEAROFF) {
            retval = PM3_ETEAROFF;
            goto OUT;
        } else if (res != PM3_SUCCESS) {
            if (g_dbglevel >= DBG_INFO) Dbprintf("Write block error");
            retval = PM3_ESOFT;
            goto OUT;
        }
    }

    if (mifare_classic_halt(pcs)) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
        retval = PM3_ESOFT;
        goto OUT;
    };

OUT:
    crypto1_deinit(pcs);

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
    BigBuf_free();

    return retval;
}

//-----------------------------------------------------------------------------
// Select, Authenticate, Read a MIFARE tag.
// read sector (data = 4 x 16 bytes = 64 bytes, or 16 x 16 bytes = 256 bytes)
//-----------------------------------------------------------------------------
void MifareReadSector(uint8_t sector_no, uint8_t key_type, uint8_t *key) {
    uint8_t block_no = FirstBlockOfSector(sector_no);
    uint8_t num_blocks = NumBlocksPerSector(sector_no);

    uint8_t outbuf[16 * 16];
    int16_t retval = mifare_cmd_readblocks(MF_WAKE_WUPA, MIFARE_AUTH_KEYA + (key_type & 0xF), key, ISO14443A_CMD_READBLOCK, block_no, num_blocks, outbuf);

    reply_old(CMD_ACK, retval == PM3_SUCCESS, 0, 0, outbuf, 16 * num_blocks);
}

void MifareUC_Auth(uint8_t arg0, uint8_t *keybytes) {

    bool turnOffField = (arg0 == 1);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(0);
        return;
    };

    if (mifare_ultra_auth(keybytes) == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Authentication failed");
        OnError(1);
        return;
    }

    if (turnOffField) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        LEDsoff();
    }
    reply_mix(CMD_ACK, 1, 0, 0, 0, 0);
}

void MifareUL_AES_Auth(bool turn_off_field, uint8_t keyno, uint8_t *keybytes) {

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnErrorNG(CMD_HF_MIFAREULAES_AUTH, PM3_ESOFT);
        return;
    };

    if (mifare_ultra_aes_auth(keyno, keybytes) == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Authentication failed");
        OnErrorNG(CMD_HF_MIFAREULAES_AUTH, PM3_ESOFT);
        return;
    }

    if (turn_off_field) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        LEDsoff();
    }
    reply_ng(CMD_HF_MIFAREULAES_AUTH, PM3_SUCCESS, NULL, 0);
}

// Arg0 = BlockNo,
// Arg1 = UsePwd bool
// datain = PWD bytes,
void MifareUReadBlock(uint8_t arg0, uint8_t arg1, uint8_t *datain) {
    uint8_t blockNo = arg0;
    uint8_t dataout[16] = {0x00};
    bool useKey = (arg1 == 1); //UL_C
    bool usePwd = (arg1 == 2); //UL_EV1/NTAG

    LEDsoff();
    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(1);
        return;
    }

    // UL-C authentication
    if (useKey) {
        uint8_t key[16] = {0x00};
        memcpy(key, datain, sizeof(key));

        if (mifare_ultra_auth(key) == 0) {
            OnError(1);
            return;
        }
    }

    // UL-EV1 / NTAG authentication
    if (usePwd) {
        uint8_t pwd[4] = {0x00};
        memcpy(pwd, datain, 4);
        uint8_t pack[4] = {0, 0, 0, 0};
        if (!mifare_ul_ev1_auth(pwd, pack)) {
            OnError(1);
            return;
        }
    }

    if (mifare_ultra_readblock(blockNo, dataout)) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Read block error");
        OnError(2);
        return;
    }

    if (mifare_ultra_halt()) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
        OnError(3);
        return;
    }

    reply_mix(CMD_ACK, 1, 0, 0, dataout, 16);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
}

// arg0 = blockNo (start)
// arg1 = Pages (number of blocks)
// arg2 = useKey
// datain = KEY bytes
void MifareUReadCard(uint8_t arg0, uint16_t arg1, uint8_t arg2, uint8_t *datain) {
    LEDsoff();
    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // free eventually allocated BigBuf memory
    BigBuf_free();
    BigBuf_Clear_ext(false);
    set_tracing(true);

    // params
    uint8_t blockNo = arg0;
    uint16_t blocks = arg1;
    bool useKey = (arg2 == 1); // UL_C
    bool usePwd = (arg2 == 2); // UL_EV1/NTAG
    uint32_t countblocks = 0;
    uint8_t *dataout = BigBuf_calloc(CARD_MEMORY_SIZE);
    if (dataout == NULL) {
        Dbprintf("Failed to allocate memory");
        OnError(1);
        return;
    }

    int len = iso14443a_select_card(NULL, NULL, NULL, true, 0, true);
    if (len == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(1);
        return;
    }

    // UL-C authentication
    if (useKey) {
        uint8_t key[16] = {0x00};
        memcpy(key, datain, sizeof(key));

        if (mifare_ultra_auth(key) == 0) {
            OnError(1);
            return;
        }
    }

    // UL-EV1 / NTAG authentication
    if (usePwd) {
        uint8_t pwd[4] = {0x00};
        memcpy(pwd, datain, sizeof(pwd));
        uint8_t pack[4] = {0, 0, 0, 0};

        if (mifare_ul_ev1_auth(pwd, pack) == 0) {
            OnError(1);
            return;
        }
    }

    for (int i = 0; i < blocks; i++) {
        if ((i * 4) + 4 >= CARD_MEMORY_SIZE) {
            Dbprintf("Data exceeds buffer!!");
            break;
        }

        len = mifare_ultra_readblock(blockNo + i, dataout + (4 * i));

        if (len) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Read block %d error", i);
            // if no blocks read - error out
            if (i == 0) {
                OnError(2);
                return;
            } else {
                //stop at last successful read block and return what we got
                break;
            }
        } else {
            countblocks++;
        }
    }

    len = mifare_ultra_halt();
    if (len) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
        OnError(3);
        return;
    }

    if (g_dbglevel >= DBG_EXTENDED) Dbprintf("Blocks read %d", countblocks);

    countblocks *= 4;

    reply_mix(CMD_ACK, 1, countblocks, dataout - BigBuf_get_addr(), 0, 0);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    BigBuf_free();
    set_tracing(false);
}

void MifareValue(uint8_t arg0, uint8_t arg1, uint8_t arg2, uint8_t *datain) {
    // params
    uint8_t blockNo = arg0;
    uint8_t keyType = arg1;
    uint8_t transferKeyType = arg2;
    uint64_t ui64Key = 0;
    uint64_t transferUi64Key = 0;
    uint8_t blockdata[16] = {0x00};

    ui64Key = bytes_to_num(datain, 6);
    memcpy(blockdata, datain + 11, 16);
    transferUi64Key = bytes_to_num(datain + 27, 6);

    // variables
    uint8_t action = datain[9];
    uint8_t transferBlk = datain[10];
    bool needAuth = datain[33];
    uint8_t isOK = 0;
    uint8_t uid[10] = {0x00};
    uint32_t cuid = 0;
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t len = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    while (true) {

        if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == 0) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
            break;
        };

        if (mifare_classic_auth(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Auth error");
            break;
        };

        if (mifare_classic_value(pcs, blockNo, blockdata, action) != PM3_SUCCESS) {
            if (g_dbglevel >= DBG_INFO) Dbprintf("Write block error");
            break;
        };

        if (needAuth) {
            // transfer to other sector
            if (mifare_classic_auth(pcs, cuid, transferBlk, transferKeyType, transferUi64Key, AUTH_NESTED)) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("Nested auth error");
                break;
            }
        }

        // send transfer (commit the change)
        len = mifare_sendcmd_short(pcs, 1, MIFARE_CMD_TRANSFER, (transferBlk != 0) ? transferBlk : blockNo, receivedAnswer, sizeof(receivedAnswer), NULL, NULL);
        if (len != 1 && receivedAnswer[0] != 0x0A) {   //  0x0a - ACK
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Cmd Error in transfer: %02x", receivedAnswer[0]);
            break;
        }

        if (mifare_classic_halt(pcs)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
            break;
        };

        isOK = 1;
        break;
    }

    crypto1_deinit(pcs);

    if (g_dbglevel >= 2) DbpString("WRITE BLOCK FINISHED");

    reply_mix(CMD_ACK, isOK, 0, 0, 0, 0);

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

// Arg0   : Block to write to.
// Arg1   : 0 = use no authentication.
//          1 = use 0x1A authentication.
//          2 = use 0x1B authentication.
// datain : 4 first bytes is data to be written.
//        : 4/16 next bytes is authentication key.
static void MifareUWriteBlockEx(uint8_t arg0, uint8_t arg1, uint8_t *datain, bool reply) {
    uint8_t blockNo = arg0;
    bool useKey = (arg1 == 1); //UL_C
    bool usePwd = (arg1 == 2); //UL_EV1/NTAG
    uint8_t blockdata[4] = {0x00};

    memcpy(blockdata, datain, 4);

    LEDsoff();
    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(0);
        return;
    };

    // UL-C authentication
    if (useKey) {
        uint8_t key[16] = {0x00};
        memcpy(key, datain + 4, sizeof(key));

        if (mifare_ultra_auth(key) == 0) {
            OnError(1);
            return;
        }
    }

    // UL-EV1 / NTAG authentication
    if (usePwd) {
        uint8_t pwd[4] = {0x00};
        memcpy(pwd, datain + 4, 4);
        uint8_t pack[4] = {0, 0, 0, 0};
        if (mifare_ul_ev1_auth(pwd, pack) == 0) {
            OnError(1);
            return;
        }
    }

    if (mifare_ultra_writeblock(blockNo, blockdata) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_INFO) Dbprintf("Write block error");
        OnError(0);
        return;
    };

    if (mifare_ultra_halt()) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
        OnError(0);
        return;
    };

    if (g_dbglevel >= 2) DbpString("WRITE BLOCK FINISHED");

    if (reply) {
        reply_mix(CMD_ACK, 1, 0, 0, 0, 0);
    }
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

void MifareUWriteBlock(uint8_t arg0, uint8_t arg1, uint8_t *datain) {
    MifareUWriteBlockEx(arg0, arg1, datain, true);
}

// Arg0   : Block to write to.
// Arg1   : 0 = use no authentication.
//          1 = use 0x1A authentication.
//          2 = use 0x1B authentication.
// datain : 16 first bytes is data to be written.
//        : 4/16 next bytes is authentication key.
void MifareUWriteBlockCompat(uint8_t arg0, uint8_t arg1, uint8_t *datain) {
    uint8_t blockNo = arg0;
    bool useKey = (arg1 == 1); //UL_C
    bool usePwd = (arg1 == 2); //UL_EV1/NTAG
    uint8_t blockdata[16] = {0x00};

    memcpy(blockdata, datain, 16);

    LEDsoff();
    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(0);
        return;
    };

    // UL-C authentication
    if (useKey) {
        uint8_t key[16] = {0x00};
        memcpy(key, datain + 16, sizeof(key));

        if (mifare_ultra_auth(key) == 0) {
            OnError(1);
            return;
        }
    }

    // UL-EV1 / NTAG authentication
    if (usePwd) {
        uint8_t pwd[4] = {0x00};
        memcpy(pwd, datain + 16, 4);
        uint8_t pack[4] = {0, 0, 0, 0};
        if (!mifare_ul_ev1_auth(pwd, pack)) {
            OnError(1);
            return;
        }
    }

    if (mifare_ultra_writeblock_compat(blockNo, blockdata) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_INFO) Dbprintf("Write block error");
        OnError(0);
        return;
    };

    if (mifare_ultra_halt()) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
        OnError(0);
        return;
    };

    if (g_dbglevel >= 2) DbpString("WRITE BLOCK FINISHED");

    reply_mix(CMD_ACK, 1, 0, 0, 0, 0);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

void MifareUSetPwd(uint8_t arg0, uint8_t *datain) {

    uint8_t pwd[16] = {0x00};
    uint8_t blockdata[4] = {0x00};

    memcpy(pwd, datain, 16);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == 0) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(0);
        return;
    };

    blockdata[0] = pwd[7];
    blockdata[1] = pwd[6];
    blockdata[2] = pwd[5];
    blockdata[3] = pwd[4];
    if (mifare_ultra_writeblock(44, blockdata) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_INFO) Dbprintf("Write block error");
        OnError(44);
        return;
    };

    blockdata[0] = pwd[3];
    blockdata[1] = pwd[2];
    blockdata[2] = pwd[1];
    blockdata[3] = pwd[0];
    if (mifare_ultra_writeblock(45, blockdata) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_INFO) Dbprintf("Write block error");
        OnError(45);
        return;
    };

    blockdata[0] = pwd[15];
    blockdata[1] = pwd[14];
    blockdata[2] = pwd[13];
    blockdata[3] = pwd[12];
    if (mifare_ultra_writeblock(46, blockdata) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_INFO) Dbprintf("Write block error");
        OnError(46);
        return;
    };

    blockdata[0] = pwd[11];
    blockdata[1] = pwd[10];
    blockdata[2] = pwd[9];
    blockdata[3] = pwd[8];
    if (mifare_ultra_writeblock(47, blockdata) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_INFO) Dbprintf("Write block error");
        OnError(47);
        return;
    };

    if (mifare_ultra_halt()) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
        OnError(0);
        return;
    };

    reply_mix(CMD_ACK, 1, 0, 0, 0, 0);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

// Return 1 if the nonce is invalid else return 0
static int valid_nonce(uint32_t Nt, uint32_t NtEnc, uint32_t Ks1, const uint8_t *parity) {
    return (
               (oddparity8((Nt >> 24) & 0xFF) == ((parity[0]) ^ oddparity8((NtEnc >> 24) & 0xFF) ^ BIT(Ks1, 16))) && \
               (oddparity8((Nt >> 16) & 0xFF) == ((parity[1]) ^ oddparity8((NtEnc >> 16) & 0xFF) ^ BIT(Ks1, 8))) && \
               (oddparity8((Nt >> 8) & 0xFF) == ((parity[2]) ^ oddparity8((NtEnc >> 8) & 0xFF) ^ BIT(Ks1, 0)))
           ) ? 1 : 0;
}

void MifareAcquireNonces(uint32_t arg0, uint32_t flags) {

    uint8_t uid[10] = {0x00};
    uint8_t answer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t par[1] = {0x00};
    uint8_t buf[PM3_CMD_DATA_SIZE] = {0x00};
    uint32_t cuid = 0;
    int16_t isOK = 0;
    uint16_t num_nonces = 0;
    uint8_t cascade_levels = 0;
    uint8_t blockNo = arg0 & 0xff;
    uint8_t keyType = (arg0 >> 8) & 0xff;
    bool initialize = flags & 0x0001;
    bool field_off = flags & 0x0004;
    bool have_uid = false;

    LED_A_ON();
    LED_C_OFF();

    BigBuf_free();
    BigBuf_Clear_ext(false);
    set_tracing(true);

    if (initialize) {
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    }

    LED_C_ON();

    while (num_nonces < PM3_CMD_DATA_SIZE / 4) {

        // Test if the action was cancelled
        if (BUTTON_PRESS()) {
            isOK = 2;
            field_off = true;
            break;
        }

        if (!have_uid) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (iso14443a_select_card(uid, &card_info, &cuid, true, 0, true) == 0) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireNonces: Can't select card (ALL)");
                continue;
            }
            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    break;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card
            if (iso14443a_fast_select_card(uid, cascade_levels) == 0) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireNonces: Can't select card (UID)");
                continue;
            }
        }

        // Transmit MIFARE_CLASSIC_AUTH
        uint8_t dcmd[4] = {0x60 + (keyType & 0x01), blockNo, 0x00, 0x00};
        AddCrc14A(dcmd, 2);
        ReaderTransmit(dcmd, sizeof(dcmd), NULL);
        int len = ReaderReceive(answer, sizeof(answer), par);

        // wait for the card to become ready again
        CHK_TIMEOUT();

        if (len != 4) {
            if (g_dbglevel >= 2) Dbprintf("AcquireNonces: Auth1 error");
            continue;
        }

        // Save the tag nonce (nt)
        memcpy(buf + num_nonces * 4, answer, 4);
        num_nonces++;
    }

    LED_C_OFF();
    LED_B_ON();
    reply_old(CMD_ACK, isOK, cuid, num_nonces, buf, sizeof(buf));
    LED_B_OFF();

    if (g_dbglevel >= 3) DbpString("AcquireNonces finished");

    if (field_off) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        LEDsoff();
        set_tracing(false);
    }
}

//-----------------------------------------------------------------------------
// acquire encrypted nonces in order to perform the attack described in
// Carlo Meijer, Roel Verdult, "Ciphertext-only Cryptanalysis on Hardened
// Mifare Classic Cards" in Proceedings of the 22nd ACM SIGSAC Conference on
// Computer and Communications Security, 2015
//-----------------------------------------------------------------------------
void MifareAcquireEncryptedNonces(uint32_t arg0, uint32_t arg1, uint32_t flags, uint8_t *datain) {

    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    uint8_t uid[10] = {0x00};
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t par_enc[1] = {0x00};
    uint8_t buf[PM3_CMD_DATA_SIZE] = {0x00};

    uint64_t ui64Key = bytes_to_num(datain, 6);
    uint32_t cuid = 0;
    int16_t isOK = PM3_SUCCESS;
    uint16_t num_nonces = 0;
    uint8_t nt_par_enc = 0;
    uint8_t cascade_levels = 0;
    uint8_t blockNo = arg0 & 0xff;
    uint8_t keyType = (arg0 >> 8) & 0xff;
    uint8_t targetBlockNo = arg1 & 0xff;
    uint8_t targetKeyType = (arg1 >> 8) & 0xff;
    bool initialize = flags & 0x0001;
    bool slow = flags & 0x0002;
    bool field_off = flags & 0x0004;
    bool have_uid = false;

    LED_A_ON();
    LED_C_OFF();

    BigBuf_free();
    BigBuf_Clear_ext(false);
    set_tracing(false);

    if (initialize) {
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    }

    LED_C_ON();

    uint8_t prev_enc_nt[] = {0, 0, 0, 0};
    uint8_t prev_counter = 0;

    for (uint16_t i = 0; i <= PM3_CMD_DATA_SIZE - 9;) {

        // Test if the action was cancelled
        if (BUTTON_PRESS()) {
            isOK = PM3_EOPABORTED;
            field_off = true;
            break;
        }

        if (have_uid == false) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (iso14443a_select_card(uid, &card_info, &cuid, true, 0, true) == 0) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireEncryptedNonces: Can't select card (ALL)");
                continue;
            }
            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    break;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card
            if (iso14443a_fast_select_card(uid, cascade_levels) == 0) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireEncryptedNonces: Can't select card (UID)");
                continue;
            }
        }

        if (slow)
            SpinDelayUs(HARDNESTED_PRE_AUTHENTICATION_LEADTIME);

        uint32_t nt1 = 0;
        if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, NULL)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireEncryptedNonces: Auth1 error");
            continue;
        }

        // nested authentication
        uint16_t len = mifare_sendcmd_short(pcs, AUTH_NESTED, MIFARE_AUTH_KEYA + (targetKeyType & 0xF), targetBlockNo, receivedAnswer, sizeof(receivedAnswer), par_enc, NULL);

        // wait for the card to become ready again
        CHK_TIMEOUT();

        if (len != 4) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireEncryptedNonces: Auth2 error len=%d", len);
            continue;
        }

        num_nonces++;
        if (num_nonces % 2) {
            memcpy(buf + i, receivedAnswer, 4);
            nt_par_enc = par_enc[0] & 0xf0;
        } else {
            nt_par_enc |= par_enc[0] >> 4;
            memcpy(buf + i + 4, receivedAnswer, 4);
            memcpy(buf + i + 8, &nt_par_enc, 1);
            i += 9;
        }


        if (prev_enc_nt[0] == receivedAnswer[0] &&
                prev_enc_nt[1] == receivedAnswer[1] &&
                prev_enc_nt[2] == receivedAnswer[2] &&
                prev_enc_nt[3] == receivedAnswer[3]
           ) {
            prev_counter++;
        }
        memcpy(prev_enc_nt, receivedAnswer, 4);
        if (prev_counter == 5) {
            if (g_dbglevel >= DBG_EXTENDED) {
                DbpString("Static encrypted nonce detected, exiting...");
                uint32_t a = bytes_to_num(prev_enc_nt, 4);
                uint32_t b = bytes_to_num(receivedAnswer, 4);
                Dbprintf("( %08x vs %08x )", a, b);
            }
            isOK = PM3_ESTATIC_NONCE;
            break;
        }

    }

    LED_C_OFF();
    crypto1_deinit(pcs);
    LED_B_ON();
    reply_old(CMD_ACK, isOK, cuid, num_nonces, buf, sizeof(buf));
    LED_B_OFF();

    if (field_off) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        LEDsoff();
        set_tracing(false);
    }
}


//-----------------------------------------------------------------------------
// acquire static encrypted nonces in order to perform the attack described in
// Philippe Teuwen, "MIFARE Classic: exposing the static encrypted nonce variant"
//-----------------------------------------------------------------------------
int MifareAcquireStaticEncryptedNonces(uint32_t flags, const uint8_t *key, bool reply, uint8_t first_block_no, uint8_t first_key_type) {
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    uint8_t uid[10] = {0x00};
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t par_enc[1] = {0x00};
    // ((MIFARE_1K_MAXSECTOR + 1) * 2) * 8 < PM3_CMD_DATA_SIZE
    // we're storing nonces in emulator memory at CARD_MEMORY_RF08S_OFFSET
    // one sector data in one 16-byte block with for each keytype:
    // uint16_t nt_first_half (as we can reconstruct the other half)
    // uint8_t  nt_par_err
    // uint8_t  flag: if 0xAA and key=000000000000 it means we don't know the key yet
    // uint32_t nt_enc
    // buf: working buffer to prepare those "blocks"
    uint8_t buf[MIFARE_BLOCK_SIZE] = {0x00};
    uint64_t ui64Key = bytes_to_num(key, 6);
    bool with_data = flags & 1;
    bool without_backdoor = (flags >> 1) & 1;
    if (with_data && without_backdoor) {
        return PM3_EINVARG;
    }
    uint32_t cuid = 0;
    int16_t isOK = PM3_SUCCESS;
    uint8_t cascade_levels = 0;
    bool have_uid = false;

    LED_A_ON();
    LED_C_OFF();

    BigBuf_free();
    BigBuf_Clear_ext(false);
    set_tracing(false);

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    LED_C_ON();

    if (without_backdoor) {
        uint32_t nt1 = 0;

        iso14a_card_select_t card_info;
        if (iso14443a_select_card(uid, &card_info, &cuid, true, 0, true) == 0) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Can't select card (ALL)");
            isOK = PM3_ERFTRANS;
            goto out;
        }
        switch (card_info.uidlen) {
            case 4 :
                cascade_levels = 1;
                break;
            case 7 :
                cascade_levels = 2;
                break;
            case 10:
                cascade_levels = 3;
                break;
            default:
                break;
        }
        if (mifare_classic_authex_cmd(pcs, cuid, first_block_no, MIFARE_AUTH_KEYA + first_key_type, ui64Key, AUTH_FIRST, &nt1, NULL, NULL, NULL, false, false)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth1 error");
            isOK = PM3_ESOFT;
            goto out;
        };

        uint16_t len = mifare_sendcmd_short(pcs, AUTH_NESTED, MIFARE_AUTH_KEYA + first_key_type, first_block_no, receivedAnswer, sizeof(receivedAnswer), par_enc, NULL);
        if (len != 4) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth2 error len=%d", len);
            isOK = PM3_ESOFT;
            goto out;
        }
        uint32_t nt_enc = bytes_to_num(receivedAnswer, 4);

        // send some crap to fail auth
        CHK_TIMEOUT();

        if (iso14443a_fast_select_card(uid, cascade_levels) == 0) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Can't select card (UID)");
            isOK = PM3_ERFTRANS;
            goto out;
        }
        if (mifare_classic_authex_cmd(pcs, cuid, first_block_no, MIFARE_AUTH_KEYA + first_key_type, ui64Key, AUTH_FIRST, &nt1, NULL, NULL, NULL, false, false)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth1 error");
            isOK = PM3_ESOFT;
            goto out;
        };
        // Recover clear nt
        struct Crypto1State mpcs_tmp = {0, 0};
        struct Crypto1State *pcs_tmp = &mpcs_tmp;
        crypto1_init(pcs_tmp, ui64Key);
        uint32_t nt = crypto1_word(pcs_tmp, nt_enc ^ cuid, 1) ^ nt_enc;
        int dist = nonce_distance(nt, nt1);
        // ref dist is not always stable. Adjust physical distance to maximise ref dist, and try values around estimated nonces...
        Dbprintf("Block %2i key %i nested nT=%08x first nT=%08x dist=%i", first_block_no, first_key_type, nt, nt1, dist);

        for (uint16_t sec = 0; sec < MIFARE_1K_MAXSECTOR + 1; sec++) {
            uint16_t sec_gap = sec;
            if (sec >= MIFARE_1K_MAXSECTOR) {
                // gap between user blocks and advanced verification method blocks
                sec_gap += 16;
            }
            uint16_t blockNo = sec_gap * 4;
            for (uint8_t keyType = 0; keyType < 2; keyType++) {
                // Test if the action was cancelled
                if (BUTTON_PRESS()) {
                    isOK = PM3_EOPABORTED;
                    break;
                }

                len = mifare_sendcmd_short(pcs, AUTH_NESTED, MIFARE_AUTH_KEYA + keyType, blockNo, receivedAnswer, sizeof(receivedAnswer), par_enc, NULL);
                if (len != 4) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth2 error len=%d", len);
                    isOK = PM3_ESOFT;
                    goto out;
                }
                // store nt_enc
                memcpy(buf + (keyType * 8) + 4, receivedAnswer, 4);
                nt_enc = bytes_to_num(receivedAnswer, 4);
                uint8_t nt_par_err = ((((par_enc[0] >> 7) & 1) ^ oddparity8((nt_enc >> 24) & 0xFF)) << 3 |
                                      (((par_enc[0] >> 6) & 1) ^ oddparity8((nt_enc >> 16) & 0xFF)) << 2 |
                                      (((par_enc[0] >> 5) & 1) ^ oddparity8((nt_enc >> 8) & 0xFF)) << 1 |
                                      (((par_enc[0] >> 4) & 1) ^ oddparity8((nt_enc >> 0) & 0xFF)));
                // Dbprintf("Sec %2i key %i {nT}=%02x%02x%02x%02x perr=%x", sec, keyType, receivedAnswer[0], receivedAnswer[1], receivedAnswer[2], receivedAnswer[3], nt_par_err);
                // store nt_par_err
                buf[(keyType * 8) + 2] = nt_par_err;
                buf[(keyType * 8) + 3] = 0xAA; // extra check to tell we have nt/nt_enc/par_err

                // send some crap to fail auth
                CHK_TIMEOUT();

                if (iso14443a_fast_select_card(uid, cascade_levels) == 0) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Can't select card (UID)");
                    isOK = PM3_ERFTRANS;
                    goto out;
                }
                if (mifare_classic_authex_cmd(pcs, cuid, first_block_no, MIFARE_AUTH_KEYA + first_key_type, ui64Key, AUTH_FIRST, &nt1, NULL, NULL, NULL, false, false)) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth1 error");
                    isOK = PM3_ESOFT;
                    goto out;
                };
                nt1 = rewind_nonce(nt1, dist);
                num_to_bytes(nt1 >> 16, 2, buf + (keyType * 8));
                emlSetMem_xt(buf, (CARD_MEMORY_RF08S_OFFSET / MIFARE_BLOCK_SIZE) + sec, 1, MIFARE_BLOCK_SIZE);
            }
        }
    } else {
        for (uint16_t sec = 0; sec < MIFARE_1K_MAXSECTOR + 1; sec++) {
            uint16_t sec_gap = sec;
            if (sec >= MIFARE_1K_MAXSECTOR) {
                // gap between user blocks and advanced verification method blocks
                sec_gap += 16;
            }
            uint16_t blockNo = sec_gap * 4;
            for (uint8_t keyType = 0; keyType < 2; keyType++) {
                // Test if the action was cancelled
                if (BUTTON_PRESS()) {
                    isOK = PM3_EOPABORTED;
                    break;
                }
                if (have_uid == false) { // need a full select cycle to get the uid first
                    iso14a_card_select_t card_info;
                    if (iso14443a_select_card(uid, &card_info, &cuid, true, 0, true) == 0) {
                        if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Can't select card (ALL)");
                        isOK = PM3_ERFTRANS;
                        goto out;
                    }
                    switch (card_info.uidlen) {
                        case 4 :
                            cascade_levels = 1;
                            break;
                        case 7 :
                            cascade_levels = 2;
                            break;
                        case 10:
                            cascade_levels = 3;
                            break;
                        default:
                            break;
                    }
                    have_uid = true;
                } else { // no need for anticollision. We can directly select the card
                    if (iso14443a_fast_select_card(uid, cascade_levels) == 0) {
                        if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Can't select card (UID)");
                        isOK = PM3_ERFTRANS;
                        goto out;
                    }
                }

                uint32_t nt1 = 0;
                if (mifare_classic_authex_cmd(pcs, cuid, blockNo, MIFARE_AUTH_KEYA + keyType + 4, ui64Key, AUTH_FIRST, &nt1, NULL, NULL, NULL, false, false)) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth1 error");
                    isOK = PM3_ESOFT;
                    goto out;
                };
                if ((with_data) && (keyType == 0)) {
                    uint8_t data[16];
                    uint8_t blocks = 4;
                    if (blockNo >= MIFARE_1K_MAXSECTOR * 4) {
                        // special RF08S advanced authentication blocks, let's dump in emulator just in case
                        blocks = 8;
                    }
                    for (uint16_t tb = blockNo; tb < blockNo + blocks; tb++) {
                        memset(data, 0x00, sizeof(data));
                        int res = mifare_classic_readblock(pcs, tb, data);
                        if (res == 1) {
                            if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Read error");
                            isOK = PM3_ESOFT;
                            goto out;
                        }
                        emlSetMem_xt(data, tb, 1, 16);
                    }
                }
                // nested authentication
                uint16_t len = mifare_sendcmd_short(pcs, AUTH_NESTED, MIFARE_AUTH_KEYA + keyType + 4, blockNo, receivedAnswer, sizeof(receivedAnswer), par_enc, NULL);
                if (len != 4) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth2 error len=%d", len);
                    isOK = PM3_ESOFT;
                    goto out;
                }
                uint32_t nt_enc = bytes_to_num(receivedAnswer, 4);
                crypto1_init(pcs, ui64Key);
                uint32_t nt = crypto1_word(pcs, nt_enc ^ cuid, 1) ^ nt_enc;
                // Dbprintf("Sec %2i key %i nT=%08x", sec, keyType + 4, nt);
                // store nt (first half)
                num_to_bytes(nt >> 16, 2, buf + (keyType * 8));
                // send some crap to fail auth
                CHK_TIMEOUT();

                if (iso14443a_fast_select_card(uid, cascade_levels) == 0) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Can't select card (UID)");
                    isOK = PM3_ERFTRANS;
                    goto out;
                }
                if (mifare_classic_authex_cmd(pcs, cuid, blockNo, MIFARE_AUTH_KEYA + keyType + 4, ui64Key, AUTH_FIRST, &nt1, NULL, NULL, NULL, false, false)) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth1 error");
                    isOK = PM3_ESOFT;
                    goto out;
                };

                // nested authentication on regular keytype
                len = mifare_sendcmd_short(pcs, AUTH_NESTED, MIFARE_AUTH_KEYA + keyType, blockNo, receivedAnswer, sizeof(receivedAnswer), par_enc, NULL);
                if (len != 4) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("AcquireStaticEncryptedNonces: Auth2 error len=%d", len);
                    isOK = PM3_ESOFT;
                    goto out;
                }
                // store nt_enc
                memcpy(buf + (keyType * 8) + 4, receivedAnswer, 4);
                nt_enc = bytes_to_num(receivedAnswer, 4);
                uint8_t nt_par_err = ((((par_enc[0] >> 7) & 1) ^ oddparity8((nt_enc >> 24) & 0xFF)) << 3 |
                                      (((par_enc[0] >> 6) & 1) ^ oddparity8((nt_enc >> 16) & 0xFF)) << 2 |
                                      (((par_enc[0] >> 5) & 1) ^ oddparity8((nt_enc >> 8) & 0xFF)) << 1 |
                                      (((par_enc[0] >> 4) & 1) ^ oddparity8((nt_enc >> 0) & 0xFF)));
                // Dbprintf("Sec %2i key %i {nT}=%02x%02x%02x%02x perr=%x", sec, keyType, receivedAnswer[0], receivedAnswer[1], receivedAnswer[2], receivedAnswer[3], nt_par_err);
                // store nt_par_err
                buf[(keyType * 8) + 2] = nt_par_err;
                buf[(keyType * 8) + 3] = 0xAA; // extra check to tell we have nt/nt_enc/par_err
                emlSetMem_xt(buf, (CARD_MEMORY_RF08S_OFFSET / MIFARE_BLOCK_SIZE) + sec, 1, MIFARE_BLOCK_SIZE);
                // send some crap to fail auth
                CHK_TIMEOUT();
            }
        }
    }
out:
    LED_C_OFF();
    crypto1_deinit(pcs);
    LED_B_ON();
    if (reply) {
        reply_mix(CMD_ACK, isOK, cuid, 0, BigBuf_get_EM_addr() + CARD_MEMORY_RF08S_OFFSET, MIFARE_BLOCK_SIZE * (MIFARE_1K_MAXSECTOR + 1));
    }
    LED_B_OFF();

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
    return isOK;
}


//-----------------------------------------------------------------------------
// MIFARE nested authentication.
//
//-----------------------------------------------------------------------------
void MifareNested(uint8_t blockNo, uint8_t keyType, uint8_t targetBlockNo, uint8_t targetKeyType, bool calibrate, uint8_t *key) {
    uint64_t ui64Key = 0;
    ui64Key = bytes_to_num(key, 6);

    // variables
    uint16_t i, j, len;
    static uint16_t dmin, dmax;

    uint8_t par[1] = {0x00};
    uint8_t par_array[4] = {0x00};
    uint8_t uid[10] = {0x00};
    uint32_t cuid = 0, nt1, nt2, nttest, ks1;
    uint32_t target_nt[2] = {0x00}, target_ks[2] = {0x00};

    uint16_t ncount = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};

    uint32_t auth1_time, auth2_time;
    static uint16_t delta_time = 0;

    LED_A_ON();
    LED_C_OFF();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // free eventually allocated BigBuf memory
    BigBuf_free();
    BigBuf_Clear_ext(false);

    set_tracing(true);

    // statistics on nonce distance
    int16_t isOK = PM3_SUCCESS;
#define NESTED_MAX_TRIES 12
    if (calibrate) { // calibrate: for first call only. Otherwise reuse previous calibration
        LED_B_ON();
        WDT_HIT();

        uint32_t prev_enc_nt = 0;
        uint8_t prev_counter = 0;

        uint16_t unsuccessful_tries = 0;
        uint16_t davg = 0;
        dmax = 0;
        dmin = 2000;
        delta_time = 0;
        uint16_t rtr;
        for (rtr = 0; rtr < 17; rtr++) {

            // Test if the action was cancelled
            if (BUTTON_PRESS() || data_available()) {
                isOK = PM3_EOPABORTED;
                break;
            }

            // prepare next select. No need to power down the card.
            if (mifare_classic_halt(pcs)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Nested: Halt error");
                rtr--;
                continue;
            }

            if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == 0) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Nested: Can't select card");
                rtr--;
                continue;
            };

            auth1_time = 0;
            if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, &auth1_time)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Nested: Auth1 error");
                rtr--;
                continue;
            };
            auth2_time = (delta_time) ? auth1_time + delta_time : 0;

            if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_NESTED, &nt2, &auth2_time)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Nested: Auth2 error");
                rtr--;
                continue;
            };

            // cards with fixed nonce
            // NXP Mifare is typical around 840,but for some unlicensed/compatible mifare card this can be 160

            uint32_t nttmp = prng_successor(nt1, 100);
            for (i = 101; i < 1200; i++) {
                nttmp = prng_successor(nttmp, 1);
                if (nttmp == nt2) break;
            }

            if (i != 1200) {
                if (rtr != 0) {
                    davg += i;
                    dmin = MIN(dmin, i);
                    dmax = MAX(dmax, i);
                } else {
                    // allow some slack for proper timing
                    delta_time = auth2_time - auth1_time + 32;
                }
                if (g_dbglevel >= DBG_DEBUG) Dbprintf("Nested: calibrating... ntdist=%d", i);
            } else {
                unsuccessful_tries++;
                // card isn't vulnerable to nested attack (random numbers are not predictable)
                if (unsuccessful_tries > NESTED_MAX_TRIES) {
                    isOK = PM3_EFAILED;
                }
            }


            if (nt1 == nt2) {
                prev_counter++;
            }
            prev_enc_nt = nt2;

            if (prev_counter == 5) {
                if (g_dbglevel >= DBG_EXTENDED) {
                    DbpString("Static encrypted nonce detected, exiting...");
                    Dbprintf("( %08x vs %08x )", prev_enc_nt, nt2);
                }
                isOK = PM3_ESTATIC_NONCE;
                break;
            }
        }

        if (rtr > 1)
            davg = (davg + (rtr - 1) / 2) / (rtr - 1);

        if (g_dbglevel >= DBG_DEBUG) Dbprintf("rtr=%d isOK=%d min=%d max=%d avg=%d, delta_time=%d", rtr, isOK, dmin, dmax, davg, delta_time);

        dmin = davg - 2;
        dmax = davg + 2;

        LED_B_OFF();
    }
//  -------------------------------------------------------------------------------------------------

    LED_C_ON();

    //  get crypted nonces for target sector
    for (i = 0; ((i < 2) && (isOK == PM3_SUCCESS)); i++) {

        // look for exactly two different nonces

        target_nt[i] = 0;
        // continue until we have an unambiguous nonce
        while (target_nt[i] == 0) {

            // Test if the action was cancelled
            if (BUTTON_PRESS() || data_available()) {
                isOK = PM3_EOPABORTED;
                break;
            }

            // prepare next select. No need to power down the card.
            if (mifare_classic_halt(pcs)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Nested: Halt error");
                continue;
            }

            if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == false) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Nested: Can't select card");
                continue;
            };

            auth1_time = 0;
            if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, &auth1_time)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Nested: Auth1 error");
                continue;
            };

            // nested authentication
            auth2_time = auth1_time + delta_time;

            len = mifare_sendcmd_short(pcs, AUTH_NESTED, MIFARE_AUTH_KEYA + (targetKeyType & 0xF), targetBlockNo, receivedAnswer, sizeof(receivedAnswer), par, &auth2_time);
            if (len != 4) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Nested: Auth2 error len=%d", len);
                continue;
            };

            nt2 = bytes_to_num(receivedAnswer, 4);
            if (g_dbglevel >= DBG_DEBUG) Dbprintf("Nonce#%d: Testing nt1=%08x nt2enc=%08x nt2par=%02x", i + 1, nt1, nt2, par[0]);

            // Parity validity check
            for (j = 0; j < 4; j++) {
                par_array[j] = (oddparity8(receivedAnswer[j]) != ((par[0] >> (7 - j)) & 0x01));
            }

            ncount = 0;
            nttest = prng_successor(nt1, dmin - 1);
            for (j = dmin; j < dmax + 1; j++) {
                nttest = prng_successor(nttest, 1);
                ks1 = nt2 ^ nttest;

                if (valid_nonce(nttest, nt2, ks1, par_array)) {
                    if (ncount > 0) { // we are only interested in disambiguous nonces, try again
                        if (g_dbglevel >= DBG_DEBUG) Dbprintf("Nonce#%d: dismissed (ambiguous), ntdist=%d", i + 1, j);
                        target_nt[i] = 0;
                        break;
                    }
                    target_nt[i] = nttest;
                    target_ks[i] = ks1;
                    ncount++;
                    if (i == 1 && target_nt[1] == target_nt[0]) { // we need two different nonces
                        target_nt[i] = 0;
                        if (g_dbglevel >= DBG_DEBUG) Dbprintf("Nonce#2: dismissed (= nonce#1), ntdist=%d", j);
                        break;
                    }
                    if (g_dbglevel >= DBG_DEBUG) Dbprintf("Nonce#%d: valid, ntdist=%d", i + 1, j);
                }
            }
            if (target_nt[i] == 0 && j == dmax + 1 && g_dbglevel >= 3) Dbprintf("Nonce#%d: dismissed (all invalid)", i + 1);
        }
    }

    LED_C_OFF();

    crypto1_deinit(pcs);

    struct p {
        int16_t isOK;
        uint8_t block;
        uint8_t keytype;
        uint8_t cuid[4];
        uint8_t nt_a[4];
        uint8_t ks_a[4];
        uint8_t nt_b[4];
        uint8_t ks_b[4];
    } PACKED payload;
    payload.isOK = isOK;
    payload.block = targetBlockNo;
    payload.keytype = targetKeyType;

    memcpy(payload.cuid, &cuid, 4);
    memcpy(payload.nt_a, &target_nt[0], 4);
    memcpy(payload.ks_a, &target_ks[0], 4);
    memcpy(payload.nt_b, &target_nt[1], 4);
    memcpy(payload.ks_b, &target_ks[1], 4);

    LED_B_ON();
    reply_ng(CMD_HF_MIFARE_NESTED, PM3_SUCCESS, (uint8_t *)&payload, sizeof(payload));
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

void MifareStaticNested(uint8_t blockNo, uint8_t keyType, uint8_t targetBlockNo, uint8_t targetKeyType, uint8_t *key) {

    LEDsoff();

    uint64_t ui64Key = bytes_to_num(key, 6);
    uint16_t len, dist1 = 160, dist2 = 320;
    uint8_t uid[10] = { 0x00 };
    uint32_t cuid = 0, nt1 = 0, nt2 = 0, nt3 = 0;
    uint32_t target_nt[2] = {0x00}, target_ks[2] = {0x00};
    uint8_t par[1] = { 0x00 };
    uint8_t receivedAnswer[10] = { 0x00 };

    struct Crypto1State mpcs = { 0, 0 };
    struct Crypto1State *pcs;
    pcs = &mpcs;

    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // free eventually allocated BigBuf memory
    BigBuf_free();
    BigBuf_Clear_ext(false);
    set_tracing(true);

    int16_t isOK = PM3_ESOFT;
    LED_C_ON();

    // Main loop - get crypted nonces for target sector
    for (uint8_t rtr = 0; rtr < 2; rtr++) {

        // distance measurement
        if (mifare_classic_halt(pcs)) {
            continue;
        }

        if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == false) {
            continue;
        };

        if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, NULL)) {
            continue;
        };

        if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_NESTED, &nt2, NULL)) {
            continue;
        };

        if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_NESTED, &nt3, NULL)) {
            continue;
        };

        dist1 = nonce_distance(nt1, nt2);
        dist2 = nonce_distance(nt1, nt3);

        if (mifare_classic_halt(pcs)) {
            continue;
        }

        if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == false) {
            continue;
        };

        // first collection
        if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, NULL)) {
            continue;
        };

        // pre-generate nonces
        if (targetKeyType == 1 && nt1 == 0x009080A2) {
            target_nt[0] = prng_successor(nt1, 161);
            target_nt[1] = prng_successor(nt1, 321);
        } else {
            target_nt[0] = prng_successor(nt1, dist1);
            target_nt[1] = prng_successor(nt1, dist2);
        }

        len = mifare_sendcmd_short(pcs, AUTH_NESTED, MIFARE_AUTH_KEYA + (targetKeyType & 0xF), targetBlockNo, receivedAnswer, sizeof(receivedAnswer), par, NULL);
        if (len != 4) {
            continue;
        };

        nt2 = bytes_to_num(receivedAnswer, 4);
        target_ks[0] = nt2 ^ target_nt[0];

        // second collection
        if (mifare_classic_halt(pcs)) {
            continue;
        }

        if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == false) {
            continue;
        };

        if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, NULL)) {
            continue;
        };

        if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_NESTED, NULL, NULL)) {
            continue;
        };

        len = mifare_sendcmd_short(pcs, AUTH_NESTED, MIFARE_AUTH_KEYA + (targetKeyType & 0xF), targetBlockNo, receivedAnswer, sizeof(receivedAnswer), par, NULL);
        if (len != 4) {
            continue;
        };

        nt3 = bytes_to_num(receivedAnswer, 4);
        target_ks[1] = nt3 ^ target_nt[1];

        isOK = PM3_SUCCESS;
    }

    LED_C_OFF();

    crypto1_deinit(pcs);

    struct p {
        uint8_t block;
        uint8_t keytype;
        uint8_t cuid[4];
        uint8_t nt_a[4];
        uint8_t ks_a[4];
        uint8_t nt_b[4];
        uint8_t ks_b[4];
    } PACKED payload;
    payload.block = targetBlockNo;
    payload.keytype = targetKeyType;

    memcpy(payload.cuid, &cuid, 4);
    memcpy(payload.nt_a, &target_nt[0], 4);
    memcpy(payload.ks_a, &target_ks[0], 4);
    memcpy(payload.nt_b, &target_nt[1], 4);
    memcpy(payload.ks_b, &target_ks[1], 4);

    LED_B_ON();
    reply_ng(CMD_HF_MIFARE_STATIC_NESTED, isOK, (uint8_t *)&payload, sizeof(payload));
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

//-----------------------------------------------------------------------------
// MIFARE check keys. key count up to 85.
//
//-----------------------------------------------------------------------------
typedef struct sector_t {
    uint8_t keyA[6];
    uint8_t keyB[6];
} sector_t;

typedef struct chk_t {
    uint64_t key;
    uint32_t cuid;
    uint8_t cl;
    uint8_t block;
    uint8_t keyType;
    uint8_t *uid;
    struct Crypto1State *pcs;
} chk_t;

// checks one key.
// fast select,  tries 5 times to select
//
// return:
//  4 = failed to select
//  3 = failed auth
//  2 = timeout
//  1 = failed auth
//  0 = correct
static uint8_t chkKey(struct chk_t *c) {
    uint8_t i = 0, res = 2;
    bool selected = false;
    while (i < 5) {
        // this part is from Piwi's faster nonce collecting part in Hardnested.
        // assume: fast select
        if (iso14443a_fast_select_card(c->uid, c->cl) == 0) {
            ++i;
            continue;
        }
        selected = true;
        res = mifare_classic_authex(c->pcs, c->cuid, c->block, c->keyType, c->key, AUTH_FIRST, NULL, NULL);

//        CHK_TIMEOUT();

        // if successful auth, send HALT
        // if ( !res )
        // mifare_classic_halt(c->pcs);
        break;
    }
    if (selected == false) {
        Dbprintf("chkKey: Failed at fast selecting the card!");
        res = 4;
    }
    return res;
}

static uint8_t chkKey_readb(struct chk_t *c, uint8_t *keyb) {

    if (iso14443a_fast_select_card(c->uid, c->cl) == 0) {
        return 2;
    }

    if (mifare_classic_authex(c->pcs, c->cuid, c->block, 0, c->key, AUTH_FIRST, NULL, NULL)) {
        return 1;
    }

    uint8_t data[16] = {0x00};
    uint8_t res = mifare_classic_readblock(c->pcs, c->block, data);

    // successful read
    if (res == 0) {
        // data was something else than zeros.
        if (memcmp(data + 10, "\x00\x00\x00\x00\x00\x00", 6) != 0) {
            memcpy(keyb, data + 10, 6);
        } else {
            res = 3;
        }
        mifare_classic_halt(c->pcs);
    }
    return res;
}

static void chkKey_scanA(struct chk_t *c, struct sector_t *k_sector, uint8_t *found, const uint8_t *sectorcnt, uint8_t *foundkeys) {
    for (uint8_t s = 0; s < *sectorcnt; s++) {

        // skip already found A keys
        if (found[(s * 2)]) {
            continue;
        }

        c->block = FirstBlockOfSector(s);

        uint8_t res = chkKey(c);
        if (res == 4) {
            // failed to select,  return immediately
            return;
        }

        if (res == 0) {
            num_to_bytes(c->key, 6, k_sector[s].keyA);
            found[(s * 2)] = 1;
            ++*foundkeys;

            if (g_dbglevel >= 3) Dbprintf("ChkKeys_fast: Scan A found (%d)", c->block);
        }
    }
}

static void chkKey_scanB(struct chk_t *c, struct sector_t *k_sector, uint8_t *found, const uint8_t *sectorcnt, uint8_t *foundkeys) {
    for (uint8_t s = 0; s < *sectorcnt; s++) {

        // skip already found B keys
        if (found[(s * 2) + 1]) {
            continue;
        }

        c->block = FirstBlockOfSector(s);

        uint8_t res = chkKey(c);
        if (res == 4) {
            // failed to select,  return immediately
            return;
        }

        if (res == 0) {
            num_to_bytes(c->key, 6, k_sector[s].keyB);
            found[(s * 2) + 1] = 1;
            ++*foundkeys;

            if (g_dbglevel >= 3) Dbprintf("ChkKeys_fast: Scan B found (%d)", c->block);
        }
    }
}

// loop all A keys,
// when A is found but not B,  try to read B.
static void chkKey_loopBonly(struct chk_t *c, struct sector_t *k_sector, uint8_t *found, uint8_t *sectorcnt, uint8_t *foundkeys) {

    // read Block B, if A is found.
    for (uint8_t s = 0; s < *sectorcnt; ++s) {

        if (found[(s * 2)] && found[(s * 2) + 1]) {
            continue;
        }

        c->block = (FirstBlockOfSector(s) + NumBlocksPerSector(s) - 1);

        // A but not B
        if (found[(s * 2)]  &&  !found[(s * 2) + 1]) {
            c->key = bytes_to_num(k_sector[s].keyA, 6);
            uint8_t status = chkKey_readb(c, k_sector[s].keyB);
            if (status == 0) {
                found[(s * 2) + 1] = 1;
                ++*foundkeys;

                if (g_dbglevel >= 3) Dbprintf("ChkKeys_fast: Reading B found (%d)", c->block);

                // try quick find all B?
                // assume: keys comes in groups. Find one B, test against all B.
                c->key = bytes_to_num(k_sector[s].keyB, 6);
                c->keyType = 1;
                chkKey_scanB(c, k_sector, found, sectorcnt, foundkeys);
            }
        }
    }
}

// get Chunks of keys, to test authentication against card.
// arg0 = antal sectorer
// arg0 = first time
// arg1 = clear trace
// arg2 = antal nycklar i keychunk
// datain = keys as array
void MifareChkKeys_fast(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint8_t *datain) {

    // first call or
    uint8_t sectorcnt = arg0 & 0xFF; // 16;
    uint8_t firstchunk = (arg0 >> 8) & 0xF;
    uint8_t lastchunk = (arg0 >> 12) & 0xF;
    uint16_t singleSectorParams = (arg0 >> 16) & 0xFFFF;
    uint8_t strategy = arg1 & 0xFF;
    uint8_t use_flashmem = (arg1 >> 8) & 0xFF;
    uint16_t keyCount = arg2 & 0xFF;
    uint8_t status = 0;
    bool singleSectorMode = (singleSectorParams >> 15) & 1;
    uint8_t keytype = (singleSectorParams >> 8) & 1;
    uint8_t blockn = singleSectorParams & 0xFF;

    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;
    struct chk_t chk_data;

    uint8_t allkeys = sectorcnt << 1;

    static uint32_t cuid = 0;
    static uint8_t cascade_levels = 0;
    static uint8_t foundkeys = 0;
    static sector_t k_sector[80];
    static uint8_t found[80];
    static uint8_t uid[10] = {0};

    int oldbg = g_dbglevel;

#ifdef WITH_FLASH
    if (use_flashmem) {
        BigBuf_free();
        uint32_t size = 0;
        if (exists_in_spiffs(MF_KEYS_FILE)) {
            size = size_in_spiffs(MF_KEYS_FILE);
        }

        if ((size == 0) || (size < MF_KEY_LENGTH)) {
            Dbprintf("Spiffs file `" _RED_("%s") "` does not exists or empty", MF_KEYS_FILE);
            goto OUT;
        }

        // Compute how many keys can fit in bigbuf
        // a key is 6 bytes
        uint16_t key_mem_available = MIN((BigBuf_get_size() / MF_KEY_LENGTH), (keyCount + (size / MF_KEY_LENGTH)));

        uint8_t *dictkeys = BigBuf_calloc(key_mem_available * MF_KEY_LENGTH);
        if (dictkeys == NULL) {
            goto OUT;
        }

        // Put user and hard-coded keys first
        memcpy(dictkeys, datain, keyCount * MF_KEY_LENGTH);

        // Now append the SPI flash dictionnary
        if (SPIFFS_OK == rdv40_spiffs_read_as_filetype(MF_KEYS_FILE, dictkeys + (keyCount * MF_KEY_LENGTH), (key_mem_available - keyCount) * MF_KEY_LENGTH, RDV40_SPIFFS_SAFETY_SAFE)) {
            if (g_dbglevel >= DBG_ERROR) {
                Dbprintf("loaded " _GREEN_("%u") " keys from spiffs file `" _YELLOW_("%s") "`", key_mem_available - keyCount, MF_KEYS_FILE);
            }
        } else {
            Dbprintf("Spiffs file `" _RED_("%s") "` cannot be read", MF_KEYS_FILE);
            goto OUT;
        }
        // Replace client provided keys
        datain = dictkeys;
        keyCount = key_mem_available;
    }
#endif

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    LEDsoff();
    LED_A_ON();

    if (firstchunk) {
        clear_trace();
        set_tracing(false);

        memset(k_sector, 0x00, 480 + 10);
        memset(found, 0x00, sizeof(found));
        foundkeys = 0;

        iso14a_card_select_t card_info;
        if (iso14443a_select_card(uid, &card_info, &cuid, true, 0, true) == 0) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("ChkKeys_fast: Can't select card (ALL)");
            goto OUT;
        }

        switch (card_info.uidlen) {
            case 4 :
                cascade_levels = 1;
                break;
            case 7 :
                cascade_levels = 2;
                break;
            case 10:
                cascade_levels = 3;
                break;
            default:
                break;
        }

        CHK_TIMEOUT();
    }

    // clear debug level. We are expecting lots of authentication failures...
    g_dbglevel = DBG_NONE;

    // set check struct.
    chk_data.uid = uid;
    chk_data.cuid = cuid;
    chk_data.cl = cascade_levels;
    chk_data.pcs = pcs;
    chk_data.block = 0;

    if (singleSectorMode) {
        allkeys = 1;
        chk_data.block = blockn;
        chk_data.keyType = keytype;
        for (uint16_t i = 0; i < keyCount; ++i) {

            // Allow button press / usb cmd to interrupt device
            if (BUTTON_PRESS() || data_available()) {
                goto OUT;
            }

            WDT_HIT();

            chk_data.key = bytes_to_num(datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);
            if (chkKey(&chk_data) == 0) {
                foundkeys++;
                reply_old(CMD_ACK, 1, 0, 0, datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);
                goto out;
            }
        }
        reply_mix(CMD_ACK, 0, 0, 0, 0, 0);
out:
        LEDsoff();
        crypto1_deinit(pcs);
        if (foundkeys == allkeys || lastchunk) {
            set_tracing(false);
            FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
            BigBuf_free();
            BigBuf_Clear_ext(false);
        }
        g_dbglevel = oldbg;
        return;
    }


    // keychunk loop - depth first one sector.
    if (strategy == 1 || use_flashmem) {

        uint8_t newfound = foundkeys;

        uint16_t lastpos = 0;
        uint16_t s_point = 0;
        // Sector main loop
        // keep track of how many sectors on card.
        for (uint8_t s = 0; s < sectorcnt; ++s) {

            if (found[(s * 2)] && found[(s * 2) + 1]) {
                continue;
            }

            for (uint16_t i = s_point; i < keyCount; ++i) {

                // Allow button press / usb cmd to interrupt device
                if (BUTTON_PRESS() || data_available()) {
                    goto OUT;
                }

                // found all keys?
                if (foundkeys == allkeys) {
                    goto OUT;
                }

                WDT_HIT();

                // assume: block0,1,2 has more read rights in accessbits than the sectortrailer. authenticating against block0 in each sector
                chk_data.block = FirstBlockOfSector(s);

                // new key
                chk_data.key = bytes_to_num(datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);

                // skip already found A keys
                if (!found[(s * 2)]) {

                    chk_data.keyType = 0;
                    status = chkKey(&chk_data);

                    if (status == 4) {
                        // failed to select,  return immediately
                        goto OUT;
                    }

                    if (status == 0) {
                        memcpy(k_sector[s].keyA, datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);
                        found[(s * 2)] = 1;
                        ++foundkeys;

                        chkKey_scanA(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        // read Block B, if A is found.
                        chkKey_loopBonly(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        chk_data.keyType = 1;
                        chkKey_scanB(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        chk_data.keyType = 0;
                        chk_data.block = FirstBlockOfSector(s);

                        if (use_flashmem) {
                            if (lastpos != i && lastpos != 0) {
                                if (i - lastpos < 0xF) {
                                    s_point = i & 0xFFF0;
                                }
                            } else {
                                lastpos = i;
                            }
                        }
                    }
                }

                // skip already found B keys
                if (!found[(s * 2) + 1]) {

                    chk_data.keyType = 1;
                    status = chkKey(&chk_data);

                    if (status == 4) {
                        // failed to select,  return immediately
                        goto OUT;
                    }

                    if (status == 0) {
                        memcpy(k_sector[s].keyB, datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);
                        found[(s * 2) + 1] = 1;
                        ++foundkeys;

                        chkKey_scanB(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        if (use_flashmem) {
                            if (lastpos != i && lastpos != 0) {

                                if (i - lastpos < 0xF)
                                    s_point = i & 0xFFF0;
                            } else {
                                lastpos = i;
                            }
                        }
                    }
                }

                if (found[(s * 2)] && found[(s * 2) + 1]) {
                    break;
                }

            } // end keys test loop - depth first

            // assume1. if no keys found in first sector, get next keychunk from client
            if (!use_flashmem && (newfound - foundkeys == 0)) {
                goto OUT;
            }

        } // end loop - sector
    } // end strategy 1

    if (foundkeys == allkeys) {
        goto OUT;
    }

    if (strategy == 2 || use_flashmem) {

        // Keychunk loop
        for (uint16_t i = 0; i < keyCount; i++) {

            // Allow button press / usb cmd to interrupt device
            if (BUTTON_PRESS() || data_available()) {
                break;
            }

            // found all keys?
            if (foundkeys == allkeys) {
                goto OUT;
            }

            WDT_HIT();

            // new key
            chk_data.key = bytes_to_num(datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);

            // Sector main loop
            // keep track of how many sectors on card.
            for (uint8_t s = 0; s < sectorcnt; ++s) {

                if (found[(s * 2)] && found[(s * 2) + 1]) {
                    continue;
                }

                // found all keys?
                if (foundkeys == allkeys) {
                    goto OUT;
                }

                // assume: block0,1,2 has more read rights in accessbits than the sectortrailer. authenticating against block0 in each sector
                chk_data.block = FirstBlockOfSector(s);

                // skip already found A keys
                if (found[(s * 2)] == 0) {

                    chk_data.keyType = MF_KEY_A;
                    status = chkKey(&chk_data);
                    if (status == 0) {
                        memcpy(k_sector[s].keyA, datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);
                        found[(s * 2)] = 1;
                        ++foundkeys;

                        chkKey_scanA(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        // read Block B, if A is found.
                        chkKey_loopBonly(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        chk_data.block = FirstBlockOfSector(s);
                    }
                }

                // skip already found B keys
                if (found[(s * 2) + 1] == 0) {

                    chk_data.keyType = MF_KEY_B;
                    status = chkKey(&chk_data);
                    if (status == 0) {
                        memcpy(k_sector[s].keyB, datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);
                        found[(s * 2) + 1] = 1;
                        ++foundkeys;

                        chkKey_scanB(&chk_data, k_sector, found, &sectorcnt, &foundkeys);
                    }
                }
            } // end loop sectors
        } // end loop keys
    } // end loop strategy 2
OUT:
    LEDsoff();

    crypto1_deinit(pcs);

    // All keys found, send to client, or last keychunk from client
    if (foundkeys == allkeys || lastchunk) {

        uint64_t foo = 0;
        for (uint8_t m = 0; m < 64; m++) {
            foo |= ((uint64_t)(found[m] & 1) << m);
        }

        uint16_t bar = 0;
        uint8_t j = 0;
        for (uint8_t m = 64; m < ARRAYLEN(found); m++) {
            bar |= ((uint16_t)(found[m] & 1) << j++);
        }

        uint8_t *tmp = BigBuf_malloc(480 + 10);
        memcpy(tmp, k_sector, sectorcnt * sizeof(sector_t));
        num_to_bytes(foo, 8, tmp + 480);
        tmp[488] = bar & 0xFF;
        tmp[489] = bar >> 8 & 0xFF;

        reply_old(CMD_ACK, foundkeys, 0, 0, tmp, 480 + 10);

        set_tracing(false);
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        BigBuf_free();
        BigBuf_Clear_ext(false);

        // special trick ecfill
        if (use_flashmem && foundkeys == allkeys) {

            uint8_t block[16] = {0};
            for (int i = 0; i < sectorcnt; i++) {

                uint8_t blockno;
                if (i < 32) {
                    blockno = (i * 4) ^ 0x3;
                } else {
                    blockno = (32 * 4 + (i - 32) * 16) ^ 0xF;
                }
                // get ST
                emlGetMem_xt(block, blockno, 1, MIFARE_BLOCK_SIZE);

                memcpy(block, k_sector[i].keyA, MF_KEY_LENGTH);
                memcpy(block + 10, k_sector[i].keyB, MF_KEY_LENGTH);

                emlSetMem_xt(block, blockno, 1, sizeof(block));
            }

            MifareECardLoad(sectorcnt, MF_KEY_A, NULL);
            MifareECardLoad(sectorcnt, MF_KEY_B, NULL);
        }
    } else {
        // partial/none keys found
        reply_mix(CMD_ACK, foundkeys, 0, 0, 0, 0);
    }

    g_dbglevel = oldbg;
}

void MifareChkKeys(uint8_t *datain, uint8_t reserved_mem) {

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    uint8_t uid[10] = {0x00};

    uint64_t key = 0;
    uint32_t cuid = 0;
    uint8_t cascade_levels = 0;
    struct {
        uint8_t key[MF_KEY_LENGTH];
        bool found;
    } PACKED keyresult;
    keyresult.found = false;
    memset(keyresult.key, 0x00, sizeof(keyresult.key));

    bool have_uid = false;

    uint8_t keyType = datain[0];
    uint8_t blockNo = datain[1];
    bool clearTrace = datain[2];
    uint16_t key_count = (datain[3] << 8) | datain[4];

    uint16_t key_mem_available;
    if (reserved_mem)
        key_mem_available = key_count * MF_KEY_LENGTH;
    else
        key_mem_available = MIN((PM3_CMD_DATA_SIZE - 5), (key_count * MF_KEY_LENGTH));

    key_count = (key_mem_available / MF_KEY_LENGTH);

    datain += 5;

    LEDsoff();
    LED_A_ON();

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    if (clearTrace)
        clear_trace();

    int oldbg = g_dbglevel;
    g_dbglevel = DBG_NONE;

    set_tracing(false);

    for (uint16_t i = 0; i < key_count; i++) {

        // Iceman: use piwi's faster nonce collecting part in hardnested.
        if (have_uid == false) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (iso14443a_select_card(uid, &card_info, &cuid, true, 0, true) == false) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("ChkKeys: Can't select card (ALL)");
                --i; // try same key once again
                continue;
            }
            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    break;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card
            if (iso14443a_select_card(uid, NULL, NULL, false, cascade_levels, true) == false) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("ChkKeys: Can't select card (UID)");
                --i; // try same key once again
                continue;
            }
        }

        key = bytes_to_num(datain + i * 6, 6);
        if (mifare_classic_auth(pcs, cuid, blockNo, keyType, key, AUTH_FIRST)) {
//        CHK_TIMEOUT();
            continue;
        }

        memcpy(keyresult.key, datain + (i * MF_KEY_LENGTH), MF_KEY_LENGTH);
        keyresult.found = true;
        break;
    }

    LED_B_ON();
    crypto1_deinit(pcs);

    reply_ng(CMD_HF_MIFARE_CHKKEYS, PM3_SUCCESS, (uint8_t *)&keyresult, sizeof(keyresult));
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
    g_dbglevel = oldbg;
}

void MifareChkKeys_file(uint8_t *fn) {

#ifdef WITH_FLASH
    BigBuf_free();

    SpinOff(0);

    int changed = rdv40_spiffs_lazy_mount();
    uint32_t size = size_in_spiffs((char *)fn);
    uint8_t *mem = BigBuf_malloc(size);

    rdv40_spiffs_read_as_filetype((char *)fn, mem, size, RDV40_SPIFFS_SAFETY_SAFE);

    if (changed) {
        rdv40_spiffs_lazy_unmount();
    }

    SpinOff(0);

    MifareChkKeys(mem, true);

    BigBuf_free();
#endif
}

//-----------------------------------------------------------------------------
// MIFARE Personalize UID. Only for Mifare Classic EV1 7Byte UID
//-----------------------------------------------------------------------------
void MifarePersonalizeUID(uint8_t keyType, uint8_t perso_option, uint64_t key) {

    uint16_t isOK = PM3_EUNDEF;
    uint8_t uid[10] = { 0 };
    uint32_t cuid = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    clear_trace();
    set_tracing(true);

    LED_A_ON();

    uint8_t rec_answer[MAX_MIFARE_FRAME_SIZE] = {0};
    uint8_t rec_answer_par[MAX_MIFARE_PARITY_SIZE] = {0};

    while (true) {

        if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == false) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
            break;
        }

        uint8_t block_number = 0;
        if (mifare_classic_auth(pcs, cuid, block_number, keyType, key, AUTH_FIRST)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Auth error");
            break;
        }

        int len = mifare_sendcmd_short(pcs, true, MIFARE_EV1_PERSONAL_UID, perso_option, rec_answer, sizeof(rec_answer), rec_answer_par, NULL);
        if (len != 1 || rec_answer[0] != CARD_ACK) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Cmd Error: %02x", rec_answer[0]);
            break;
        }

        if (mifare_classic_halt(pcs)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
            break;
        }
        isOK = PM3_SUCCESS;
        break;
    }

    crypto1_deinit(pcs);

    LED_B_ON();
    reply_ng(CMD_HF_MIFARE_PERSONALIZE_UID, isOK, NULL, 0);
    LED_B_OFF();

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
}


//-----------------------------------------------------------------------------
// Load a card into the emulator memory
//
//-----------------------------------------------------------------------------
int MifareECardLoadExt(uint8_t sectorcnt, uint8_t keytype, uint8_t *key) {
    int retval = MifareECardLoad(sectorcnt, keytype, key);
    reply_ng(CMD_HF_MIFARE_EML_LOAD, retval, NULL, 0);
    return retval;
}

int MifareECardLoad(uint8_t sectorcnt, uint8_t keytype, uint8_t *key) {

    if ((keytype > MF_KEY_B) && (key == NULL)) {

        if (g_dbglevel >= DBG_ERROR) {
            Dbprintf("Error, missing key");
        }
        return PM3_EINVARG;
    }

    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    // variables
    bool have_uid = false;
    bool bd_authenticated = false;
    uint8_t cascade_levels = 0;
    uint32_t cuid = 0;
    uint8_t uid[10] = {0x00};
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    int retval = PM3_SUCCESS;

    // increase time-out.  Magic card etc are slow
    uint32_t timeout = iso14a_get_timeout();
    // frame waiting time (FWT) in 1/fc
    uint32_t fwt = 256 * 16 * (1 << 7);
    iso14a_set_timeout(fwt / (8 * 16));

    for (uint8_t s = 0; s < sectorcnt; s++) {

        uint64_t ui64Key = emlGetKey(s, keytype);

        // MIFARE Classic 1K Ev1  ,  MIFARE Classic MINI Ev1
        if (sectorcnt == 18) {
            // MFC 1K EV1, skip sector 16 since its lockdown
            if (s == 16) {
                // unknown sector trailer, keep the keys, set only the AC
                uint8_t st[MIFARE_BLOCK_SIZE] = {0x00};
                emlGetMem_xt(st, FirstBlockOfSector(s) + 3, 1, MIFARE_BLOCK_SIZE);
                memcpy(st + 6, "\x70\xF0\xF8\x69", 4);
                emlSetMem_xt(st, FirstBlockOfSector(s) + 3, 1, MIFARE_BLOCK_SIZE);
                continue;
            }

            // ICEMAN: ugly hack,  we don't want to trigger the partial load message
            // MFC 1K EV1 sector 17 don't use key A.
            // not mention we don't save signatures in our MFC dump files.
            if (s == 17 && keytype == MF_KEY_A) {
                ui64Key = 0x4B791BEA7BCC;
                keytype = 1;
            }
        }

        // use fast select
        if (have_uid == false) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (iso14443a_select_card(uid, &card_info, &cuid, true, 0, true) == 0) {
                if (s == 0) {
                    // first attempt, if no card let's stop directly
                    retval = PM3_EFAILED;
                    if (g_dbglevel >= DBG_ERROR) {
                        Dbprintf("Card not found");
                    }
                    goto out;
                }
                continue;
            }

            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    break;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card

            if (bd_authenticated == false) { // no need to select if bd_authenticated with backdoor
                if (iso14443a_fast_select_card(uid, cascade_levels) == 0) {
                    continue;
                }
            }
        }

        // Auth
        if (keytype > MF_KEY_B) {
            if (bd_authenticated == false) {
                ui64Key = bytes_to_num(key, 6);
                if (mifare_classic_auth(pcs, cuid, 0, keytype, ui64Key, AUTH_FIRST)) {
                    retval = PM3_EFAILED;
                    if (g_dbglevel >= DBG_ERROR) {
                        Dbprintf("Auth error");
                    }
                    goto out;
                }
                bd_authenticated = true;
            }
        } else if (mifare_classic_auth(pcs, cuid, FirstBlockOfSector(s), keytype, ui64Key, AUTH_FIRST)) {

            ui64Key = emlGetKey(s, MF_KEY_B);

            if (mifare_classic_auth(pcs, cuid, FirstBlockOfSector(s), MF_KEY_B, ui64Key, AUTH_FIRST)) {
                retval = PM3_EPARTIAL;
                if (g_dbglevel >= DBG_ERROR) {
                    Dbprintf("Sector %2d - Auth error", s);
                }
                continue;
            }
        }


#define MAX_RETRIES 2

        uint8_t data[MIFARE_BLOCK_SIZE] = {0x00};
        for (uint8_t b = 0; b < NumBlocksPerSector(s); b++) {

            memset(data, 0x00, sizeof(data));
            uint8_t tb = FirstBlockOfSector(s) + b;
            uint8_t r = 0;
            for (; r < MAX_RETRIES; r++) {

                int res = mifare_classic_readblock(pcs, tb, data);
                if (res == 1) {
                    retval |= PM3_EPARTIAL;
                    if (g_dbglevel >= DBG_ERROR) {
                        Dbprintf("Error No rights reading sector %2d block %2d", s, b);
                    }
                    break;
                }
                // retry if wrong len.
                if (res != 0) {
                    continue;
                }

                // No need to copy empty
                if (memcmp(data, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", sizeof(data)) == 0) {
                    break;
                }

                if (IsSectorTrailer(b)) {
                    // sector trailer, keep the keys, set only the AC
                    uint8_t st[MIFARE_BLOCK_SIZE] = {0x00};
                    emlGetMem_xt(st, tb, 1, MIFARE_BLOCK_SIZE);
                    memcpy(st + 6, data + 6, 4);
                    emlSetMem_xt(st,  tb, 1, MIFARE_BLOCK_SIZE);
                } else {
                    emlSetMem_xt(data, tb, 1, MIFARE_BLOCK_SIZE);
                }
                break;
            }

            // if we failed all retries,  notify client
            if (r == MAX_RETRIES) {
                retval |= PM3_EPARTIAL;
            }
        }
    }
    int res;
out:
    res = mifare_classic_halt(pcs);
    (void)res;

    iso14a_set_timeout(timeout);
    crypto1_deinit(pcs);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
    return retval;
}



//-----------------------------------------------------------------------------
// Work with "magic Chinese" card (email him: ouyangweidaxian@live.cn)
//
// PARAMS - workFlags
// bit 0 - need get UID
// bit 1 - need wupC
// bit 2 - need HALT after sequence
// bit 3 - need turn on FPGA before sequence
// bit 4 - need turn off FPGA
// bit 5 - need to set datain instead of issuing USB reply (called via ARM for StandAloneMode14a)
// bit 6 - wipe tag.
// bit 7 - use USCUID/GDM (20/23) magic wakeup
//-----------------------------------------------------------------------------

void MifareCSetBlock(uint32_t arg0, uint32_t arg1, uint8_t *datain) {

    // params
    uint8_t workFlags = arg0;
    uint8_t blockNo = arg1;

    // detect 1a/1b
    bool is1b = false;

    // variables
    bool isOK = false; //assume we will get an error
    uint8_t errormsg = 0x00;
    uint8_t uid[10] = {0x00};
    uint8_t data[18] = {0x00};
    uint32_t cuid = 0;

    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t receivedAnswerPar[MAX_MIFARE_PARITY_SIZE] = {0x00};

    if (workFlags & MAGIC_INIT) {
        LED_A_ON();
        LED_B_OFF();
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
        clear_trace();
        set_tracing(true);
    }

    //loop doesn't loop just breaks out if error
    while (true) {
        // read UID and return to client with write
        if (workFlags & MAGIC_UID) {
            if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == 0) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Can't select card");
                errormsg = MAGIC_UID;
                mifare_classic_halt(NULL);
                break;
            }
            mifare_classic_halt(NULL);
        }

        // wipe tag, fill it with zeros
        if (workFlags & MAGIC_WIPE) {
            ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("wupC1 error");
                errormsg = MAGIC_WIPE;
                break;
            }

            uint32_t old_timeout = iso14a_get_timeout();

            // 2000 ms timeout
            // 13560000 / 1000 / (8 * 16) * timeout
            iso14a_set_timeout(21190);

            ReaderTransmit(wipeC, sizeof(wipeC), NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("wipeC error");
                errormsg = MAGIC_WIPE;
                break;
            }
            iso14a_set_timeout(old_timeout);

            mifare_classic_halt(NULL);
        }

        if (workFlags & MAGIC_GDM_ALT_WUPC) {
            ReaderTransmitBitsPar(wupGDM1, 7, NULL, NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("wupGDM1 error");
                errormsg = MAGIC_WUPC;
                break;
            }

            ReaderTransmit(wupGDM2, sizeof(wupC2), NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("wupGDM2 error");
                errormsg = MAGIC_WUPC;
                break;
            }
        }

        // write block
        if (workFlags & MAGIC_WUPC) {
            ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("wupC1 error");
                errormsg = MAGIC_WUPC;
                break;
            }

            if (!is1b) {
                ReaderTransmit(wupC2, sizeof(wupC2), NULL);
                if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                    if (g_dbglevel >= DBG_INFO) Dbprintf("Assuming Magic Gen 1B tag. [wupC2 failed]");
                    is1b = true;
                    continue;
                }
            }
        }

        // Write signature blocks using GDM write command
        if (blockNo >= MIFARE_1K_MAXBLOCK && blockNo < MIFARE_1K_EV1_MAXBLOCK) {

            blockNo %= 0x40;
            if ((mifare_sendcmd_short(NULL, CRYPT_NONE, MIFARE_MAGIC_GDM_WRITEBLOCK, blockNo, receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar, NULL) != 1) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("Magic write block send command error");
                errormsg = 4;
                break;
            }

        } else {

            if ((mifare_sendcmd_short(NULL, CRYPT_NONE, ISO14443A_CMD_WRITEBLOCK, blockNo, receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar, NULL) != 1) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_INFO) Dbprintf("write block send command error");
                errormsg = 5;
                break;
            }
        }

        memcpy(data, datain, 16);
        AddCrc14A(data, 16);

        ReaderTransmit(data, sizeof(data), NULL);
        if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) != 1) || (receivedAnswer[0] != 0x0a)) {
            if (g_dbglevel >= DBG_INFO) Dbprintf("write block send data error");
            errormsg = 0;
            break;
        }

        if (workFlags & MAGIC_HALT) {
            mifare_classic_halt(NULL);
        }

        isOK = true;
        break;

    } // end while

    if (isOK)
        reply_mix(CMD_ACK, 1, 0, 0, uid, sizeof(uid));
    else
        OnErrorMagic(errormsg);

    if (workFlags & MAGIC_OFF)
        OnSuccessMagic();
}

void MifareCGetBlock(uint32_t arg0, uint32_t arg1, uint8_t *datain) {

    uint8_t workFlags = arg0;
    uint8_t blockNo = arg1;
    uint8_t errormsg = 0x00;
    bool isOK = false; //assume we will get an error

    // detect 1a/1b
    bool is1b = false;

    // variables
    uint8_t data[MAX_MIFARE_FRAME_SIZE];
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t receivedAnswerPar[MAX_MIFARE_PARITY_SIZE] = {0x00};

    memset(data, 0x00, sizeof(data));

    if (workFlags & MAGIC_INIT) {
        LED_A_ON();
        LED_B_OFF();
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
        clear_trace();
        set_tracing(true);
    }

    // increase time-out.  Magic card etc are slow
    uint32_t timeout = iso14a_get_timeout();
    // frame waiting time (FWT) in 1/fc
    uint32_t fwt = 256 * 16 * (1 << 7);
    iso14a_set_timeout(fwt / (8 * 16));

    //loop doesn't loop just breaks out if error or done
    while (true) {
        if (workFlags & MAGIC_GDM_ALT_WUPC) {
            ReaderTransmitBitsPar(wupGDM1, 7, NULL, NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("wupGDM1 error");
                errormsg = MAGIC_WUPC;
                break;
            }

            ReaderTransmit(wupGDM2, sizeof(wupC2), NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("wupGDM2 error");
                errormsg = MAGIC_WUPC;
                break;
            }
        }

        if (workFlags & MAGIC_WUPC) {
            ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
            if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0)  || (receivedAnswer[0] != 0x0a)) {
                if (g_dbglevel >= DBG_ERROR) Dbprintf("wupC1 error");
                errormsg = MAGIC_WUPC;
                break;
            }

            if (is1b == false)  {
                ReaderTransmit(wupC2, sizeof(wupC2), NULL);
                if ((ReaderReceive(receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar) == 0) || (receivedAnswer[0] != 0x0a)) {
                    if (g_dbglevel >= DBG_INFO) Dbprintf("Assuming Magic Gen 1B tag. [wupC2 failed]");
                    is1b = true;
                    continue;
                }
            }
        }

        // read block
        if ((mifare_sendcmd_short(NULL, CRYPT_NONE, ISO14443A_CMD_READBLOCK, blockNo, receivedAnswer, sizeof(receivedAnswer), receivedAnswerPar, NULL) != MIFARE_BLOCK_SIZE + CRC16_SIZE)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("read block send command error");
            errormsg = 0;
            break;
        }

        memcpy(data, receivedAnswer, sizeof(data));

        // send HALT
        if (workFlags & MAGIC_HALT) {
            mifare_classic_halt(NULL);
        }

        isOK = true;
        break;
    }
    // if MAGIC_DATAIN, the data stays on device side.
    if (workFlags & MAGIC_DATAIN) {

        if (isOK) {
            memcpy(datain, data, sizeof(data));
        }

    } else {

        if (isOK) {
            reply_old(CMD_ACK, 1, 0, 0, data, sizeof(data));
        } else {
            OnErrorMagic(errormsg);
        }

    }

    if (workFlags & MAGIC_OFF) {
        OnSuccessMagic();
    }

    iso14a_set_timeout(timeout);
}

static void mf_reset_card(void) {
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    SpinDelay(40);
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
}

void MifareCIdent(bool is_mfc, uint8_t keytype, uint8_t *key) {
    // variables
    uint8_t rec[1] = {0x00};
    uint8_t recpar[1] = {0x00};
    uint8_t rdblf0[4] = {ISO14443A_CMD_READBLOCK, 0xF0, 0x8D, 0x5f};
    uint8_t rdbl00[4] = {ISO14443A_CMD_READBLOCK, 0x00, 0x02, 0xa8};
    uint8_t gen4gdmAuth[4] = {MIFARE_MAGIC_GDM_AUTH_KEY, 0x00, 0x6C, 0x92};
    uint8_t gen4gdmGetConf[4] = {MIFARE_MAGIC_GDM_READ_CFG, 0x00, 0x39, 0xF7};
    uint8_t gen4gdmGetMagicBlock[4] = {MIFARE_MAGIC_GDM_READBLOCK, 0x00, 0xC2, 0x66};
    uint8_t gen4GetConf[8] = {GEN_4GTU_CMD, 0x00, 0x00, 0x00, 0x00, GEN_4GTU_GETCNF, 0, 0};
    uint8_t superGen1[9] = {0x0A, 0x00, 0x00, 0xA6, 0xB0, 0x00, 0x10, 0x14, 0x1D};
    uint8_t uid[10];
    uint8_t *par = BigBuf_calloc(MAX_PARITY_SIZE);
    uint8_t *buf = BigBuf_calloc(PM3_CMD_DATA_SIZE);
    iso14a_card_select_t *card = (iso14a_card_select_t *) BigBuf_calloc(sizeof(iso14a_card_select_t));

    bool isGen2 = false;
    uint16_t flag = MAGIC_FLAG_NONE;
    uint32_t cuid = 0;
    int res = 0;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // Generation 1 test
    ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
    if (ReaderReceive(rec, 1, recpar) && (rec[0] == 0x0A)) {

        flag = MAGIC_FLAG_GEN_1A;
        ReaderTransmit(wupC2, sizeof(wupC2), NULL);
        uint16_t tmp = ReaderReceive(rec, 1, recpar);
        if ((tmp && (rec[0] != 0x0A)) || (tmp == 0)) {
            flag = MAGIC_FLAG_GEN_1B;
        }

        // check for GDM config
        ReaderTransmit(gen4gdmGetConf, sizeof(gen4gdmGetConf), NULL);
        res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
        if (res > 1) {
            // could be ZUID or full USCUID, the magic blocks don't exist on ZUID so
            // a failure here indicates a feature limited chip like ZUID
            // check for GDM hidden block read
            ReaderTransmit(gen4gdmGetMagicBlock, sizeof(gen4gdmGetMagicBlock), NULL);
            res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
            if (res > 1) {
                flag |= MAGIC_FLAG_GDM_WUP_40;
            } else {
                flag |= MAGIC_FLAG_GDM_WUP_40_ZUID;
            }
        }
    }

    mf_reset_card();

    res = iso14443a_select_card(uid, NULL, &cuid, true, 0, true);
    if (res) {
        // Check for Magic Gen4 GTU with default password:
        // Get config should return 30 or 32 bytes
        AddCrc14A(gen4GetConf, sizeof(gen4GetConf) - 2);
        ReaderTransmit(gen4GetConf, sizeof(gen4GetConf), NULL);
        res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
        if (res == 32 || res == 34) {
            flag |= MAGIC_FLAG_GEN_4GTU;
        }
    }

    // reset card
    mf_reset_card();
    // Use special magic detection function that always attempts RATS regardless of SAK
    res = iso14443a_select_card_for_magic(uid, card, &cuid, true, 0);
    if (res) {
        mf_reset_card();
        if (cuid == 0xAA55C396) {
            flag |= MAGIC_FLAG_GEN_UNFUSED;
        }

        if (memcmp(card->ats, "\x09\x78\x00\x91\x02\xDA\xBC\x19\x10", 9) == 0) {
            // test for some MFC gen2
            isGen2 = true;
            flag |= MAGIC_FLAG_GEN_2;
        } else if (memcmp(card->ats, "\x0D\x78\x00\x71\x02\x88\x49\xA1\x30\x20\x15\x06\x08\x56\x3D", 15) == 0) {
            // test for some MFC 7b gen2
            isGen2 = true;
            flag |= MAGIC_FLAG_GEN_2;
        } else if (memcmp(card->ats, "\x0A\x78\x00\x81\x02\xDB\xA0\xC1\x19\x40\x2A\xB5", 12) == 0) {
            // test for Ultralight magic gen2
            isGen2 = true;
            flag |= MAGIC_FLAG_GEN_2;
        } else if (memcmp(card->ats, "\x85\x00\x00\xA0\x00\x00\x0A\xC3\x00\x04\x03\x01\x01\x00\x0B\x03\x41\xDF", 18) == 0) {
            // test for Ultralight EV1 magic gen2
            isGen2 = true;
            flag |= MAGIC_FLAG_GEN_2;
        } else if (memcmp(card->ats, "\x85\x00\x00\xA0\x0A\x00\x0A\xC3\x00\x04\x03\x01\x01\x00\x0B\x03\x16\xD7", 18) == 0) {
            // test for some other Ultralight EV1 magic gen2
            isGen2 = true;
            flag |= MAGIC_FLAG_GEN_2;
        } else if (memcmp(card->ats, "\x85\x00\x00\xA0\x0A\x00\x0A\xB0\x00\x00\x00\x00\x00\x00\x00\x00\x18\x4D", 18) == 0) {
            // test for some other Ultralight magic gen2
            isGen2 = true;
            flag |= MAGIC_FLAG_GEN_2;
        } else if (memcmp(card->ats, "\x85\x00\x00\xA0\x00\x00\x0A\xA5\x00\x04\x04\x02\x01\x00\x0F\x03\x79\x0C", 18) == 0) {
            // test for NTAG213 magic gen2
            isGen2 = true;
            flag |= MAGIC_FLAG_GEN_2;
        }

        // test for super card
        ReaderTransmit(superGen1, sizeof(superGen1), NULL);
        res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
        if (res == 22) {
            uint8_t isGen = MAGIC_FLAG_SUPER_GEN1;

            // check for super card gen2
            // not available after RATS, reset card before executing
            mf_reset_card();

            iso14443a_select_card(uid, NULL, &cuid, true, 0, true);
            ReaderTransmit(rdbl00, sizeof(rdbl00), NULL);
            res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
            if (res == 18) {
                isGen = MAGIC_FLAG_SUPER_GEN2;
            }

            flag |= isGen;
        }
    }

    if (is_mfc == false) {
        // magic ntag test
        mf_reset_card();

        res = iso14443a_select_card(uid, NULL, &cuid, true, 0, true);
        if (res == 2) {
            ReaderTransmit(rdblf0, sizeof(rdblf0), NULL);
            res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
            if (res == 18) {
                flag |= MAGIC_FLAG_NTAG21X;
            }
        }

    } else {

        struct Crypto1State mpcs = {0, 0};
        struct Crypto1State *pcs;
        pcs = &mpcs;

        // CUID (with default sector 0 B key) test
        // regular cards will NAK the WRITEBLOCK(0) command, while DirectWrite will ACK it
        // if we do get an ACK, we immediately abort to ensure nothing is ever actually written
        // only perform test if we haven't already identified Gen2.  No need test if we have a positive identification already
        if (isGen2 == false) {
            mf_reset_card();

            res = iso14443a_select_card(uid, NULL, &cuid, true, 0, true);
            if (res) {

                uint64_t tmpkey = bytes_to_num(key, 6);
                if (mifare_classic_authex(pcs, cuid, 0, keytype, tmpkey, AUTH_FIRST, NULL, NULL) == 0) {

                    if ((mifare_sendcmd_short(pcs, 1, ISO14443A_CMD_WRITEBLOCK, 0, buf, PM3_CMD_DATA_SIZE, par, NULL) == 1) && (buf[0] == 0x0A)) {
                        flag |= MAGIC_FLAG_GEN_2;
                        // turn off immediately to ensure nothing ever accidentally writes to the block
                        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
                    }
                }
                crypto1_deinit(pcs);
            }
        }

        // magic MFC Gen3 test 1
        mf_reset_card();

        res = iso14443a_select_card(uid, NULL, &cuid, true, 0, true);
        if (res) {
            ReaderTransmit(rdbl00, sizeof(rdbl00), NULL);
            res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
            if (res == 18) {
                flag |= MAGIC_FLAG_GEN_3;
            }
        }

        // magic MFC Gen4 GDM magic auth test
        mf_reset_card();

        res = iso14443a_select_card(uid, NULL, &cuid, true, 0, true);
        if (res) {
            ReaderTransmit(gen4gdmAuth, sizeof(gen4gdmAuth), NULL);
            res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
            if (res == 4) {
                flag |= MAGIC_FLAG_GDM_AUTH;
            }
        }

        // QL88 test
        mf_reset_card();

        res = iso14443a_select_card(uid, NULL, &cuid, true, 0, true);
        if (res) {

            if (mifare_classic_authex(pcs, cuid, 68, MF_KEY_B, 0x707B11FC1481, AUTH_FIRST, NULL, NULL) == 0) {
                flag |= MAGIC_FLAG_QL88;
            }
            crypto1_deinit(pcs);
        }
    }

    // GDM alt magic wakeup (20)
    ReaderTransmitBitsPar(wupGDM1, 7, NULL, NULL);
    if (ReaderReceive(rec, 1, recpar) && (rec[0] == 0x0a)) {
        flag |= MAGIC_FLAG_GDM_WUP_20;
    }

    reply_ng(CMD_HF_MIFARE_CIDENT, PM3_SUCCESS, (uint8_t *)&flag, sizeof(uint16_t));
    // turns off
    OnSuccessMagic();
    BigBuf_free();
}

void MifareHasStaticNonce(void) {

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

    // variables
    int retval = PM3_SUCCESS;
    uint32_t nt = 0;
    uint8_t uid[10] = {0};

    uint8_t data[1] = { NONCE_FAIL };
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    uint8_t counter = 0;
    for (uint8_t i = 0; i < 3; i++) {

        if (iso14443a_select_card(uid, NULL, NULL, true, 0, true) == 0) {
            retval = PM3_ESOFT;
            goto OUT;
        }

        uint8_t rec[4] = {0x00};
        uint8_t recpar[1] = {0x00};
        // Transmit MIFARE_CLASSIC_AUTH 0x60, block 0
        int len = mifare_sendcmd_short(pcs, false, MIFARE_AUTH_KEYA, 0, rec, sizeof(rec), recpar, NULL);
        if (len != 4) {
            retval = PM3_ESOFT;
            goto OUT;
        }

        // Save the tag nonce (nt)
        if (nt == bytes_to_num(rec, 4)) {
            counter++;
        }

        nt = bytes_to_num(rec, 4);

        // some cards with static nonce need to be reset before next query
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        LEDsoff();
        CHK_TIMEOUT();

        memset(rec, 0x00, sizeof(rec));
    }

    if (counter) {
        Dbprintf("Static nonce....... " _YELLOW_("%08x"), nt);
        data[0] = NONCE_STATIC;
    } else {
        data[0] = NONCE_NORMAL;
    }

OUT:
    reply_ng(CMD_HF_MIFARE_STATIC_NONCE, retval, data, sizeof(data));
    // turns off
    OnSuccessMagic();
    BigBuf_free();
    crypto1_deinit(pcs);
}

// FUDAN card w static encrypted nonces
// 2B F9 1C 1B D5 08 48 48 03 A4 B1 B1 75 FF 2D 90
//                         ^^                   ^^

void MifareHasStaticEncryptedNonce(uint8_t block_no, uint8_t key_type, uint8_t *key, uint8_t block_no_nested, uint8_t key_type_nested, uint8_t *key_nested, uint8_t nr_nested, bool reset, bool hardreset, bool addread, bool addauth, bool incblk2, bool corruptnrar, bool corruptnrarparity) {
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

    LEDsoff();
    clear_trace();
    set_tracing(true);

    int retval = PM3_SUCCESS;

    uint8_t data[14] = { NONCE_FAIL };

    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    uint8_t first_nt_counter = 0;
    uint8_t first_nt_repetition_counter = 0;
    uint8_t nested_nt_session_counter = 0;
    uint8_t nested_nt_repetition_counter = 0;
    uint8_t first_and_nested_nt_repetition_counter = 0;
    uint8_t key_auth_cmd = MIFARE_AUTH_KEYA + key_type;
    uint8_t key_auth_cmd_nested = MIFARE_AUTH_KEYA + key_type_nested;
    uint64_t ui64key = bytes_to_num(key, 6);
    uint64_t ui64key_nested = bytes_to_num(key_nested, 6);
    uint32_t oldntenc = 0;
    bool need_first_auth = true;
    uint32_t cuid = 0;
    uint32_t nt = 0;
    uint32_t old_nt = 0;
    uint32_t nt_first = 0;
    uint32_t old_nt_first = 0;
    uint32_t ntenc = 0;
    uint8_t ntencpar = 0;
    bool is_last_auth_first_auth = true;

    if (nr_nested == 0) {
        cuid = 0;
        if (iso14443a_select_card(NULL, NULL, &cuid, true, 0, true) == false) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Select error");
            retval = PM3_ESOFT;
            goto OUT;
        }
        if (mifare_classic_authex_cmd(pcs, cuid, block_no, key_auth_cmd, ui64key, AUTH_FIRST, &nt, NULL, NULL, NULL, corruptnrar, corruptnrarparity)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Auth error");
            retval = PM3_ESOFT;
            goto OUT;
        };
        first_nt_counter++;

    } else {

        for (uint8_t i = 0; i < nr_nested; i++) {

            if (need_first_auth) {
                cuid = 0;

                if (hardreset) {
                    if (g_dbglevel >= DBG_EXTENDED) {
                        Dbprintf("RF reset");
                    }
                    // some cards need longer than mf_reset_card() to see effect on nT
                    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
                    SpinDelay(150);
                    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
                }

                if (g_dbglevel >= DBG_EXTENDED) {
                    Dbprintf("select");
                }

                if (iso14443a_select_card(NULL, NULL, &cuid, true, 0, true) == false) {
                    retval = PM3_ESOFT;
                    goto OUT;
                }

                if (mifare_classic_authex_cmd(pcs, cuid, block_no, key_auth_cmd, ui64key, AUTH_FIRST, &nt_first, NULL, NULL, NULL, corruptnrar, corruptnrarparity)) {
                    if (g_dbglevel >= DBG_ERROR) Dbprintf("Auth error");
                    retval = PM3_ESOFT;
                    goto OUT;
                };

                is_last_auth_first_auth = true;
                first_nt_counter++;
                if ((first_nt_counter > 1) && (old_nt_first == nt_first)) {
                    first_nt_repetition_counter++;
                }

                old_nt_first = nt_first;
                if (!reset && !hardreset) {
                    need_first_auth = false;
                }

                if (addread) {
                    uint8_t dataread[16] = {0x00};
                    mifare_classic_readblock(pcs, block_no, dataread);
                }

                if (addauth) {

                    if (mifare_classic_authex_cmd(pcs, cuid, block_no, key_auth_cmd, ui64key, AUTH_NESTED, &nt, NULL, NULL, NULL, false, false)) {

                        if (g_dbglevel >= DBG_ERROR) Dbprintf("Auth error");
                        retval = PM3_ESOFT;
                        goto OUT;

                    } else if (g_dbglevel >= DBG_EXTENDED) {
                        Dbprintf("Nonce distance: %5i (first nonce <> nested nonce)", nonce_distance(nt_first, nt));
                    }

                    is_last_auth_first_auth = false;
                    if (nt == nt_first) {
                        first_and_nested_nt_repetition_counter++;
                    }

                    old_nt = nt;
                }
            }

            nt = 0;
            ntenc = 0;
            if (mifare_classic_authex_cmd(pcs, cuid, incblk2 ? block_no_nested + (i * 4) : block_no_nested, key_auth_cmd_nested, ui64key_nested, AUTH_NESTED, &nt, &ntenc, &ntencpar, NULL, false, false)) {

                if (g_dbglevel >= DBG_ERROR) Dbprintf("Nested auth error");
                need_first_auth = true;

            } else if (g_dbglevel >= DBG_EXTENDED) {

                if (is_last_auth_first_auth) {
                    Dbprintf("Nonce distance: %5i (first nonce <> nested nonce)", nonce_distance(nt_first, nt));
                } else {
                    Dbprintf("Nonce distance: %5i", nonce_distance(old_nt, nt));
                }
            }

            nested_nt_session_counter++;
            is_last_auth_first_auth = false;
            old_nt = nt;
            if (nt == nt_first) {
                first_and_nested_nt_repetition_counter++;
            }

            if ((nested_nt_session_counter > 1) && (oldntenc == ntenc)) {
                nested_nt_repetition_counter++;
            }
            oldntenc = ntenc;
        }
    }

    data[1] = (cuid >> 24) & 0xFF;
    data[2] = (cuid >> 16) & 0xFF;
    data[3] = (cuid >> 8) & 0xFF;
    data[4] = (cuid >> 0) & 0xFF;

    if (first_and_nested_nt_repetition_counter) {
        data[0] = NONCE_SUPERSTATIC;
        data[5] = (nt >> 24) & 0xFF;
        data[6] = (nt >> 16) & 0xFF;
        data[7] = (nt >> 8) & 0xFF;
        data[8] = (nt >> 0) & 0xFF;

    } else if (first_nt_repetition_counter) {
        data[0] = NONCE_STATIC;
        data[5] = (nt_first >> 24) & 0xFF;
        data[6] = (nt_first >> 16) & 0xFF;
        data[7] = (nt_first >> 8) & 0xFF;
        data[8] = (nt_first >> 0) & 0xFF;

    } else if (nested_nt_repetition_counter) {
        data[0] = NONCE_STATIC_ENC;
        data[5] = (nt >> 24) & 0xFF;
        data[6] = (nt >> 16) & 0xFF;
        data[7] = (nt >> 8) & 0xFF;
        data[8] = (nt >> 0) & 0xFF;
        data[9] = (ntenc >> 24) & 0xFF;
        data[10] = (ntenc >> 16) & 0xFF;
        data[11] = (ntenc >> 8) & 0xFF;
        data[12] = (ntenc >> 0) & 0xFF;
        data[13] = ntencpar;

    } else {
        data[0] = NONCE_NORMAL;
        data[5] = (nt >> 24) & 0xFF;
        data[6] = (nt >> 16) & 0xFF;
        data[7] = (nt >> 8) & 0xFF;
        data[8] = (nt >> 0) & 0xFF;
        data[9] = (ntenc >> 24) & 0xFF;
        data[10] = (ntenc >> 16) & 0xFF;
        data[11] = (ntenc >> 8) & 0xFF;
        data[12] = (ntenc >> 0) & 0xFF;
        data[13] = ntencpar;
    }

OUT:
    crypto1_deinit(pcs);

    reply_ng(CMD_HF_MIFARE_STATIC_ENCRYPTED_NONCE, retval, data, sizeof(data));
    // turns off
    OnSuccessMagic();
    BigBuf_free();
}

void OnSuccessMagic(void) {
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

void OnErrorMagic(uint8_t reason) {
    //          ACK, ISOK, reason,0,0,0
    reply_mix(CMD_ACK, 0, reason, 0, 0, 0);
    OnSuccessMagic();
}

int DoGen3Cmd(uint8_t *cmd, uint8_t cmd_len) {
    int retval = PM3_SUCCESS;
    uint8_t *par = BigBuf_calloc(MAX_PARITY_SIZE);
    uint8_t *buf = BigBuf_calloc(PM3_CMD_DATA_SIZE);

    LED_B_ON();
    uint32_t save_iso14a_timeout = iso14a_get_timeout();
    iso14a_set_timeout(13560000 / 1000 / (8 * 16) * 2000); // 2 seconds timeout

    ReaderTransmit(cmd, cmd_len, NULL);
    int res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);
    if (res == 4 && memcmp(buf, "\x90\x00\xfd\x07", 4) == 0) {
        // timeout for card memory reset
        SpinDelay(1000);
    } else {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Card operation not completed");
        retval = PM3_ESOFT;
    }
    iso14a_set_timeout(save_iso14a_timeout);
    LED_B_OFF();
    return retval;
}

void MifareGen3UID(uint8_t uidlen, uint8_t *uid) {
    int retval = PM3_SUCCESS;
    uint8_t uid_cmd[5] = { 0x90, 0xfb, 0xcc, 0xcc, 0x07 };
    uint8_t *old_uid = BigBuf_calloc(10);
    uint8_t *cmd = BigBuf_calloc(sizeof(uid_cmd) + uidlen + 2);
    iso14a_card_select_t *card_info = (iso14a_card_select_t *) BigBuf_calloc(sizeof(iso14a_card_select_t));

    LEDsoff();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    clear_trace();
    set_tracing(true);

    if (iso14443a_select_card(old_uid, card_info, NULL, true, 0, true) == false) {
        retval = PM3_ESOFT;
        goto OUT;
    }
    if (card_info->uidlen != uidlen) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Wrong UID length");
        retval = PM3_ESOFT;
        goto OUT;
    }

    memcpy(cmd, uid_cmd, sizeof(uid_cmd));
    memcpy(&cmd[sizeof(uid_cmd)], uid, uidlen);
    AddCrc14A(cmd, sizeof(uid_cmd) + uidlen);

    retval = DoGen3Cmd(cmd, sizeof(uid_cmd) + uidlen + 2);

OUT:
    reply_ng(CMD_HF_MIFARE_GEN3UID, retval, old_uid, uidlen);
    // turns off
    OnSuccessMagic();
    BigBuf_free();
}

void MifareGen3Blk(uint8_t block_len, uint8_t *block) {

    int retval = PM3_SUCCESS;
    uint8_t block_cmd[5] = { 0x90, 0xf0, 0xcc, 0xcc, 0x10 };
    uint8_t cmdlen = sizeof(block_cmd) + MIFARE_BLOCK_SIZE + CRC16_SIZE;
    uint8_t *cmd = BigBuf_calloc(cmdlen);

    iso14a_card_select_t *card_info = (iso14a_card_select_t *) BigBuf_calloc(sizeof(iso14a_card_select_t));

    LEDsoff();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    clear_trace();
    set_tracing(true);

    if (iso14443a_select_card(NULL, card_info, NULL, true, 0, true) == false) {
        retval = PM3_ESOFT;
        goto OUT;
    }

    bool doReselect = false;
    if (block_len < MIFARE_BLOCK_SIZE) {
        if ((mifare_sendcmd_short(NULL, CRYPT_NONE, ISO14443A_CMD_READBLOCK, 0, &cmd[sizeof(block_cmd)], MIFARE_BLOCK_SIZE + CRC16_SIZE, NULL, NULL) != MIFARE_BLOCK_SIZE + CRC16_SIZE)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Read manufacturer block failed");
            retval = PM3_ESOFT;
            goto OUT;
        }
        doReselect = true;
    }

    if (block_len > 0) {
        memcpy(cmd, block_cmd, sizeof(block_cmd));
        memcpy(&cmd[sizeof(block_cmd)], block, block_len);
        int ofs = sizeof(block_cmd);
        if (card_info->uidlen == 4) {
            cmd[ofs + 4] = cmd[ofs + 0] ^ cmd[ofs + 1] ^ cmd[ofs + 2] ^ cmd[ofs + 3];
            ofs += 5;
        } else if (card_info->uidlen == 7) {
            ofs += 7;
        } else {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Wrong Card UID length");
            retval = PM3_ESOFT;
            goto OUT;
        }
        cmd[ofs] = block_len <= card_info->uidlen ? card_info->sak : cmd[ofs];
        ofs++;
        cmd[ofs++] = card_info->atqa[0];
        cmd[ofs++] = card_info->atqa[1];
        AddCrc14A(cmd, sizeof(block_cmd) + MIFARE_BLOCK_SIZE);

        if (doReselect) {
            if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == 0) {
                retval = PM3_ESOFT;
                goto OUT;
            }
        }

        retval = DoGen3Cmd(cmd, sizeof(block_cmd) + MIFARE_BLOCK_SIZE + CRC16_SIZE);
    }

OUT:
    reply_ng(CMD_HF_MIFARE_GEN3BLK, retval, &cmd[sizeof(block_cmd)], MIFARE_BLOCK_SIZE);
    // turns off
    OnSuccessMagic();
    BigBuf_free();
}

void MifareGen3Freez(void) {

    LEDsoff();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    clear_trace();
    set_tracing(true);

    int retval = PM3_SUCCESS;
    uint8_t freeze_cmd[7] = { 0x90, 0xfd, 0x11, 0x11, 0x00, 0xe7, 0x91 };

    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == false) {
        retval = PM3_ESOFT;
        goto OUT;
    }

    retval = DoGen3Cmd(freeze_cmd, sizeof(freeze_cmd));

OUT:
    reply_ng(CMD_HF_MIFARE_GEN3FREEZ, retval, NULL, 0);
    // turns off
    OnSuccessMagic();
    BigBuf_free();
}

void MifareG4ReadBlk(uint8_t blockno, uint8_t *pwd, uint8_t workFlags) {
    bool setup = ((workFlags & MAGIC_INIT) == MAGIC_INIT) ;
    bool done = ((workFlags & MAGIC_OFF)  == MAGIC_OFF) ;

    int res = 0;
    int retval = PM3_SUCCESS;

    uint8_t *buf = BigBuf_malloc(PM3_CMD_DATA_SIZE);
    if (buf == NULL) {
        retval = PM3_EMALLOC;
        goto OUT;
    }

    uint8_t *par = BigBuf_malloc(MAX_PARITY_SIZE);
    if (par == NULL) {
        retval = PM3_EMALLOC;
        goto OUT;
    }

    if (setup) {
        LEDsoff();
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
        clear_trace();
        set_tracing(true);

        if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == false) {
            retval = PM3_ESOFT;
            goto OUT;
        }
    }

    LED_B_ON();

    static uint32_t save_iso14a_timeout;
    if (setup) {
        save_iso14a_timeout = iso14a_get_timeout();
        iso14a_set_timeout(13560000 / 1000 / (8 * 16) * 1000); // 2 seconds timeout
    }

    uint8_t cmd[] = { GEN_4GTU_CMD, 0x00, 0x00, 0x00, 0x00, GEN_4GTU_READ, blockno,
                      0x00, 0x00
                    };

    memcpy(cmd + 1, pwd, 4);

    AddCrc14A(cmd, sizeof(cmd) - 2);

    ReaderTransmit(cmd, sizeof(cmd), NULL);
    res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);

    if (res != 18) {
        retval = PM3_ESOFT;
    }

    if (done || retval != 0) {
        iso14a_set_timeout(save_iso14a_timeout);
    }

    LED_B_OFF();

OUT:
    reply_ng(CMD_HF_MIFARE_G4_RDBL, retval, buf, res);

    // turns off
    if (done || retval != 0) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    }

    LEDsoff();
    if (done || retval != 0) {
        set_tracing(false);
    }

    BigBuf_free();
}

void MifareG4WriteBlk(uint8_t blockno, uint8_t *pwd, uint8_t *data, uint8_t workFlags) {
    bool setup = ((workFlags & MAGIC_INIT) == MAGIC_INIT) ;
    bool done = ((workFlags & MAGIC_OFF)  == MAGIC_OFF) ;

    int res = 0;
    int retval = PM3_SUCCESS;

    uint8_t *buf = BigBuf_malloc(PM3_CMD_DATA_SIZE);
    if (buf == NULL) {
        retval = PM3_EMALLOC;
        goto OUT;
    }

    // check args
    if (data == NULL) {
        retval = PM3_EINVARG;
        goto OUT;
    }

    uint8_t *par = BigBuf_malloc(MAX_PARITY_SIZE);
    if (par == NULL) {
        retval = PM3_EMALLOC;
        goto OUT;
    }

    if (setup) {
        LEDsoff();
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
        clear_trace();
        set_tracing(true);

        if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == false) {
            retval = PM3_ESOFT;
            goto OUT;
        }
    }

    LED_B_ON();

    static uint32_t save_iso14a_timeout;
    if (setup) {
        save_iso14a_timeout = iso14a_get_timeout();
        iso14a_set_timeout(13560000 / 1000 / (8 * 16) * 1000); // 2 seconds timeout
    }

    uint8_t cmd[] = { GEN_4GTU_CMD, 0x00, 0x00, 0x00, 0x00, GEN_4GTU_WRITE, blockno,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00
                    };

    memcpy(cmd + 1, pwd, 4);
    memcpy(cmd + 7, data, 16);

    AddCrc14A(cmd, sizeof(cmd) - 2);

    ReaderTransmit(cmd, sizeof(cmd), NULL);
    res = ReaderReceive(buf, PM3_CMD_DATA_SIZE, par);

    if ((res != 4) || (memcmp(buf, "\x90\x00\xfd\x07", 4) != 0)) {
        retval = PM3_ESOFT;
    }

    if (done || retval != 0) {
        iso14a_set_timeout(save_iso14a_timeout);
    }
    LED_B_OFF();

OUT:
    reply_ng(CMD_HF_MIFARE_G4_WRBL, retval, buf, res);
    // turns off
    if (done || retval != 0) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    }

    LEDsoff();
    if (done || retval != 0) {
        set_tracing(false);
    }

    BigBuf_free();
}

void MifareSetMod(uint8_t *datain) {

    uint8_t mod = datain[0];
    uint64_t ui64Key = bytes_to_num(datain + 1, 6);

    // variables
    uint16_t isOK = PM3_EUNDEF;
    uint8_t *uid = BigBuf_calloc(10);

    uint32_t cuid = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs = &mpcs;

    uint8_t *buf = BigBuf_calloc(MAX_MIFARE_FRAME_SIZE);
    uint8_t *par = BigBuf_calloc(MAX_MIFARE_PARITY_SIZE);

    LEDsoff();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    LED_A_ON();

    while (true) {
        if (iso14443a_select_card(uid, NULL, &cuid, true, 0, true) == false) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
            break;
        }

        if (mifare_classic_auth(pcs, cuid, 0, 0, ui64Key, AUTH_FIRST)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Auth error");
            break;
        }

        int respLen;
        if (((respLen = mifare_sendcmd_short(pcs, CRYPT_ALL, MIFARE_EV1_SETMOD, mod, buf, MAX_MIFARE_FRAME_SIZE, par, NULL)) != 1) || (buf[0] != 0x0a)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("SetMod error; response[0]: %hhX, len: %d", buf[0], respLen);
            break;
        }

        if (mifare_classic_halt(pcs)) {
            if (g_dbglevel >= DBG_ERROR) Dbprintf("Halt error");
            break;
        }

        isOK = PM3_SUCCESS;
        break;
    }

    crypto1_deinit(pcs);

    LED_B_ON();
    reply_ng(CMD_HF_MIFARE_SETMOD, isOK, NULL, 0);
    LED_B_OFF();

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    BigBuf_free();
}

//
// DESFIRE
//
void Mifare_DES_Auth1(uint8_t arg0, uint8_t *datain) {
    uint8_t dataout[12] = {0x00};
    uint32_t cuid = 0;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    clear_trace();
    set_tracing(true);

    int len = iso14443a_select_card(NULL, NULL, &cuid, true, 0, false);
    if (!len) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(1);
        return;
    };

    if (mifare_desfire_des_auth1(cuid, dataout) != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Authentication part1: Fail.");
        OnError(4);
        return;
    }

    if (g_dbglevel >= DBG_EXTENDED) DbpString("AUTH 1 FINISHED");
    reply_mix(CMD_ACK, 1, cuid, 0, dataout, sizeof(dataout));
}

void Mifare_DES_Auth2(uint32_t arg0, uint8_t *datain) {
    uint32_t cuid = arg0;
    uint8_t key[16] = {0x00};
    uint8_t dataout[12] = {0x00};
    uint8_t isOK = 0;

    memcpy(key, datain, 16);

    isOK = mifare_desfire_des_auth2(cuid, key, dataout);

    if (isOK != PM3_SUCCESS) {
        if (g_dbglevel >= DBG_EXTENDED) Dbprintf("Authentication part2: Failed");
        OnError(4);
        return;
    }

    if (g_dbglevel >= DBG_EXTENDED) DbpString("AUTH 2 FINISHED");

    reply_old(CMD_ACK, isOK, 0, 0, dataout, sizeof(dataout));
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

//
// Tear-off attack against MFU.
// - Moebius et al
void MifareU_Otp_Tearoff(uint8_t blno, uint32_t tearoff_time, uint8_t *data_testwrite) {
    uint8_t blockNo = blno;

    if (g_dbglevel >= DBG_DEBUG) DbpString("Preparing OTP tear-off");

    if (tearoff_time > 43000) {
        tearoff_time = 43000;
    }

    g_tearoff_delay_us = tearoff_time;
    g_tearoff_enabled = true;

    LEDsoff();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    clear_trace();
    set_tracing(true);

    // write cmd to send, include CRC
    // 1b write, 1b block, 4b data, 2 crc
    uint8_t cmd[] = {
        MIFARE_ULC_WRITE, blockNo,
        data_testwrite[0], data_testwrite[1], data_testwrite[2], data_testwrite[3],
        0, 0
    };
    AddCrc14A(cmd, sizeof(cmd) - 2);

    // anticollision / select card
    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == false) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(1);
        reply_ng(CMD_HF_MFU_OTP_TEAROFF, PM3_EFAILED, NULL, 0);
        return;
    };
    // send
    LED_D_ON();
    ReaderTransmit(cmd, sizeof(cmd), NULL);
    tearoff_hook();
    reply_ng(CMD_HF_MFU_OTP_TEAROFF, PM3_SUCCESS, NULL, 0);
}

//
// Tear-off attack against MFU counter
void MifareU_Counter_Tearoff(uint8_t counter, uint32_t tearoff_time, uint8_t *datain) {

    if (tearoff_time > 43000) {
        tearoff_time = 43000;
    }

    LEDsoff();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    clear_trace();
    set_tracing(true);

    // Send MFU counter increase cmd
    uint8_t cmd[] = {
        MIFARE_ULEV1_INCR_CNT,
        counter,
        datain[0],  // lsb
        datain[1],
        datain[2],  // msb
        datain[3],  // rfu
        0,
        0,
    };
    AddCrc14A(cmd, sizeof(cmd) - 2);

    // anticollision / select card
    if (iso14443a_select_card(NULL, NULL, NULL, true, 0, true) == false) {
        if (g_dbglevel >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(1);
        switch_off();
        LEDsoff();
        return;
    };

    // send
    ReaderTransmit(cmd, sizeof(cmd), NULL);
    LED_D_ON();
    SpinDelayUsPrecision(tearoff_time);
    switch_off();
    LEDsoff();
    reply_ng(CMD_HF_MFU_COUNTER_TEAROFF, PM3_SUCCESS, NULL, 0);
}
