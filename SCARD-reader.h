/* --------------------------------------------------------------------------------
 #
 #  SCARD-reader.h
 #  One interface for every way the plugin talks to a card reader:
 #
 #    macOS    CryptoTokenKit      any reader with a system driver (RC-S300, ...)
 #    macOS    libusb              Sony RC-S380 (NFC Port-100)
 #    macOS    libpafe + libusb    Sony RC-S310 / RC-S320 / RC-S330 (RC-S956)
 #    Windows  PC/SC               any reader with a driver
 #
 #  Each adapter only moves bytes (open, wait for a card, read a service, close).
 #  What is read - identifiers, then the usage history - is decided once, in
 #  scard_read_tag().
 #
 # --------------------------------------------------------------------------------*/

#ifndef SCARD_READER_H
#define SCARD_READER_H

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <winscard.h>
#endif

#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <ctime>
#include <cstdint>

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <CryptoTokenKit/CryptoTokenKit.h>
#include "libusb-1.0/libusb.h"
#include "libpafe.h"
#include "pasori_command.h"
#include "felica_command.h"

/* libusb */
#define LIBUSB_DATASIZE 255
#define LIBUSB_USLEEP_DURATION 250000 //microseconds
#define LIBUSB_API_TIMEOUT 2000  //milliseconds
#define LIBUSB_API_TIMEOUT_FOR_POLLING 25000 //milliseconds

/* SONY PaSoRi */
#define LIBUSB_SONY 0x054C
#define LIBUSB_SONY_RC_S380 0x06C3
#define LIBUSB_SONY_RC_S330 0x02E1
#define LIBUSB_SONY_RC_S320 0x01BB
#define LIBUSB_SONY_RC_S310 0x006C
#define LIBUSB_SONY_RC_S300 0x0DC9

/* CryptoTokenKit */
#define TK_POLL_INTERVAL 250000  //microseconds
#define TK_REPLY_TIMEOUT_MS 5000  //milliseconds
#endif

#ifdef _WIN32
#define DEFAULT_TIMEOUT_MS_FOR_RESOURCE_MANAGER 5000
#define LIBPCSC_API_TIMEOUT 2000  //milliseconds
#endif

/* FeliCa usage history (transit IC cards) */
#define SCARD_HISTORY_SERVICE 0x090F
#define SCARD_HISTORY_BLOCKS 20

enum
{
    APDU_CLA_GENERIC = 0xFF,

    APDU_INS_GET_DATA = 0xCA,
    APDU_INS_READ_BINARY = 0xB0,
    APDU_INS_SELECT_FILE = 0xA4,

    APDU_P1_GET_UID = 0x00,
    APDU_P1_GET_PMm = 0x01,

    APDU_P2_NONE = 0x00,

    APDU_LE_MAX_LENGTH = 0x00
};

struct ReadOptions {
    char nfc_type;          /* 'F' FeliCa, 'B' Type B, 'A' Type A (RC-S380 only) */
    unsigned int timeout;   /* seconds to wait for a card, then again for the history */
    bool system;            /* also read the usage history */
    ReadOptions() : nfc_type('F'), timeout(60), system(false) {}
};

struct ReadResult {
    bool success;
    std::string IDm;
    std::string PMm;
    /* Type B (ATQB), RC-S380 only */
    std::string nfcid;
    std::string appdata;
    std::string pinfo;
    std::string cid;
    std::string errorMessage;
    /* usage history */
    bool hasServiceData;
    std::string serviceCode;
    std::vector<std::string> serviceData;  /* one hex string per 16-byte block */
    ReadResult() : success(false), hasServiceData(false) {}
};

class CardReader {
public:
    virtual ~CardReader() {}
    /* open the reader; false and errorMessage on failure */
    virtual bool open(const ReadOptions& options, std::string& errorMessage) = 0;
    /* wait until a card answers, the deadline passes or the read is cancelled;
       true if at least one identifier was read into result */
    virtual bool poll(const ReadOptions& options, time_t deadline,
                      const std::atomic<bool>& cancelled, ReadResult& result) = 0;
    /* read up to maxBlocks 16-byte blocks of a FeliCa service without encryption,
       from block 0, stopping at the first block that cannot be read */
    virtual void readService(uint16_t serviceCode, unsigned int maxBlocks, time_t deadline,
                             const std::atomic<bool>& cancelled, std::vector<std::string>& blocks) = 0;
    /* release everything open() acquired; safe to call after a failed open() */
    virtual void close() = 0;
};

/* the adapter for a reader name, as returned by SCARD Get readers */
std::unique_ptr<CardReader> scard_create_reader(const std::string& slotName);

/* open, wait for a card, read its identifiers (and the history), close */
void scard_read_tag(const std::string& slotName, const ReadOptions& options,
                    const std::atomic<bool>& cancelled, ReadResult& result);

void scard_hex(const uint8_t *bytes, size_t length, std::string& hex);

#endif /* SCARD_READER_H */
