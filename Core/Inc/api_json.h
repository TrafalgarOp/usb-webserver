//
// Created by meyti on 11/20/26.
//
// Minimal bounded JSON writer and reader. No allocation, no printf. The writer
// appends into a caller-provided fixed buffer and latches an overflow flag; the
// reader scans a caller-provided buffer in place and latches an error flag.
//

#ifndef USB_WEBSERVER_API_JSON_H
#define USB_WEBSERVER_API_JSON_H

#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Writer                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct
{
    char    *buf;
    uint16_t cap;
    uint16_t len;
    uint8_t  overflow;
} JSON_Writer_t;

void     JSON_Init(JSON_Writer_t *w, char *buf, uint16_t cap);
void     JSON_Raw(JSON_Writer_t *w, const char *s);
void     JSON_Char(JSON_Writer_t *w, char c);
void     JSON_String(JSON_Writer_t *w, const char *s);
void     JSON_UInt(JSON_Writer_t *w, uint32_t value);
void     JSON_Bool(JSON_Writer_t *w, uint8_t value);

uint16_t JSON_Len(const JSON_Writer_t *w);
uint8_t  JSON_Overflowed(const JSON_Writer_t *w);

/* ------------------------------------------------------------------------- */
/* Reader                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Deliberately narrow: it parses one flat JSON object whose members have
 * string or integer values. That is the entire request surface for the mutation
 * endpoints, so a full JSON parser would be extra attack surface for no gain.
 *
 * Strictness (all reported as an error, so the caller answers 400 INVALID_JSON):
 *   - exactly one object at the top level;
 *   - no trailing data after the object;
 *   - no duplicate handling here — the caller tracks that per field;
 *   - no unknown value kinds: only quoted strings and integers are read;
 *   - strings and keys are bounded by the destination buffer.
 */
typedef struct
{
    const char *buf;
    uint16_t    len;
    uint16_t    pos;
    uint8_t     first;     /* inside an object, no member consumed yet */
    uint8_t     error;     /* latched on any syntax problem */
    uint8_t     truncated; /* last string did not fit its destination */
} JSON_Reader_t;

typedef enum
{
    JSON_MEMBER_KEY = 0, /* a key was read; its value token is next */
    JSON_MEMBER_END,     /* the closing '}' was consumed */
    JSON_MEMBER_ERROR
} JSON_Member_t;

void          JSON_ReaderInit(JSON_Reader_t *r, const char *buf, uint16_t len);
uint8_t       JSON_ReadObjectStart(JSON_Reader_t *r);
JSON_Member_t JSON_ReadKey(JSON_Reader_t *r, char *key, uint16_t key_cap);
uint8_t       JSON_ReadStringValue(JSON_Reader_t *r, char *out, uint16_t out_cap);
uint8_t       JSON_ReadIntValue(JSON_Reader_t *r, int32_t *out);
uint8_t       JSON_ReadDone(JSON_Reader_t *r);
uint8_t       JSON_ReaderError(const JSON_Reader_t *r);
uint8_t       JSON_ReaderTruncated(const JSON_Reader_t *r);

#endif // USB_WEBSERVER_API_JSON_H
