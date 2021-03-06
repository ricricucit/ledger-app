#include "os.h"
#include "cx.h"
#include <stdbool.h>
#include <stdlib.h>
#include "utils.h"
#include "menu.h"

#define SPEND_TRANSACTION_PREFIX 12
#define ACCOUNT_ADDRESS_PREFIX 1

static const char BASE_58_ALPHABET[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', 'G',
                                        'H', 'J', 'K', 'L', 'M', 'N', 'P', 'Q',
                                        'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g',
                                        'h', 'i', 'j', 'k', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                        'w', 'x', 'y', 'z'};

static unsigned char encodeBase58(unsigned char WIDE *in, unsigned char length,
                           unsigned char *out, unsigned char maxoutlen) {
    unsigned char tmp[164];
    unsigned char buffer[164];
    unsigned char j;
    unsigned char startAt;
    unsigned char zeroCount = 0;
    if (length > sizeof(tmp)) {
        THROW(INVALID_PARAMETER);
    }
    os_memmove(tmp, in, length);
    while ((zeroCount < length) && (tmp[zeroCount] == 0)) {
        ++zeroCount;
    }
    j = 2 * length;
    startAt = zeroCount;
    while (startAt < length) {
        unsigned short remainder = 0;
        unsigned char divLoop;
        for (divLoop = startAt; divLoop < length; divLoop++) {
            unsigned short digit256 = (unsigned short)(tmp[divLoop] & 0xff);
            unsigned short tmpDiv = remainder * 256 + digit256;
            tmp[divLoop] = (unsigned char)(tmpDiv / 58);
            remainder = (tmpDiv % 58);
        }
        if (tmp[startAt] == 0) {
            ++startAt;
        }
        buffer[--j] = (unsigned char)BASE_58_ALPHABET[remainder];
    }
    while ((j < (2 * length)) && (buffer[j] == BASE_58_ALPHABET[0])) {
        ++j;
    }
    while (zeroCount-- > 0) {
        buffer[--j] = BASE_58_ALPHABET[0];
    }
    length = 2 * length - j;
    if (maxoutlen < length) {
        THROW(EXCEPTION_OVERFLOW);
    }
    os_memmove(out, (buffer + j), length);
    return length;
}

static void getAeAddressStringFromBinary(uint8_t *publicKey, char *address) {
    uint8_t buffer[36];
    uint8_t hashAddress[32];

    os_memmove(buffer, publicKey, 32);
    cx_hash_sha256(buffer, 32, hashAddress);
    cx_hash_sha256(hashAddress, 32, hashAddress);
    os_memmove(buffer + 32, hashAddress, 4);

    snprintf(address, sizeof(address), "ak_");
    address[encodeBase58(buffer, 36, (unsigned char*)address + 3, 51) + 3] = '\0';
}

void getAeAddressStringFromKey(cx_ecfp_public_key_t *publicKey, char *address) {
    uint8_t buffer[32];

    for (int i = 0; i < 32; i++) {
        buffer[i] = publicKey->W[64 - i];
    }
    if ((publicKey->W[32] & 1) != 0) {
        buffer[31] |= 0x80;
    }
    getAeAddressStringFromBinary(buffer, address);
}

uint32_t readUint32BE(uint8_t *buffer) {
  return (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | (buffer[3]);
}

static const uint32_t HARDENED_OFFSET = 0x80000000;

static const uint32_t derivePath[BIP32_PATH] = {
  44 | HARDENED_OFFSET,
  457 | HARDENED_OFFSET,
  0 | HARDENED_OFFSET,
  0 | HARDENED_OFFSET,
  0 | HARDENED_OFFSET
};

void getPrivateKey(uint32_t accountNumber, cx_ecfp_private_key_t *privateKey) {
    uint8_t privateKeyData[32];
    uint32_t bip32Path[BIP32_PATH];

    os_memmove(bip32Path, derivePath, sizeof(derivePath));
    bip32Path[2] = accountNumber | HARDENED_OFFSET;
    os_perso_derive_node_bip32(CX_CURVE_Ed25519, bip32Path, BIP32_PATH, privateKeyData, NULL);
    cx_ecfp_init_private_key(CX_CURVE_Ed25519, privateKeyData, 32, privateKey);
    os_memset(privateKeyData, 0, sizeof(privateKeyData));
}

void sign(uint32_t accountNumber, uint8_t *data, uint32_t dataLength, uint8_t *out) {
    cx_ecfp_private_key_t privateKey;
    uint8_t signature[64];

    getPrivateKey(accountNumber, &privateKey);
    unsigned int info = 0;
    cx_eddsa_sign(&privateKey, CX_RND_RFC6979 | CX_LAST, CX_SHA512,
                         data,
                         dataLength,
                         NULL, 0, signature, &info);
    os_memset(&privateKey, 0, sizeof(privateKey));
    os_memmove(out, signature, 64);
}

void sendResponse(uint8_t tx, bool approve) {
    G_io_apdu_buffer[tx++] = approve? 0x90 : 0x69;
    G_io_apdu_buffer[tx++] = approve? 0x00 : 0x85;
    // Send back the response, do not restart the event loop
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, tx);
    // Display back the original UX
    ui_idle();
}

unsigned int ui_prepro(const bagl_element_t *element) {
    unsigned int display = 1;
    if (element->component.userid > 0) {
        display = (ux_step == element->component.userid - 1);
        if (display) {
            if (element->component.userid == 1) {
                UX_CALLBACK_SET_INTERVAL(2000);
            }
            else {
                UX_CALLBACK_SET_INTERVAL(MAX(3000, 1000 + bagl_label_roundtrip_duration_ms(element, 7)));
            }
        }
    }
    return display;
}

static bool rlpCanDecode(uint8_t *buffer, uint32_t bufferLength, bool *valid) {
    if (*buffer <= 0x7f) {
    } else if (*buffer <= 0xb7) {
    } else if (*buffer <= 0xbf) {
        if (bufferLength < (1 + (*buffer - 0xb7))) {
            return false;
        }
        if (*buffer > 0xbb) {
            *valid = false; // arbitrary 32 bits length limitation
            return true;
        }
    } else if (*buffer <= 0xf7) {
    } else {
        if (bufferLength < (1 + (*buffer - 0xf7))) {
            return false;
        }
        if (*buffer > 0xfb) {
            *valid = false; // arbitrary 32 bits length limitation
            return true;
        }
    }
    *valid = true;
    return true;
}

static bool rlpDecodeLength(uint8_t *buffer, uint32_t *fieldLength, uint32_t *offset, bool *list) {
    if (*buffer <= 0x7f) {
        *offset = 0;
        *fieldLength = 1;
        *list = false;
    } else if (*buffer <= 0xb7) {
        *offset = 1;
        *fieldLength = *buffer - 0x80;
        *list = false;
    } else if (*buffer <= 0xbf) {
        *offset = 1 + (*buffer - 0xb7);
        *list = false;
        switch (*buffer) {
        case 0xb8:
            *fieldLength = *(buffer + 1);
            break;
        case 0xb9:
            *fieldLength = (*(buffer + 1) << 8) + *(buffer + 2);
            break;
        case 0xba:
            *fieldLength =
                (*(buffer + 1) << 16) + (*(buffer + 2) << 8) + *(buffer + 3);
            break;
        case 0xbb:
            *fieldLength = (*(buffer + 1) << 24) + (*(buffer + 2) << 16) +
                           (*(buffer + 3) << 8) + *(buffer + 4);
            break;
        default:
            return false; // arbitrary 32 bits length limitation
        }
    } else if (*buffer <= 0xf7) {
        *offset = 1;
        *fieldLength = *buffer - 0xc0;
        *list = true;
    } else {
        *offset = 1 + (*buffer - 0xf7);
        *list = true;
        switch (*buffer) {
        case 0xf8:
            *fieldLength = *(buffer + 1);
            break;
        case 0xf9:
            *fieldLength = (*(buffer + 1) << 8) + *(buffer + 2);
            break;
        case 0xfa:
            *fieldLength =
                (*(buffer + 1) << 16) + (*(buffer + 2) << 8) + *(buffer + 3);
            break;
        case 0xfb:
            *fieldLength = (*(buffer + 1) << 24) + (*(buffer + 2) << 16) +
                           (*(buffer + 3) << 8) + *(buffer + 4);
            break;
        default:
            return false; // arbitrary 32 bits length limitation
        }
    }

    return true;
}

static void rlpParseInt(uint8_t *workBuffer, uint32_t fieldLength, uint32_t offset, char *buffer) {
    uint64_t amount = 0;
    if (offset == 0) {
        workBuffer--;
        amount = *workBuffer++;
    }
    else{
        workBuffer += offset - 1;
        for (uint8_t i = 0; i < fieldLength; i++) {
            amount += *workBuffer++ << (8 * (fieldLength - 1 - i));
        }
    }
    if (amount == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    uint8_t digits;
    uint64_t temp = amount;
    //int nDigits = floor(log10(abs(amount))) + 1;
    for (digits = 0; temp != 0; ++digits, temp /= 10 );

    for (uint8_t i = 0; amount != 0; ++i, amount /= 10 )
    {
        buffer[digits - 1 - i] = amount % 10 + '0';
    }
    buffer[digits] = '\0';
}

void parseTx(char *address, char *amount, char *fee, uint8_t *data) {
    uint8_t publicKey[32];
    uint8_t buffer[5];
    uint8_t bufferPos = 0;
    uint32_t fieldLength;
    uint32_t offset = 0;
    rlpTxType type = -1;
    bool isList = true;
    bool valid = false;
    while (type != TX_FEE) {
        do {
            buffer[bufferPos++] = *data++;
        } while (!rlpCanDecode(buffer, bufferPos, &valid));
        if (!rlpDecodeLength(data - bufferPos,
                                &fieldLength, &offset,
                                &isList)) {
            PRINTF("Invalid RLP Length\n");
            THROW(0x6800);
        }
        type++;
        switch (type) {
            case TX_TYPE:
                if (*data++ != SPEND_TRANSACTION_PREFIX) {
                    PRINTF("Wrong type of transaction\n");
                    THROW(0x6A80);
                }
                data++;
                break;
            case TX_SENDER:
                if (*data != ACCOUNT_ADDRESS_PREFIX) {
                    PRINTF("Wrong type of sender: %d\n", *data);
                    THROW(0x6A80);
                }
                data++;
                data += 32;
                break;
            case TX_RECIPENT:
                if (*data != ACCOUNT_ADDRESS_PREFIX) {
                    PRINTF("Wrong type of recipent: %d\n", *data);
                    THROW(0x6A80);
                }
                data++;
                os_memmove(publicKey, data, 32);
                getAeAddressStringFromBinary(publicKey, address);
                data += 32;
                break;
            case TX_AMOUNT:
                rlpParseInt(data, fieldLength, offset, amount);
                data += fieldLength + offset - 1;
                break;
            case TX_FEE:
                rlpParseInt(data, fieldLength, offset, fee);
                break;
        }
        bufferPos = 0;
        fieldLength = 0;
        valid = false;
    }
}
