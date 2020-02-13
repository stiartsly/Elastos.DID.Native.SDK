/*
 * Copyright (c) 2019 Elastos Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

#include "HDkey.h"
#include "BRBIP39Mnemonic.h"
#include "BRBIP39WordsEn.h"
#include "BRBIP39WordsChs.h"
#include "BRBIP39WordsFrance.h"
#include "BRBIP39WordsJap.h"
#include "BRBIP39WordsSpan.h"
#include "BRBIP39WordsCht.h"
#include "BRBIP32Sequence.h"
#include "BRCrypto.h"
#include "BRBase58.h"
#include "BRInt.h"

static unsigned char PADDING_IDENTITY = 0x67;
static unsigned char PADDING_STANDARD = 0xAD;

static uint32_t VersionCode = 0x0488ade4;

#define MAX_PUBLICKEY_BASE58 64

static const char **get_word_list(int language)
{
    switch (language) {
    case LANGUAGE_ENGLISH:
        return BRBIP39WordsEn;

    case LANGUAGE_FRENCH:
        return BRBIP39WordsFrance;

    case LANGUAGE_SPANISH:
        return BRBIP39WordsSpan;

    case LANGUAGE_JAPANESE:
        return BRBIP39WordsJap;

    case LANGUAGE_CHINESE_SIMPLIFIED:
        return BRBIP39WordsChs;

    case LANGUAGE_CHINESE_TRADITIONAL:
        return BRBIP39WordsCht;

    default:
        return BRBIP39WordsEn;
    }
}

// need to free after use this function
const char *HDKey_GenerateMnemonic(int language)
{
    unsigned char rand[16];
    const char **word_list;
    char *phrase;
    size_t len;

    //create random num
    if (RAND_bytes(rand, sizeof(rand)) != 1)
        return NULL;

    word_list = get_word_list(language);
    if (!word_list)
        return NULL;

    len = BRBIP39Encode(NULL, 0, word_list, rand, sizeof(rand));

    phrase = (char *)malloc(len);
    if (!phrase)
        return NULL;

    BRBIP39Encode(phrase, len, word_list, rand, sizeof(rand));
    return (const char *)phrase;
}

void HDKey_FreeMnemonic(void *mnemonic)
{
    if (mnemonic)
        free(mnemonic);
}

uint8_t *HDKey_GetSeedFromMnemonic(const char *mnemonic,
        const char* passphrase, int language, uint8_t *seed)
{
    int len;
    const char **word_list;

    if (!mnemonic || !seed)
        return NULL;

    word_list = get_word_list(language);
    if (!word_list)
        return NULL;

    if (!BRBIP39PhraseIsValid(word_list, mnemonic))
        return NULL;

    BRBIP39DeriveKey((UInt512 *)seed, mnemonic, passphrase);

    return seed;
}

ssize_t HDKey_GetExtendedkeyFromMnemonic(const char *mnemonic,
        const char* passphrase, int language, uint8_t *extendedkey, size_t size)
{
    int len;
    const char **word_list;
    uint8_t seed[SEED_BYTES];

    if (!mnemonic || !*mnemonic || !seed)
        return -1;

    word_list = get_word_list(language);
    if (!word_list)
        return -1;

    if (!BRBIP39PhraseIsValid(word_list, mnemonic))
        return -1;

    BRBIP39DeriveKey((UInt512 *)seed, mnemonic, passphrase);

    return HDKey_GetExtendedkeyFromSeed(extendedkey, size, seed, sizeof(seed));
}

ssize_t HDKey_GetExtendedkeyFromSeed(uint8_t *extendedkey, size_t size,
        uint8_t *seed, size_t seedLen)
{
    UInt256 chaincode, secret;
    uint8_t version[4];
    unsigned int md32[32];
    uint8_t privatekey[33];
    size_t tsize = 0;

    if (size < EXTENDEDKEY_BYTES)
        return -1;

    BRBIP32vRootFromSeed(&secret, &chaincode, (const void *)seed, seedLen);

    privatekey[0] = 0;
    memcpy(privatekey + 1, secret.u8, PRIVATEKEY_BYTES);

    version[0] = (uint8_t)((VersionCode >> 24) & 0xFF);
    version[1] = (uint8_t)((VersionCode >> 16) & 0xFF);
    version[2] = (uint8_t)((VersionCode >> 8) & 0xFF);
    version[3] = (uint8_t)(VersionCode & 0xFF);

    memset(extendedkey, 0, size);
    //version
    memcpy(extendedkey, version, sizeof(version));
    tsize += sizeof(version);
    // 1 bytes--depth: 0x00 for master nodes
    // 4 bytes--the fingerprint of the parent's key (0x00000000 if master key)
    // 4 bytes--child number. (0x00000000 if master key)
    tsize += 9;
    // chaincoid
    memcpy(extendedkey + tsize, chaincode.u8, CHAINCODE_BYTES);
    tsize += CHAINCODE_BYTES;
    // private key
    memcpy(extendedkey + tsize, privatekey, sizeof(privatekey));
    tsize += sizeof(privatekey);

    BRSHA256_2(md32, version, sizeof(version));
    memcpy(extendedkey + tsize, md32, 4);
    tsize += 4;

    assert(tsize == size);
    return tsize;
}

bool HDKey_MnemonicIsValid(const char *mnemonic, int language)
{
    const char **word_list;

    if (!mnemonic)
        return false;

    word_list = get_word_list(language);
    if (!word_list)
        return false;

    return (BRBIP39PhraseIsValid(word_list, mnemonic) != 0);
}

static int parse_extendedkey(uint8_t *extendedkey, size_t len, UInt256 *chaincode,
        UInt256 *secret)
{
    size_t size;

    assert(extendedkey && len > 0);
    assert(secret);
    assert(chaincode);

    size = 13;
    if (size + sizeof(chaincode->u8) > len)
        return -1;

    memcpy(chaincode->u8, extendedkey + size, sizeof(chaincode->u8));
    size += sizeof(chaincode->u8) + 1;
    if (size + sizeof(secret->u8) > len)
        return -1;

    memcpy(secret->u8, extendedkey + size, sizeof(secret->u8));
    return 0;
}

HDKey *HDKey_GetPrivateIdentity(const uint8_t *extendedkey, size_t size,
        int coinType, HDKey *privateIdentity)
{
    char publickey[PUBLICKEY_BYTES];
    UInt256 chaincode, secret;
    BRKey key;

    if (!extendedkey || size <= 0 || !privateIdentity)
        return NULL;

    if (parse_extendedkey((uint8_t*)extendedkey, size, &chaincode, &secret) == -1)
        return NULL;

    memcpy(privateIdentity->chainCodeForPk, chaincode.u8, sizeof(privateIdentity->chainCodeForPk));
    memcpy(privateIdentity->chainCodeForSk, chaincode.u8, sizeof(privateIdentity->chainCodeForSk));
    memcpy(privateIdentity->privatekey, secret.u8, sizeof(privateIdentity->privatekey));

    BRBIP32PrivKeyPathFromRoot(&key, (UInt256*)privateIdentity->chainCodeForPk,
            &secret, 3, 44 | BIP32_HARD, coinType | BIP32_HARD, 0 | BIP32_HARD);

    getPubKeyFromPrivKey(publickey, &(key.secret));
    privateIdentity->fingerPrint = BRKeyHash160(&key).u32[0];
    memcpy(privateIdentity->publickey, publickey, sizeof(publickey));

    var_clean(&chaincode);
    var_clean(&secret);
    return privateIdentity;
}

void HDKey_Wipe(HDKey *privateIdentity)
{
    if (!privateIdentity)
        return;

    memset(privateIdentity, 0, sizeof(HDKey));
}

uint8_t *HDKey_GetSubPrivateKey(HDKey* privateIdentity, int coinType, int chain,
        int index, uint8_t *privatekey)
{
    BRKey key;
    UInt256 chaincode, secret;

    if (!privateIdentity || !privatekey)
        return NULL;

    memcpy(chaincode.u8, privateIdentity->chainCodeForSk, sizeof(chaincode.u8));
    memcpy(secret.u8, privateIdentity->privatekey, sizeof(secret.u8));

    BRBIP32PrivKeyPathFromRoot(&key, &chaincode, &secret,
            5, 44 | BIP32_HARD, coinType | BIP32_HARD,
            0 | BIP32_HARD, chain, index);
    assert(sizeof(key.secret.u8) == PRIVATEKEY_BYTES);

    memcpy(privatekey, key.secret.u8, PRIVATEKEY_BYTES);

    var_clean(&chaincode);
    var_clean(&secret);
    return privatekey;
}

uint8_t *HDKey_GetSubPublicKey(HDKey* privateIdentity, int chain, int index,
        uint8_t *publickey)
{
    if (!privateIdentity || !publickey)
        return NULL;

    BRMasterPubKey brPublicKey;
    memset(&brPublicKey, 0, sizeof(brPublicKey));

    brPublicKey.fingerPrint = privateIdentity->fingerPrint;
    memcpy((uint8_t*)&brPublicKey.chainCode, &privateIdentity->chainCodeForPk,
            sizeof(brPublicKey.chainCode));
    memcpy(brPublicKey.pubKey, privateIdentity->publickey, sizeof(brPublicKey.pubKey));

    assert(BRBIP32PubKey(NULL, 0, brPublicKey, chain, index) == PUBLICKEY_BYTES);

    BRBIP32PubKey(publickey, PUBLICKEY_BYTES, brPublicKey, chain, index);
    return publickey;
}

char *HDKey_GetAddress(uint8_t *publickey, char *address, size_t len)
{
    unsigned char redeem_script[35];
    unsigned int md32[32];
    unsigned char md20[20];
    unsigned char program_hash[21];
    unsigned char bin_idstring[25];
    size_t expected_len;

    if (!publickey || !address || !len)
        return NULL;

    redeem_script[0] = 33;
    memcpy(redeem_script + 1, publickey, 33);
    redeem_script[34] = PADDING_STANDARD;
    BRHash160(md20, redeem_script, sizeof(redeem_script));

    program_hash[0] = PADDING_IDENTITY;
    memcpy(program_hash + 1, md20, sizeof(md20));

    BRSHA256_2(md32, program_hash, sizeof(program_hash));

    memcpy(bin_idstring, program_hash, sizeof(program_hash));
    memcpy(bin_idstring + sizeof(program_hash), md32, 4);

    expected_len = BRBase58Encode(NULL, 0, bin_idstring, sizeof(bin_idstring));
    if (len < expected_len)
        return NULL;

    BRBase58Encode(address, len, bin_idstring, sizeof(bin_idstring));
    return address;
}

DerivedKey *HDKey_GetDerivedKey(HDKey* privateIdentity, DerivedKey *derivedkey,
        int coinType, int chain, int index)
{
    uint8_t *pk, *sk;
    char *idstring;
    uint8_t publickey[PUBLICKEY_BYTES];
    uint8_t privatekey[PRIVATEKEY_BYTES];
    char address[ADDRESS_LEN];

    if (!privateIdentity || !derivedkey)
        return NULL;

    pk = HDKey_GetSubPublicKey(privateIdentity, chain, index, publickey);
    sk = HDKey_GetSubPrivateKey(privateIdentity, coinType, chain, index, privatekey);
    idstring = HDKey_GetAddress(publickey, address, sizeof(address));

    memcpy(derivedkey->publickey, pk, PUBLICKEY_BYTES);
    memcpy(derivedkey->privatekey, sk, PRIVATEKEY_BYTES);
    memcpy(derivedkey->address, idstring, ADDRESS_LEN);

    return derivedkey;
}

uint8_t *DerivedKey_GetPublicKey(DerivedKey *derivedkey)
{
    if (!derivedkey)
        return NULL;

    return derivedkey->publickey;
}

const char *DerivedKey_GetPublicKeyBase(DerivedKey *derivedkey, char *base, size_t size)
{
    size_t len;

    if (!derivedkey || !base || size < MAX_PUBLICKEY_BASE58)
        return NULL;

    len = BRBase58Encode(NULL, 0, derivedkey->publickey,
            sizeof(derivedkey->publickey));

    BRBase58Encode(base, len, derivedkey->publickey, sizeof(derivedkey->publickey));
    return base;
}

uint8_t *DerivedKey_GetPrivateKey(DerivedKey *derivedkey)
{
    if (!derivedkey)
        return NULL;

    return derivedkey->privatekey;
}

char *DerivedKey_GetAddress(DerivedKey *derivedkey)
{
    if (!derivedkey)
        return NULL;

    return derivedkey->address;
}

void DerivedKey_Wipe(DerivedKey *derivedkey)
{
    if (!derivedkey)
        return;

    memset(derivedkey, 0, sizeof(DerivedKey));
}