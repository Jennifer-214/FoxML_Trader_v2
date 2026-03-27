// Copyright (c) 2026 Jennifer Lewis. All rights reserved.
// Licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
// See LICENSE file in the project root for full license text.

//======================================================================================================
// [FAUX FIX PROTOCOL]
//======================================================================================================
// simplified FIX 4.4 market data parser - real FIX uses SOH (0x01) as delimiter between tag=value
// pairs, we use the same format but also support '|' as a stand-in for readability in test data
//
// real FIX market data snapshot (MsgType 35=W) looks like:
//   8=FIX.4.4 SOH 9=126 SOH 35=W SOH 49=EXCHANGE SOH 56=CLIENT SOH 34=42 SOH
//   52=20260318-14:30:00.000 SOH 55=AAPL SOH 268=1 SOH 269=2 SOH 270=187.4500 SOH
//   271=1500 SOH 10=128 SOH
//
// tag reference (subset we care about):
//   8   = BeginString       (protocol version, always "FIX.4.4")
//   9   = BodyLength        (byte count after tag 9 up to but not including tag 10)
//   10  = CheckSum          (modulo 256 sum of all bytes up to but not including tag 10)
//   34  = MsgSeqNum         (sequence number, monotonically increasing per session)
//   35  = MsgType           (W = market data snapshot, X = incremental refresh, D = new order)
//   49  = SenderCompID      (who sent this)
//   52  = SendingTime       (UTC timestamp YYYYMMDD-HH:MM:SS.sss)
//   55  = Symbol            (ticker, e.g. "AAPL")
//   56  = TargetCompID      (who receives this)
//   268 = NoMDEntries       (how many market data entries follow)
//   269 = MDEntryType       (0=Bid, 1=Offer, 2=Trade)
//   270 = MDEntryPx         (price)
//   271 = MDEntrySize       (volume/quantity)
//
// we only parse 270 and 271 into the DataStream struct since thats what the gates and regression
// consume, but we validate the checksum and extract the other fields into the parsed message struct
// so you could extend this later for order routing or whatever
//======================================================================================================
#ifndef FAUX_FIX_HPP
#define FAUX_FIX_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include <stdint.h>
#include <string.h>

//======================================================================================================
// [CONSTANTS]
//======================================================================================================
#define FIX_SOH '\x01'
#define FIX_MAX_SYMBOL_LEN 8
#define FIX_MAX_FIELDS 32
#define FIX_MAX_MSG_LEN 512

//======================================================================================================
// [PARSED MESSAGE STRUCT]
//======================================================================================================
// holds the extracted fields from a single FIX message, symbol is null-terminated
// msg_type: 'W' = snapshot, 'X' = incremental, 'D' = new order single
// entry_type: 0 = bid, 1 = offer, 2 = trade
//======================================================================================================
struct FIX_ParsedMessage {
    char symbol[FIX_MAX_SYMBOL_LEN];
    char msg_type;      // tag 35
    uint8_t entry_type; // tag 269 (0=bid, 1=offer, 2=trade)
    uint32_t seq_num;   // tag 34
    double price;       // tag 270
    double volume;      // tag 271
    uint8_t checksum;   // tag 10
    int valid;          // 1 if parsed ok, 0 if malformed
};

//======================================================================================================
// [PARSER HELPERS]
//======================================================================================================
// parse a single tag=value pair, advancing the pointer past the delimiter
// returns the tag number, writes value start and length into out params
//======================================================================================================
static inline int FIX_ParseTag(const char *msg, int *pos, int len, int *tag_out, const char **val_out, int *val_len_out) {
    // parse tag number
    int tag = 0;
    while (*pos < len && msg[*pos] >= '0' && msg[*pos] <= '9') {
        tag = tag * 10 + (msg[*pos] - '0');
        (*pos)++;
    }
    if (*pos >= len || msg[*pos] != '=')
        return 0;
    (*pos)++; // skip '='

    // value starts here
    *val_out   = &msg[*pos];
    int vstart = *pos;
    while (*pos < len && msg[*pos] != FIX_SOH && msg[*pos] != '|') {
        (*pos)++;
    }
    *val_len_out = *pos - vstart;
    if (*pos < len)
        (*pos)++; // skip delimiter

    *tag_out = tag;
    return 1;
}

//======================================================================================================
// [SIMPLE DOUBLE PARSER]
//======================================================================================================
// parses a decimal string into a double, handles optional sign and decimal point
// not meant to be fast, just correct enough for test data
//======================================================================================================
static inline double FIX_ParseDouble(const char *s, int len) {
    double result   = 0.0;
    double frac     = 0.0;
    double frac_div = 1.0;
    int neg         = 0;
    int i           = 0;
    int in_frac     = 0;

    if (i < len && s[i] == '-') {
        neg = 1;
        i++;
    }
    while (i < len) {
        if (s[i] == '.') {
            in_frac = 1;
            i++;
            continue;
        }
        if (s[i] < '0' || s[i] > '9')
            break;
        if (in_frac) {
            frac_div *= 10.0;
            frac += (double)(s[i] - '0') / frac_div;
        } else {
            result = result * 10.0 + (double)(s[i] - '0');
        }
        i++;
    }
    result += frac;
    return neg ? -result : result;
}

//======================================================================================================
// [INT PARSER]
//======================================================================================================
static inline uint32_t FIX_ParseUint(const char *s, int len) {
    uint32_t result = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            break;
        result = result * 10 + (uint32_t)(s[i] - '0');
    }
    return result;
}

//======================================================================================================
// [CHECKSUM]
//======================================================================================================
// real FIX checksum: sum of all bytes from tag 8 up to (not including) "10=" field, mod 256
// we compute it over the whole message up to the last delimiter before tag 10
//======================================================================================================
static inline uint8_t FIX_ComputeChecksum(const char *msg, int len) {
    // find "10=" - scan backwards from end
    int cs_start = -1;
    for (int i = len - 1; i >= 2; i--) {
        if (msg[i] == '=' && msg[i - 1] == '0' && msg[i - 2] == '1' && (i == 2 || msg[i - 3] == FIX_SOH || msg[i - 3] == '|')) {
            cs_start = i - 2;
            break;
        }
    }
    if (cs_start < 0)
        cs_start = len; // no checksum field found, sum everything

    uint8_t sum = 0;
    for (int i = 0; i < cs_start; i++) {
        sum += (uint8_t)msg[i];
    }
    return sum;
}

//======================================================================================================
// [PARSE MESSAGE]
//======================================================================================================
// takes a raw FIX message string (SOH or | delimited) and fills out a FIX_ParsedMessage
// validates checksum if tag 10 is present
//======================================================================================================
static inline FIX_ParsedMessage FIX_Parse(const char *msg, int len) {
    FIX_ParsedMessage parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.valid = 1;

    int pos = 0;
    int tag;
    const char *val;
    int val_len;
    int has_checksum = 0;

    while (pos < len && FIX_ParseTag(msg, &pos, len, &tag, &val, &val_len)) {
        switch (tag) {
        case 35: // MsgType
            if (val_len > 0)
                parsed.msg_type = val[0];
            break;
        case 34: // MsgSeqNum
            parsed.seq_num = FIX_ParseUint(val, val_len);
            break;
        case 55: // Symbol
        {
            int copy_len = val_len < FIX_MAX_SYMBOL_LEN - 1 ? val_len : FIX_MAX_SYMBOL_LEN - 1;
            memcpy(parsed.symbol, val, copy_len);
            parsed.symbol[copy_len] = '\0';
        } break;
        case 269: // MDEntryType
            parsed.entry_type = (uint8_t)FIX_ParseUint(val, val_len);
            break;
        case 270: // MDEntryPx (price)
            parsed.price = FIX_ParseDouble(val, val_len);
            break;
        case 271: // MDEntrySize (volume)
            parsed.volume = FIX_ParseDouble(val, val_len);
            break;
        case 10: // CheckSum
            parsed.checksum = (uint8_t)FIX_ParseUint(val, val_len);
            has_checksum    = 1;
            break;
        }
    }

    // validate checksum if present
    if (has_checksum) {
        uint8_t computed = FIX_ComputeChecksum(msg, len);
        if (computed != parsed.checksum) {
            parsed.valid = 0;
        }
    }

    return parsed;
}

//======================================================================================================
// [CONVERT TO DATASTREAM]
//======================================================================================================
// takes a parsed FIX message and converts price/volume into a DataStream<F> for the gates and
// regression pipeline, only makes sense for trade entries (269=2) but we convert regardless and
// let the caller filter by entry_type if they care
//======================================================================================================
template <unsigned F> inline DataStream<F> FIX_ToDataStream(const FIX_ParsedMessage *msg) {
    DataStream<F> stream;
    stream.price  = FPN_FromDouble<F>(msg->price);
    stream.volume = FPN_FromDouble<F>(msg->volume);
    return stream;
}

//======================================================================================================
// [BUILD FIX MESSAGE]
//======================================================================================================
// constructs a FIX market data snapshot string with proper checksum, returns length written
// this is the faux exchange side - generates what a real exchange would send you
// writes into buf which must be at least FIX_MAX_MSG_LEN bytes
//======================================================================================================
static inline int FIX_BuildMarketDataMsg(char *buf, int buf_size, uint32_t seq_num, const char *symbol, uint8_t entry_type, double price,
                                         double volume) {
    // build body first (everything between tag 8 and tag 10)
    char body[FIX_MAX_MSG_LEN];
    int pos = 0;

// helper to append a string
#define FIX_APPEND(s)                                                                                                                      \
    do {                                                                                                                                   \
        const char *_s = (s);                                                                                                              \
        while (*_s && pos < FIX_MAX_MSG_LEN - 1)                                                                                           \
            body[pos++] = *_s++;                                                                                                           \
    } while (0)

// helper to append tag=value|
#define FIX_TAG_STR(tag, val)                                                                                                              \
    do {                                                                                                                                   \
        FIX_APPEND(tag "=");                                                                                                               \
        FIX_APPEND(val);                                                                                                                   \
        body[pos++] = FIX_SOH;                                                                                                             \
    } while (0)

    // convert numbers to strings inline (small buffer on stack)
    char num_buf[32];

    FIX_TAG_STR("8", "FIX.4.4");

    // tag 9 (body length) - we'll patch this after
    int body_len_pos = pos;
    FIX_APPEND("9=000");
    body[pos++]    = FIX_SOH;
    int body_start = pos;

    FIX_TAG_STR("35", "W");

    // seq num
    {
        int n      = 0;
        uint32_t v = seq_num;
        do {
            num_buf[n++] = '0' + (v % 10);
            v /= 10;
        } while (v > 0);
        FIX_APPEND("34=");
        for (int i = n - 1; i >= 0; i--)
            body[pos++] = num_buf[i];
        body[pos++] = FIX_SOH;
    }

    // symbol
    FIX_APPEND("55=");
    FIX_APPEND(symbol);
    body[pos++] = FIX_SOH;

    // entries count
    FIX_TAG_STR("268", "1");

    // entry type
    FIX_APPEND("269=");
    body[pos++] = '0' + entry_type;
    body[pos++] = FIX_SOH;

    // price - format to 4 decimal places
    {
        int neg            = (price < 0.0);
        double ap          = neg ? -price : price;
        uint64_t int_part  = (uint64_t)ap;
        uint64_t frac_part = (uint64_t)((ap - (double)int_part) * 10000.0 + 0.5);
        if (frac_part >= 10000) {
            int_part++;
            frac_part -= 10000;
        }

        FIX_APPEND("270=");
        if (neg)
            body[pos++] = '-';
        // integer part
        int n      = 0;
        uint64_t v = int_part;
        do {
            num_buf[n++] = '0' + (v % 10);
            v /= 10;
        } while (v > 0);
        for (int i = n - 1; i >= 0; i--)
            body[pos++] = num_buf[i];
        body[pos++] = '.';
        // fractional part (4 digits)
        body[pos++] = '0' + (char)((frac_part / 1000) % 10);
        body[pos++] = '0' + (char)((frac_part / 100) % 10);
        body[pos++] = '0' + (char)((frac_part / 10) % 10);
        body[pos++] = '0' + (char)(frac_part % 10);
        body[pos++] = FIX_SOH;
    }

    // volume
    {
        FIX_APPEND("271=");
        uint64_t v = (uint64_t)volume;
        int n      = 0;
        do {
            num_buf[n++] = '0' + (v % 10);
            v /= 10;
        } while (v > 0);
        for (int i = n - 1; i >= 0; i--)
            body[pos++] = num_buf[i];
        body[pos++] = FIX_SOH;
    }

    // patch body length (bytes from after tag 9 to before tag 10)
    int body_len = pos - body_start;
    {
        char len_str[4];
        len_str[0] = '0' + (body_len / 100) % 10;
        len_str[1] = '0' + (body_len / 10) % 10;
        len_str[2] = '0' + body_len % 10;
        // overwrite the "000" we placed earlier
        body[body_len_pos + 2] = len_str[0];
        body[body_len_pos + 3] = len_str[1];
        body[body_len_pos + 4] = len_str[2];
    }

    // compute checksum over everything so far
    uint8_t sum = 0;
    for (int i = 0; i < pos; i++)
        sum += (uint8_t)body[i];

    // append tag 10
    FIX_APPEND("10=");
    body[pos++] = '0' + (sum / 100) % 10;
    body[pos++] = '0' + (sum / 10) % 10;
    body[pos++] = '0' + sum % 10;
    body[pos++] = FIX_SOH;

#undef FIX_APPEND
#undef FIX_TAG_STR

    // copy to output
    int out_len = pos < buf_size - 1 ? pos : buf_size - 1;
    memcpy(buf, body, out_len);
    buf[out_len] = '\0';
    return out_len;
}

//======================================================================================================
//======================================================================================================
#endif // FAUX_FIX_HPP
