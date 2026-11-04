/* --------------------------------------------------------------------------------
 #
 #  SCARD-reader.cpp
 #  Reader adapters and the read sequence shared by all of them.
 #  See SCARD-reader.h for the overview.
 #
 # --------------------------------------------------------------------------------*/

#include "SCARD-reader.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iostream>

#ifndef _WIN32
#include <unistd.h>
#endif

void scard_hex(const uint8_t *bytes, size_t length, std::string& hex) {

    static const char digits[] = "0123456789abcdef";
    hex.clear();
    hex.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        hex += digits[bytes[i] >> 4];
        hex += digits[bytes[i] & 0x0F];
    }
}

static bool deadline_passed(time_t deadline, const std::atomic<bool>& cancelled) {
    return cancelled || (time(0) >= deadline);
}

#pragma mark - shared read sequence

void scard_read_tag(const std::string& slotName, const ReadOptions& options,
                    const std::atomic<bool>& cancelled, ReadResult& result) {

    std::unique_ptr<CardReader> reader = scard_create_reader(slotName);
    if(!reader) {
        result.errorMessage = "no reader adapter for this slotName";
        return;
    }

    if(reader->open(options, result.errorMessage)) {

        time_t deadline = time(0) + options.timeout;

        if(reader->poll(options, deadline, cancelled, result)) {

            result.success = true;

            /* the usage history is a FeliCa service: Type A/B cards have none */
            if(options.system && (options.nfc_type == 'F')) {
                result.hasServiceData = true;
                result.serviceCode = "090f";
                /* the history gets its own timeout window, as before */
                reader->readService(SCARD_HISTORY_SERVICE, SCARD_HISTORY_BLOCKS,
                                    time(0) + options.timeout, cancelled, result.serviceData);
            }
        }
    }

    reader->close();
}

#ifdef __APPLE__

#pragma mark - macOS: CryptoTokenKit

/*
 The previous implementation nested every request in the reply block of the one
 before, and signalled the waiting thread from the innermost block. A request
 that failed on the way left the thread waiting forever. Here every request is
 sent and waited for on its own, with a timeout.
 */

class TKReader : public CardReader {

    std::string slotName;
    TKSmartCardSlot *slot;
    TKSmartCard *card;
    bool inSession;

    bool beginSession() {
        __block bool ok = false;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [card beginSessionWithReply:^(BOOL success, NSError *error) {
            ok = success;
            dispatch_semaphore_signal(sem);
        }];
        long timedOut = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)TK_REPLY_TIMEOUT_MS * NSEC_PER_MSEC));
        dispatch_release(sem); /* the reply block holds its own reference (MRC) */
        return (timedOut == 0) && ok;
    }

    bool transmit(const uint8_t *apdu, size_t length, std::vector<uint8_t>& response) {
        response.clear();
        __block NSData *reply = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        NSData *request = [NSData dataWithBytes:apdu length:length];
        [card transmitRequest:request reply:^(NSData *data, NSError *error) {
            if((error == nil) && (data != nil)) {
                reply = [data retain];
            }
            dispatch_semaphore_signal(sem);
        }];
        long timedOut = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)TK_REPLY_TIMEOUT_MS * NSEC_PER_MSEC));
        dispatch_release(sem);
        if(timedOut != 0) {
            /* a late reply only touches the block's own copy of `reply` (one NSData leaks) */
            return false;
        }
        if(reply == nil) {
            return false;
        }
        const uint8_t *bytes = (const uint8_t *)[reply bytes];
        response.assign(bytes, bytes + [reply length]);
        [reply release];
        return true;
    }

    static bool statusOK(const std::vector<uint8_t>& response) {
        size_t n = response.size();
        return (n >= 2) && (response[n - 2] == 0x90) && (response[n - 1] == 0x00);
    }

    /* GET DATA (FF CA p1 00 00): UID / PMm, without the status words, at most 8 bytes */
    bool getData(uint8_t p1, std::string& hex) {
        uint8_t apdu[5] = {APDU_CLA_GENERIC, APDU_INS_GET_DATA, p1, APDU_P2_NONE, APDU_LE_MAX_LENGTH};
        std::vector<uint8_t> response;
        if(transmit(apdu, sizeof(apdu), response) && statusOK(response)) {
            size_t length = response.size() - 2;
            scard_hex(response.data(), length < 8 ? length : 8, hex);
            return true;
        }
        return false;
    }

public:

    TKReader(const std::string& name) : slotName(name), slot(nil), card(nil), inSession(false) {}
    ~TKReader() { close(); }

    bool open(const ReadOptions& options, std::string& errorMessage) {
        @autoreleasepool {
            TKSmartCardSlotManager *manager = [TKSmartCardSlotManager defaultManager];
            if(!manager) {
                errorMessage = "TKSmartCardSlotManager::defaultManager() failed";
                return false;
            }
            NSString *name = [[NSString alloc]initWithUTF8String:slotName.c_str()];
            slot = [[manager slotNamed:name] retain];
            [name release];
            if(!slot) {
                errorMessage = "TKSmartCardSlotManager::slotNamed() failed";
                return false;
            }
        }
        return true;
    }

    bool poll(const ReadOptions& options, time_t deadline,
              const std::atomic<bool>& cancelled, ReadResult& result) {

        /* wait for a card: the previous implementation failed at once if none was present */
        while (slot.state != TKSmartCardSlotStateValidCard) {
            if(deadline_passed(deadline, cancelled)) {
                return false;
            }
            usleep(TK_POLL_INTERVAL);
        }

        bool found = false;
        @autoreleasepool {
            card = [[slot makeSmartCard] retain];
            if(!card) {
                result.errorMessage = "TKSmartCard::makeSmartCard() failed";
                return false;
            }
            inSession = beginSession();
            if(!inSession) {
                result.errorMessage = "TKSmartCard::beginSession() failed";
                return false;
            }
            found = getData(APDU_P1_GET_UID, result.IDm);
            found = getData(APDU_P1_GET_PMm, result.PMm) || found;
        }
        return found;
    }

    void readService(uint16_t serviceCode, unsigned int maxBlocks, time_t deadline,
                     const std::atomic<bool>& cancelled, std::vector<std::string>& blocks) {
        if(!inSession) return;
        @autoreleasepool {
            /* SELECT FILE takes the service code little-endian */
            uint8_t select[7] = {APDU_CLA_GENERIC, APDU_INS_SELECT_FILE, 0x00, 0x01, 0x02,
                (uint8_t)(serviceCode & 0xFF), (uint8_t)(serviceCode >> 8)};
            for (unsigned int i = 0; (i < maxBlocks) && (i < 256); ++i) {
                if(deadline_passed(deadline, cancelled)) break;
                std::vector<uint8_t> response;
                if(!transmit(select, sizeof(select), response)) break;
                uint8_t read[5] = {APDU_CLA_GENERIC, APDU_INS_READ_BINARY, 0x00, (uint8_t)i, 0x00};
                if(!transmit(read, sizeof(read), response)) break;
                if(response.size() != 18) break; /* 16 bytes + SW1 SW2 */
                std::string hex;
                scard_hex(response.data(), 16, hex);
                blocks.push_back(hex);
            }
        }
    }

    void close() {
        if(card) {
            if(inSession) {
                [card endSession];
                inSession = false;
            }
            [card release];
            card = nil;
        }
        if(slot) {
            [slot release];
            slot = nil;
        }
    }
};

#pragma mark - macOS: Sony RC-S380 over libusb

struct device_info {
  libusb_device *dev;
  libusb_device_handle *dh;
  uint8_t ep_in;
  uint8_t ep_out;
  int interface_num;
};
typedef device_info usb_device_info;

static short checksum(char cmd, uint8_t *buf, int size) {
    
  int sum = (unsigned int)cmd;
  for(int i = 0; i < size; i++){
    sum += buf[i];
  }
  return (0x100 - sum) % 0x100;
}

static bool get_usb_information(libusb_device_handle *dh, usb_device_info *devinfo) {
    
    memset(devinfo, 0, sizeof(usb_device_info));
    
    libusb_device *dev;
    struct libusb_config_descriptor *conf;
    const struct libusb_endpoint_descriptor *endp;
    const struct libusb_interface *intf;
    const struct libusb_interface_descriptor *intdesc;
    
    dev = libusb_get_device(dh);
    
    if(dev == NULL){
        std::cout << "device get error..." << std::endl;
        return false;
    }
    
    devinfo->dh = dh;
    devinfo->dev = dev;
    
    if(libusb_get_config_descriptor(dev, 0, &conf) != LIBUSB_SUCCESS) {
        /* conf is not set on failure */
        return false;
    }

    for(int i = 0; i < (int)conf->bNumInterfaces; i++){
        intf = &conf->interface[i];
        for(int j = 0; j < intf->num_altsetting; j++){
            intdesc = &intf->altsetting[j];
            for(int k = 0; k < (int)intdesc->bNumEndpoints; k++){
                endp = &intdesc->endpoint[k];
                
                switch(endp->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) {
                    case LIBUSB_TRANSFER_TYPE_BULK:
                        //printf("bulk endpoint: %02x\n", endp->bEndpointAddress);
                        if((endp->bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_IN){
                            devinfo->ep_in = endp->bEndpointAddress;
                        }
                        if((endp->bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_OUT){
                            devinfo->ep_out = endp->bEndpointAddress;
                        }
                        break;
                    case LIBUSB_TRANSFER_TYPE_INTERRUPT:
                        //printf("interrupt endpoint: %02x\n", endp->bEndpointAddress);
                        break;
                }
            }
        }
    }
    libusb_free_config_descriptor(conf);
    
    return true;
}

static int packet_init(usb_device_info *devinfo, int timeout) {
    
  uint8_t cmd[6];
  int ret;
  int len;

  // ack command
  memcpy(cmd, "\x00\x00\xff\x00\xff\x00", 6);
      
  ret = libusb_bulk_transfer(devinfo->dh, devinfo->ep_out,
                 (unsigned char *)cmd, sizeof(cmd), &len, timeout);
    
  if(ret < 0) std::cout << "data send error..." << std::endl;
    
  return ret;
}

static size_t packet_send(usb_device_info *devinfo, uint8_t *buf, int size,
                          std::vector<uint8_t> *usbbuf, int timeout) {
    
    uint8_t rcv[LIBUSB_DATASIZE], rbuf[LIBUSB_DATASIZE];
    
    int len;
    int ret;
    
    ret = libusb_bulk_transfer(devinfo->dh, devinfo->ep_out,
                               (unsigned char *)buf, size, &len, timeout);
    if(ret < 0){
        std::cout << "data send error..." << std::endl;
        return 0;
    }
    
    // receive ack/nck
    ret = libusb_bulk_transfer(devinfo->dh, devinfo->ep_in,
                               (unsigned char *)rcv, sizeof(rcv), &len, timeout);
    if(ret < 0){
        std::cout << "data receive error..." << std::endl;
        return 0;
    }
    
    len = 0;
    
    // receive response
    ret = libusb_bulk_transfer(devinfo->dh, devinfo->ep_in,
                               (unsigned char *)rbuf, sizeof(rbuf), &len, timeout);
    if(ret < 0){
        std::cout << "data receive error..." << std::endl;
        return 0;
    }
    
    if(len > usbbuf->size()) {
        usbbuf->resize(len);
    }
    
    memcpy(&usbbuf->at(0), rbuf, len);

    return len;
}

static size_t packet_write(usb_device_info *devinfo, uint8_t *buf, int size,
                             std::vector<uint8_t> *usbbuf, int timeout) {
    
  uint8_t cmd[LIBUSB_DATASIZE];
  int n;
  short csum;

  n = size;
  if(n < 1) return 0;

  // data = 0xd6 + data
  // len = len(data)
  // 00 00 ff ff ff len(L) len(H) checksum(len) data checksum(data) 00
    
    /* header */
    cmd[0] = 0x00; cmd[1] = 0x00; cmd[2] = 0xff;
    cmd[3] = 0xff; cmd[4] = 0xff;
    
    /* data length */
    cmd[5] = ((n + 1) & 0xff);
    cmd[6] = ((n + 1) & 0xff00) >> 8;
    
    /* checksum */
    csum = (0x100 - (cmd[5] + cmd[6])) % 0x100;
    cmd[7] = csum;
    
    /* delimiter */
    cmd[8] = 0xd6;
    
    /* data */
    memcpy(cmd + 9, buf, size);

    /* checksum */
    csum = checksum(cmd[8], buf, size);
    cmd[9 + n] = csum;
    
    /* terminator */
    cmd[10 + n] = 0x00;
    n += 11;
  
  return packet_send(devinfo, cmd, n, usbbuf, timeout);
}

static size_t packet_setcommandtype(usb_device_info *devinfo,
                                      std::vector<uint8_t> *usbbuf, int timeout) {
    
  uint8_t cmd[2];
  memcpy(cmd, "\x2a\x01", 2);
  return packet_write(devinfo, cmd, sizeof(cmd), usbbuf, timeout);
}

static size_t packet_switch_rf(usb_device_info *devinfo,
                                 std::vector<uint8_t> *usbbuf, int timeout) {
    
  uint8_t cmd[2];
  memcpy(cmd, "\x06\x00", 2);
    
  return packet_write(devinfo, cmd, sizeof(cmd), usbbuf, timeout);
}

static size_t packet_sens_req(usb_device_info *devinfo, char type,
                                std::vector<uint8_t> *usbbuf, int timeout) {
    
  uint8_t cmd[9];
  int len;
  if(type == 'F'){
    len = 9;
    memcpy(cmd, "\x04\x6e\x00\x06\x00\xff\xff\x01\x00", len);
  }
  if(type == 'A'){
    len = 4;
    memcpy(cmd, "\x04\x6e\x00\x26", len);
  }
  if(type == 'B'){
    len = 6;
    memcpy(cmd, "\x04\x6e\x00\x05\x00\x10", len);
  }
  return packet_write(devinfo, cmd, len, usbbuf, timeout);
}

static size_t packet_inset_rf(usb_device_info *devinfo, char type,
                                std::vector<uint8_t> *usbbuf, int timeout) {
    
  uint8_t cmd[5];
    
  if(type == 'F') memcpy(cmd, "\x00\x01\x01\x0f\x01", 5); // 212F
  if(type == 'A') memcpy(cmd, "\x00\x02\x03\x0f\x03", 5); // 106A
  if(type == 'B') memcpy(cmd, "\x00\x03\x07\x0f\x07", 5); // 106B
    
  return packet_write(devinfo, cmd, sizeof(cmd), usbbuf, timeout);
}

static size_t packet_inset_protocol_1(usb_device_info *devinfo,
                                      std::vector<uint8_t> *usbbuf, int timeout) {
    
    uint8_t cmd[39];
    
    memcpy(cmd, "\x02\x00\x18\x01\x01\x02\x01\x03\x00\x04\x00\x05\x00\x06\x00\x07\x08\x08\x00\x09\x00\x0a\x00\x0b\x00\x0c\x00\x0e\x04\x0f\x00\x10\x00\x11\x00\x12\x00\x13\x06", 39);
    
    return packet_write(devinfo, cmd, sizeof(cmd), usbbuf, timeout);
}

static size_t packet_inset_protocol_2(usb_device_info *devinfo, char type,
                                      std::vector<uint8_t> *usbbuf, int timeout) {
    
  uint8_t cmd[11];
    
  int len;
  if(type == 'F'){
    len = 3;
    memcpy(cmd, "\x02\x00\x18", len);
  }
  if(type == 'A'){
    len = 11;
    memcpy(cmd, "\x02\x00\x06\x01\x00\x02\x00\x05\x01\x07\x07", len);
  }
  if(type == 'B'){
    len = 11;
    memcpy(cmd, "\x02\x00\x14\x09\x01\x0a\x01\x0b\x01\x0c\x01", len);
  }
    
  return packet_write(devinfo, cmd, len, usbbuf, timeout);
}


/*
 The packet sequences below are the ones scard-v3 sent, unchanged; they were
 worked out against the hardware (see nfcpy's rcs380.py). Only the stop
 conditions are shared with the other readers now.
 */

class RCS380Reader : public CardReader {

    libusb_device_handle *device;
    usb_device_info devinfo;
    std::vector<uint8_t> usbbuf;
    bool libusbReady;
    bool claimed;
    char nfc_type;
    uint8_t idm[8];

public:

    RCS380Reader() : device(NULL), usbbuf(LIBUSB_DATASIZE), libusbReady(false), claimed(false), nfc_type('F') {
        memset(&devinfo, 0, sizeof(devinfo));
        memset(idm, 0, sizeof(idm));
    }
    ~RCS380Reader() { close(); }

    bool open(const ReadOptions& options, std::string& errorMessage) {
        nfc_type = options.nfc_type;
        if(libusb_init(NULL) < 0) {
            errorMessage = "libusb_init() failed";
            return false;
        }
        libusbReady = true;
        device = libusb_open_device_with_vid_pid(NULL, LIBUSB_SONY, LIBUSB_SONY_RC_S380);
        if(!device) {
            errorMessage = "RC-S380 could not be opened";
            return false;
        }
        // usb interface setting
        libusb_set_auto_detach_kernel_driver(device, 1);
        libusb_set_configuration(device, 1);
        claimed = (libusb_claim_interface(device, 0) == LIBUSB_SUCCESS);
        libusb_set_interface_alt_setting(device, 0, 0);
        // get usb information
        if(!get_usb_information(device, &devinfo)) {
            errorMessage = "RC-S380 USB descriptor could not be read";
            return false;
        }
        packet_init(&devinfo, LIBUSB_API_TIMEOUT);
        packet_setcommandtype(&devinfo, &usbbuf, LIBUSB_API_TIMEOUT);
        packet_switch_rf(&devinfo, &usbbuf, LIBUSB_API_TIMEOUT);
        packet_inset_rf(&devinfo, nfc_type, &usbbuf, LIBUSB_API_TIMEOUT);
        packet_inset_protocol_1(&devinfo, &usbbuf, LIBUSB_API_TIMEOUT);
        packet_inset_protocol_2(&devinfo, nfc_type, &usbbuf, LIBUSB_API_TIMEOUT);
        return true;
    }

    bool poll(const ReadOptions& options, time_t deadline,
              const std::atomic<bool>& cancelled, ReadResult& result) {

        while (!deadline_passed(deadline, cancelled)) {

            size_t len = packet_sens_req(&devinfo, nfc_type, &usbbuf, LIBUSB_API_TIMEOUT_FOR_POLLING);
            if(len == 0) {
                /* read/write error */
                usleep(LIBUSB_USLEEP_DURATION);
                continue;
            }
            if(!(usbbuf[9] == 0x05 && usbbuf[10] == 0x00)) {
                continue;
            }
            /* FeliCa: polling response */
            int rlen = ((usbbuf[6] << 8) + usbbuf[5]);
            if(rlen == 27) {
                if(usbbuf[6 + 9] == 0x14 && usbbuf[7 + 9] == 0x01) {
                    uint8_t SW1 = usbbuf[25];
                    uint8_t SW2 = usbbuf[26];
                    if((SW1 == 0x10) && (SW2 == 0x0B)){
                        uint8_t pmm[8];
                        memcpy(idm, &usbbuf[ 8 + 9], 8);
                        memcpy(pmm, &usbbuf[16 + 9], 8);
                        scard_hex(idm, 8, result.IDm);
                        scard_hex(pmm, 8, result.PMm);
                        return true;
                    }
                    /* a FeliCa answer with unexpected status: stop, as before */
                    return false;
                }
            }
            /* Type B: ATQB (response to REQB) */
            if(usbbuf[6 + 9] == 0x50) {
                scard_hex(&usbbuf[ 7 + 9], 4, result.nfcid);   /* PUPI */
                scard_hex(&usbbuf[11 + 9], 4, result.appdata); /* Application Data */
                scard_hex(&usbbuf[15 + 9], 3, result.pinfo);   /* Protocol Info */
                scard_hex(&usbbuf[18 + 9], 1, result.cid);     /* CID */
                return true;
            }
            /* Type A: ATQA incomplete, the SAK anti-collision sequence is not implemented
               http://www.ti.com/lit/an/sloa136/sloa136.pdf
               https://www.nxp.com/docs/en/application-note/AN10833.pdf
               https://github.com/nfc-tools/libnfc/blob/master/examples/nfc-anticol.c */
        }
        return false;
    }

    void readService(uint16_t serviceCode, unsigned int maxBlocks, time_t deadline,
                     const std::atomic<bool>& cancelled, std::vector<std::string>& blocks) {

        /* read without encryption, two blocks per command */
        uint8_t cmd[21] = {
            0x04,
            0x86,
            0x01,
            0x12,       /* cmd len */
            0x06,       /* cmd_code:0x06 (read_without_encryption) */
            idm[0],     /* captured tag identifier (IDm) */
            idm[1],
            idm[2],
            idm[3],
            idm[4],
            idm[5],
            idm[6],
            idm[7],
            0x01, /* count of sc */
            (uint8_t)(serviceCode & 0xFF), /* service code, little-endian */
            (uint8_t)(serviceCode >> 8),
            0x02, /* count of bc */
            0x80, 0x00, /* block 0 */
            0x80, 0x01  /* block 1 */
        };

        packet_inset_rf(&devinfo, nfc_type, &usbbuf, LIBUSB_API_TIMEOUT);
        packet_inset_protocol_1(&devinfo, &usbbuf, LIBUSB_API_TIMEOUT);
        packet_write(&devinfo, cmd, sizeof(cmd), &usbbuf, LIBUSB_API_TIMEOUT);//InCommRF

        while ((blocks.size() < maxBlocks) && !deadline_passed(deadline, cancelled)) {

            size_t len = packet_sens_req(&devinfo, nfc_type, &usbbuf, LIBUSB_API_TIMEOUT_FOR_POLLING);
            if(len == 0) {
                /* read/write error */
                usleep(LIBUSB_USLEEP_DURATION);
                continue;
            }
            if(!(usbbuf[9] == 0x05 && usbbuf[10] == 0x00)) {
                continue;
            }
            int rlen = ((usbbuf[6] << 8) + usbbuf[5]);
            if(rlen == 52) {
                std::string hex;
                scard_hex(&usbbuf[28], 16, hex);
                blocks.push_back(hex);
                scard_hex(&usbbuf[28 + 16], 16, hex);
                blocks.push_back(hex);
                if(cmd[20] >= 0xFD) break; /* block numbers are one byte */
                cmd[18] = cmd[18] + 2;
                cmd[20] = cmd[20] + 2;
            }
            if(blocks.size() < maxBlocks) {
                packet_write(&devinfo, cmd, sizeof(cmd), &usbbuf, LIBUSB_API_TIMEOUT);//InCommRF
            }
        }

        if(blocks.size() > maxBlocks) {
            blocks.resize(maxBlocks);
        }
    }

    void close() {
        if(device) {
            if(claimed) {
                libusb_release_interface(device, 0);
                claimed = false;
            }
            libusb_close(device);
            device = NULL;
        }
        if(libusbReady) {
            libusb_exit(NULL);
            libusbReady = false;
        }
    }
};

#pragma mark - macOS: Sony RC-S310/320/330 over libpafe

class RCS330Reader : public CardReader {

    pasori *p;
    felica *f;

public:

    RCS330Reader() : p(NULL), f(NULL) {}
    ~RCS330Reader() { close(); }

    bool open(const ReadOptions& options, std::string& errorMessage) {
        p = pasori_open();
        if(!p) {
            errorMessage = "pasori_open() failed";
            return false;
        }
        if(pasori_init(p)) {
            errorMessage = "pasori_init() failed";
            return false;
        }
        pasori_set_timeout(p, LIBUSB_API_TIMEOUT);
        pasori_reset(p);
        return true;
    }

    bool poll(const ReadOptions& options, time_t deadline,
              const std::atomic<bool>& cancelled, ReadResult& result) {

        /* FeliCa only; the previous implementation polled once and ignored timeout */
        while (!deadline_passed(deadline, cancelled)) {
            f = felica_polling(p, FELICA_POLLING_ANY, 0, 0);
            if(f) {
                scard_hex(f->IDm, 8, result.IDm);
                scard_hex(f->PMm, 8, result.PMm);
                return true;
            }
            usleep(LIBUSB_USLEEP_DURATION);
        }
        return false;
    }

    void readService(uint16_t serviceCode, unsigned int maxBlocks, time_t deadline,
                     const std::atomic<bool>& cancelled, std::vector<std::string>& blocks) {

        if(!f) return;

        /*
         Request System Code: list the card's system codes, then look for the
         service under the first transit one.
         0003 : transit IC cards (Suica, PASMO, ...)
         80DE : IruCa
         */
        uint16 resp[256];
        int n = sizeof(resp)/sizeof(*resp);
        if(felica_request_system(f, &n, resp)) return;

        for (int i = 0; i < n; ++i) {

            if((resp[i] != 0x0003) && (resp[i] != 0x80DE)) continue;

            felica *ff = felica_polling(p, resp[i], 0, 0);
            if(!ff) break;

            /* Search Service Code until 0xffff is returned */
            if(!felica_search_service(ff)) {
                for (int j = 0; j < ff->service_num; ++j) {
                    if(ff->service[j].bin != serviceCode) continue;
                    if(ff->service[j].attr & 1) { /* readable without encryption */
                        uint8_t b[16];
                        for (unsigned int k = 0; (k < maxBlocks) && (k < 256); ++k) {
                            if(deadline_passed(deadline, cancelled)) break;
                            if(felica_read_single(ff, ff->service[j].bin, 0, (uint8)k, b)) break;
                            std::string hex;
                            scard_hex(b, 16, hex);
                            blocks.push_back(hex);
                        }
                    }
                    break;
                }
            }
            free(ff);
            break;
        }
    }

    void close() {
        if(f) {
            free(f);
            f = NULL;
        }
        if(p) {
            pasori_close(p);
            p = NULL;
        }
    }
};

#endif /* __APPLE__ */

#ifdef _WIN32

#pragma mark - Windows: PC/SC

class PCSCReader : public CardReader {

    std::wstring readerName;
    SCARDCONTEXT hContext;
    SCARDHANDLE hCard;
    DWORD activeProtocol;
    bool contextReady;
    bool connected;

    bool transmit(const BYTE *apdu, DWORD length, std::vector<BYTE>& response) {
        BYTE buf[2048];
        DWORD received = sizeof(buf);
        LPCSCARD_IO_REQUEST pci = (activeProtocol == SCARD_PROTOCOL_T0) ? SCARD_PCI_T0 : SCARD_PCI_T1;
        LONG lResult = SCardTransmit(hCard, pci, apdu, length, NULL, buf, &received);
        if(lResult != SCARD_S_SUCCESS) {
            response.clear();
            return false;
        }
        response.assign(buf, buf + received);
        return true;
    }

    static bool statusOK(const std::vector<BYTE>& response) {
        size_t n = response.size();
        return (n >= 2) && (response[n - 2] == 0x90) && (response[n - 1] == 0x00);
    }

    bool getData(BYTE p1, std::string& hex) {
        BYTE apdu[5] = {APDU_CLA_GENERIC, APDU_INS_GET_DATA, p1, APDU_P2_NONE, APDU_LE_MAX_LENGTH};
        std::vector<BYTE> response;
        if(transmit(apdu, sizeof(apdu), response) && statusOK(response)) {
            size_t length = response.size() - 2;
            scard_hex(response.data(), length < 8 ? length : 8, hex);
            return true;
        }
        return false;
    }

public:

    PCSCReader(const std::string& name) : hContext(0), hCard(0), activeProtocol(0), contextReady(false), connected(false) {
        int len = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), (int)name.length(), NULL, 0);
        if(len > 0) {
            std::vector<wchar_t> buf(len);
            MultiByteToWideChar(CP_UTF8, 0, name.c_str(), (int)name.length(), &buf[0], len);
            readerName.assign(&buf[0], len);
        }
    }
    ~PCSCReader() { close(); }

    bool open(const ReadOptions& options, std::string& errorMessage) {
        if(readerName.empty()) {
            errorMessage = "slotName is empty";
            return false;
        }
        DWORD scope = SCARD_SCOPE_USER;
        LONG lResult = SCardEstablishContext(scope, NULL, NULL, &hContext);
        /* http://eternalwindows.jp/security/scard/scard02.html */
        if (lResult == SCARD_E_NO_SERVICE) {
            HANDLE hEvent = SCardAccessStartedEvent();
            DWORD dwResult = WaitForSingleObject(hEvent, DEFAULT_TIMEOUT_MS_FOR_RESOURCE_MANAGER);
            if (dwResult == WAIT_OBJECT_0) {
                lResult = SCardEstablishContext(scope, NULL, NULL, &hContext);
            }
            SCardReleaseStartedEvent();
        }
        if(lResult != SCARD_S_SUCCESS) {
            errorMessage = "SCardEstablishContext() failed";
            return false;
        }
        contextReady = true;
        return true;
    }

    bool poll(const ReadOptions& options, time_t deadline,
              const std::atomic<bool>& cancelled, ReadResult& result) {

        SCARD_READERSTATE readerState;
        memset(&readerState, 0, sizeof(readerState));
        readerState.szReader = readerName.c_str();
        readerState.dwCurrentState = SCARD_STATE_UNAWARE;
        /* return immediately; check state */
        if(SCardGetStatusChange(hContext, 0, &readerState, 1) != SCARD_S_SUCCESS) {
            result.errorMessage = "SCardGetStatusChange() failed";
            return false;
        }

        bool present = false;
        while (!deadline_passed(deadline, cancelled)) {
            if (readerState.dwEventState & SCARD_STATE_UNAVAILABLE) {
                break;
            }
            if (readerState.dwEventState & SCARD_STATE_PRESENT) {
                present = true;
                break;
            }
            /* wait for a change; with dwCurrentState left at UNAWARE the call returns at once */
            readerState.dwCurrentState = readerState.dwEventState & ~SCARD_STATE_CHANGED;
            LONG lResult = SCardGetStatusChange(hContext, LIBPCSC_API_TIMEOUT, &readerState, 1);
            if((lResult != SCARD_S_SUCCESS) && (lResult != SCARD_E_TIMEOUT)) {
                break;
            }
        }
        if(!present) {
            return false;
        }

        if(SCardConnect(hContext, readerName.c_str(), SCARD_SHARE_SHARED,
                        SCARD_PROTOCOL_T0|SCARD_PROTOCOL_T1, &hCard, &activeProtocol) != SCARD_S_SUCCESS) {
            result.errorMessage = "SCardConnect() failed";
            return false;
        }
        connected = true;

        bool found = getData(APDU_P1_GET_UID, result.IDm);
        found = getData(APDU_P1_GET_PMm, result.PMm) || found;
        return found;
    }

    void readService(uint16_t serviceCode, unsigned int maxBlocks, time_t deadline,
                     const std::atomic<bool>& cancelled, std::vector<std::string>& blocks) {
        if(!connected) return;
        /* SELECT FILE takes the service code little-endian. scard-v3 sent this form only
           to the RC-S380 and a malformed one (no Lc) to every other reader. */
        BYTE select[7] = {APDU_CLA_GENERIC, APDU_INS_SELECT_FILE, 0x00, 0x01, 0x02,
            (BYTE)(serviceCode & 0xFF), (BYTE)(serviceCode >> 8)};
        for (unsigned int i = 0; (i < maxBlocks) && (i < 256); ++i) {
            if(deadline_passed(deadline, cancelled)) break;
            std::vector<BYTE> response;
            if(!transmit(select, sizeof(select), response)) break;
            BYTE read[5] = {APDU_CLA_GENERIC, APDU_INS_READ_BINARY, 0x00, (BYTE)i, 0x00};
            if(!transmit(read, sizeof(read), response)) break;
            if((response.size() < 18) || !statusOK(response)) break; /* 16 bytes + SW1 SW2 */
            std::string hex;
            scard_hex(response.data(), 16, hex);
            blocks.push_back(hex);
        }
    }

    void close() {
        if(connected) {
            SCardDisconnect(hCard, SCARD_LEAVE_CARD);
            connected = false;
        }
        if(contextReady) {
            SCardReleaseContext(hContext);
            contextReady = false;
        }
    }
};

#endif /* _WIN32 */

#pragma mark - factory

std::unique_ptr<CardReader> scard_create_reader(const std::string& slotName) {

#ifdef __APPLE__
    /* names given by SCARD Get readers to the Sony readers it opens over USB */
    if(slotName == "Sony FeliCa RC-S380") {
        return std::unique_ptr<CardReader>(new RCS380Reader());
    }
    if((slotName == "Sony FeliCa RC-S310")
       || (slotName == "Sony FeliCa RC-S320")
       || (slotName == "Sony FeliCa RC-S330")) {
        return std::unique_ptr<CardReader>(new RCS330Reader());
    }
    return std::unique_ptr<CardReader>(new TKReader(slotName));
#else
    return std::unique_ptr<CardReader>(new PCSCReader(slotName));
#endif
}
