#include "DS2431.h"

DS2431::DS2431(uint8_t ID1, uint8_t ID2, uint8_t ID3, uint8_t ID4, uint8_t ID5, uint8_t ID6, uint8_t ID7) : OneWireItem(ID1, ID2, ID3, ID4, ID5, ID6, ID7)
{
    static_assert(sizeof(scratchpad) < 256, "Implementation does not cover the whole address-space");
    static_assert(sizeof(memory) < 256,  "Implementation does not cover the whole address-space");

    memset(&scratchpad[0], static_cast<uint8_t>(0x00), sizeof(scratchpad));

    page_protection = 0;
    page_eprom_mode = 0;

    checkMemory();
};

void DS2431::duty(OneWireHub *hub)
{
    constexpr uint8_t ALTERNATING_10 = 0xAA;
    static uint16_t reg_TA = 0; // contains TA1, TA2
    static uint8_t  reg_ES = 0; // E/S register
    uint16_t crc = 0;

    uint8_t  page_offset = 0, cmd, b;
    if (hub->recv(&cmd,1,crc))  return;

    switch (cmd)
    {
        case 0x0F:      // WRITE SCRATCHPAD COMMAND
            if (hub->recv(reinterpret_cast<uint8_t *>(&reg_TA),2,crc))  return;
            reg_ES = reinterpret_cast<uint8_t *>(&reg_TA)[0] & uint8_t(0b00000111); // TODO: when not zero we should issue reg_ES |= 0b00100000; (datasheet not clear)

            // up to 8 bytes of data
            page_offset = reg_ES;
            for (uint8_t i = page_offset; i < SCRATCHPAD_SIZE; ++i)
            {
                if (hub->recv(&scratchpad[i], 1, crc)) break; // can not return here, have to write data
                if (reg_ES < 7) reg_ES++;
            };

            if (!hub->getError())  // try to send crc if wanted
            {
                crc = ~crc; // normally crc16 is sent ~inverted
                hub->send(reinterpret_cast<uint8_t *>(&crc), 2);
            };

            if (reg_TA < 128) // check if page is protected or in eprom-mode
            {
                const uint8_t position = uint8_t(reg_TA) & uint8_t(0b11111000);
                if (checkProtection(reinterpret_cast<uint8_t *>(&reg_TA)[0]))       // protected: load memory-segment to scratchpad
                {
                    for (uint8_t i = 0; i < SCRATCHPAD_SIZE; ++i) scratchpad[i] = memory[position + i];
                }
                else if (checkEpromMode(reinterpret_cast<uint8_t *>(&reg_TA)[0]))   // eprom: logical AND of memory and data, TODO: there is somehow a bug here, protection works but EPROM-Mode not (CRC-Error)
                {
                    for (uint8_t i = page_offset; i < SCRATCHPAD_SIZE; ++i) scratchpad[i] &= memory[position + i];
                };
            };
            break;

        case 0xAA:      // READ SCRATCHPAD COMMAND
            if (hub->send(reinterpret_cast<uint8_t *>(&reg_TA),2,crc))  return;
            if (hub->send(&reg_ES,1,crc)) return;


            {   // send Scratchpad content
                const uint8_t start  = reinterpret_cast<uint8_t *>(&reg_TA)[0] & uint8_t(0x03);
                const uint8_t length = (reg_ES & uint8_t(0x03))+ uint8_t(1) - start;
                if (hub->send(&scratchpad[start],length,crc)) return;
            }

            crc = ~crc;
            if (hub->send(reinterpret_cast<uint8_t *>(&crc),2)) return;
            break; // send 1s when read is complete, is passive, so do nothing

        case 0x55:      // COPY SCRATCHPAD COMMAND
            if (hub->recv(&b))                                  return;
            if (b != reinterpret_cast<uint8_t *>(&reg_TA)[0])   break;
            if (hub->recv(&b))                                  return;
            if (b != reinterpret_cast<uint8_t *>(&reg_TA)[1])   break;
            if (hub->recv(&b))                                  return;
            if (b != reg_ES)                                    return; // Auth code must match

            if (reg_ES & 0b00100000)    return; // writing failed before

            reg_TA &= ~uint16_t(0b00000111);

            // Write Scratchpad
            writeMemory(scratchpad, SCRATCHPAD_SIZE, reinterpret_cast<uint8_t *>(&reg_TA)[0]); // checks if copy protected

            // set the auth code uppermost bit, AA
            reg_ES |= 0b10000000;
            delayMicroseconds(10000); // writing takes so long

            do
            {
                hub->sendBit(true);
            }
            while (hub->clearError() == Error::READ_TIMESLOT_TIMEOUT_HIGH);

            while (true) // send 1s when alternating 1 & 0 after copy is complete
            {
                if (hub->send(&ALTERNATING_10)) return;
            };

        case 0xF0:      // READ MEMORY COMMAND
            if (hub->recv(reinterpret_cast<uint8_t *>(&reg_TA),2))  return;
            if (reg_TA >= sizeof(memory)) return;
            if (hub->send(&memory[reg_TA],sizeof(memory) - reg_TA,crc)) return;
            break; // send 1s when read is complete, is passive, so do nothing here

        default:
            hub->raiseSlaveError(cmd);
    };
};

bool DS2431::checkProtection(const uint8_t position)
{
    // should be an accurate model of the control bytes
    if      (position <  32)
    {
        if (page_protection & 1) return true;
    }
    else if (position <  64)
    {
        if (page_protection & 2) return true;
    }
    else if (position <  96)
    {
        if (page_protection & 4) return true;
    }
    else if (position < 128)
    {
        if (page_protection & 8) return true;
    }
    else if (position == 0x80)
    {
        if ((page_protection & (1 + 16)) || (page_eprom_mode & 1)) return true;
    }
    else if (position == 0x81)
    {
        if ((page_protection & (2 + 16)) || (page_eprom_mode & 2)) return true;
    }
    else if (position == 0x82)
    {
        if ((page_protection & (4 + 16)) || (page_eprom_mode & 4)) return true;
    }
    else if (position == 0x83)
    {
        if ((page_protection & (8 + 16)) || (page_eprom_mode & 8)) return true;
    }
    else if (position == 0x85)
    {
        if (page_protection & (32+16)) return true;
    }
    else if ((position == 0x86) || (position == 0x87))
    {
        if (page_protection & (64+16)) return true;
    }
    else if (position > 127) // filter the rest
    {
        if (page_protection & 16) return true;
    };
    return false;
};

bool DS2431::checkEpromMode(const uint8_t position)
{
    if      (position <  32)
    {
        if (page_eprom_mode & 1) return true;
    }
    else if (position <  64)
    {
        if (page_eprom_mode & 2) return true;
    }
    else if (position <  96)
    {
        if (page_eprom_mode & 4) return true;
    }
    else if (position < 128)
    {
        if (page_eprom_mode & 8) return true;
    };
    return false;
};


void DS2431::clearMemory(void)
{
    memset(&memory[0], static_cast<uint8_t>(0x00), sizeof(memory));
};

bool DS2431::writeMemory(const uint8_t* source, const uint8_t length, const uint8_t position)
{
    for (uint8_t i = 0; i < length; ++i) {
        if ((position + i) >= sizeof(memory)) break;
        if (checkProtection(position+i)) continue;
        memory[position + i] = source[i];
    };

    if ((position+length) > 127) checkMemory();

    return true;
};

bool DS2431::checkMemory(void)
{
    constexpr uint8_t WPM = 0x55; // write protect mode
    constexpr uint8_t EPM = 0xAA; // eprom mode

    page_eprom_mode = 0;
    page_protection = 0;

    if (memory[0x80] == WPM) page_protection |= 1;
    if (memory[0x81] == WPM) page_protection |= 2;
    if (memory[0x82] == WPM) page_protection |= 4;
    if (memory[0x83] == WPM) page_protection |= 8;

    if (memory[0x84] == WPM) page_protection |= 16;
    if (memory[0x84] == EPM) page_protection |= 16;

    if (memory[0x85] == WPM) page_protection |= 32; // only byte x85
    if (memory[0x85] == EPM) page_protection |= 64+32; // also byte x86 x87

    if (memory[0x80] == EPM) page_eprom_mode |= 1;
    if (memory[0x81] == EPM) page_eprom_mode |= 2;
    if (memory[0x82] == EPM) page_eprom_mode |= 4;
    if (memory[0x83] == EPM) page_eprom_mode |= 8;
    return true;
};
