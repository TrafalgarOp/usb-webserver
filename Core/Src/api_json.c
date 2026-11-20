//
// Created by meyti on 11/20/26.
//

#include "api_json.h"

/* ------------------------------------------------------------------------- */
/* Writer                                                                     */
/* ------------------------------------------------------------------------- */

void JSON_Init(JSON_Writer_t *w, char *buf, uint16_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;

    if (cap > 0)
    {
        w->buf[0] = '\0';
    }
}

void JSON_Char(JSON_Writer_t *w, char c)
{
    /* Reserve one byte for the NUL terminator. */
    if (w->overflow || (uint32_t)w->len + 1 >= w->cap)
    {
        w->overflow = 1;
        return;
    }

    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
}

void JSON_Raw(JSON_Writer_t *w, const char *s)
{
    while (*s != '\0')
    {
        JSON_Char(w, *s++);
    }
}

void JSON_String(JSON_Writer_t *w, const char *s)
{
    JSON_Char(w, '"');

    for (; *s != '\0'; s++)
    {
        unsigned char c = (unsigned char)*s;

        if (c == '"' || c == '\\')
        {
            JSON_Char(w, '\\');
            JSON_Char(w, (char)c);
        }
        else if (c < 0x20)
        {
            /* Control characters do not occur in the values this API emits.
             * Substitute a space so the output stays valid JSON. */
            JSON_Char(w, ' ');
        }
        else
        {
            JSON_Char(w, (char)c);
        }
    }

    JSON_Char(w, '"');
}

void JSON_UInt(JSON_Writer_t *w, uint32_t value)
{
    char tmp[10];
    uint8_t n = 0;

    if (value == 0)
    {
        JSON_Char(w, '0');
        return;
    }

    while (value > 0 && n < sizeof tmp)
    {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (n > 0)
    {
        JSON_Char(w, tmp[--n]);
    }
}

void JSON_Bool(JSON_Writer_t *w, uint8_t value)
{
    JSON_Raw(w, value ? "true" : "false");
}

uint16_t JSON_Len(const JSON_Writer_t *w)
{
    return w->len;
}

uint8_t JSON_Overflowed(const JSON_Writer_t *w)
{
    return w->overflow;
}

/* ------------------------------------------------------------------------- */
/* Reader                                                                     */
/* ------------------------------------------------------------------------- */

void JSON_ReaderInit(JSON_Reader_t *r, const char *buf, uint16_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->first = 1;
    r->error = 0;
    r->truncated = 0;
}

uint8_t JSON_ReaderError(const JSON_Reader_t *r)
{
    return r->error;
}

uint8_t JSON_ReaderTruncated(const JSON_Reader_t *r)
{
    return r->truncated;
}

static void jr_fail(JSON_Reader_t *r)
{
    r->error = 1;
}

static void jr_skip_ws(JSON_Reader_t *r)
{
    while (r->pos < r->len)
    {
        char c = r->buf[r->pos];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            r->pos++;
        }
        else
        {
            break;
        }
    }
}

/* Scan a JSON string into out[0..cap). Returns 1 on success. */
static uint8_t jr_string(JSON_Reader_t *r, char *out, uint16_t cap)
{
    uint16_t n = 0;

    r->truncated = 0;
    jr_skip_ws(r);

    if (r->pos >= r->len || r->buf[r->pos] != '"')
    {
        jr_fail(r);
        return 0;
    }
    r->pos++;

    while (r->pos < r->len)
    {
        char c = r->buf[r->pos++];

        if (c == '"')
        {
            if (n >= cap)
            {
                jr_fail(r);
                return 0;
            }
            out[n] = '\0';
            return 1;
        }

        if (c == '\\')
        {
            char e;

            if (r->pos >= r->len)
            {
                jr_fail(r);
                return 0;
            }
            e = r->buf[r->pos++];

            switch (e)
            {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;

                case 'u':
                {
                    uint32_t cp = 0;

                    if (r->pos + 4 > r->len)
                    {
                        jr_fail(r);
                        return 0;
                    }

                    for (uint8_t i = 0; i < 4; i++)
                    {
                        char h = r->buf[r->pos++];

                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= (uint32_t)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
                        else
                        {
                            jr_fail(r);
                            return 0;
                        }
                    }

                    /* The accepted field values are ASCII; a code point that
                     * cannot be represented is rejected rather than truncated. */
                    if (cp > 0x7F)
                    {
                        jr_fail(r);
                        return 0;
                    }
                    c = (char)cp;
                    break;
                }

                default:
                    jr_fail(r);
                    return 0;
            }
        }
        else if ((unsigned char)c < 0x20)
        {
            jr_fail(r); /* raw control character in a string */
            return 0;
        }

        if (n + 1 < cap)
        {
            out[n++] = c;
        }
        else
        {
            /* The value does not fit its destination. Keep consuming so the
             * document stays parseable; an over-long value can never equal a
             * supported one, so the caller maps it to an unsupported value
             * rather than to a JSON syntax error. */
            r->truncated = 1;
        }
    }

    jr_fail(r); /* unterminated string */
    return 0;
}

uint8_t JSON_ReadObjectStart(JSON_Reader_t *r)
{
    if (r->error)
    {
        return 0;
    }

    jr_skip_ws(r);

    if (r->pos >= r->len || r->buf[r->pos] != '{')
    {
        jr_fail(r);
        return 0;
    }

    r->pos++;
    r->first = 1;
    return 1;
}

JSON_Member_t JSON_ReadKey(JSON_Reader_t *r, char *key, uint16_t key_cap)
{
    if (r->error)
    {
        return JSON_MEMBER_ERROR;
    }

    jr_skip_ws(r);

    if (r->pos >= r->len)
    {
        jr_fail(r);
        return JSON_MEMBER_ERROR;
    }

    if (r->buf[r->pos] == '}')
    {
        r->pos++;
        return JSON_MEMBER_END;
    }

    if (!r->first)
    {
        if (r->buf[r->pos] != ',')
        {
            jr_fail(r);
            return JSON_MEMBER_ERROR;
        }
        r->pos++;
        jr_skip_ws(r);

        if (r->pos >= r->len || r->buf[r->pos] == '}')
        {
            jr_fail(r); /* trailing comma */
            return JSON_MEMBER_ERROR;
        }
    }
    r->first = 0;

    if (!jr_string(r, key, key_cap))
    {
        return JSON_MEMBER_ERROR;
    }

    jr_skip_ws(r);
    if (r->pos >= r->len || r->buf[r->pos] != ':')
    {
        jr_fail(r);
        return JSON_MEMBER_ERROR;
    }
    r->pos++;

    return JSON_MEMBER_KEY;
}

uint8_t JSON_ReadStringValue(JSON_Reader_t *r, char *out, uint16_t out_cap)
{
    if (r->error)
    {
        return 0;
    }

    return jr_string(r, out, out_cap);
}

uint8_t JSON_ReadIntValue(JSON_Reader_t *r, int32_t *out)
{
    uint8_t neg = 0;
    uint8_t digits = 0;
    uint16_t start;
    uint32_t v = 0;

    if (r->error)
    {
        return 0;
    }

    jr_skip_ws(r);

    if (r->pos < r->len && r->buf[r->pos] == '-')
    {
        neg = 1;
        r->pos++;
    }

    start = r->pos;

    while (r->pos < r->len && r->buf[r->pos] >= '0' && r->buf[r->pos] <= '9')
    {
        if (v <= 100000000u) /* keep v*10 within range; saturate above it */
        {
            v = v * 10u + (uint32_t)(r->buf[r->pos] - '0');
        }
        r->pos++;
        digits = 1;
    }

    if (!digits)
    {
        jr_fail(r);
        return 0;
    }

    /* JSON forbids leading zeros. */
    if (r->buf[start] == '0' && r->pos - start > 1)
    {
        jr_fail(r);
        return 0;
    }

    /* An integer field must be an integer: reject fraction and exponent. */
    if (r->pos < r->len &&
        (r->buf[r->pos] == '.' || r->buf[r->pos] == 'e' || r->buf[r->pos] == 'E'))
    {
        jr_fail(r);
        return 0;
    }

    *out = neg ? -(int32_t)v : (int32_t)v;
    return 1;
}

uint8_t JSON_ReadDone(JSON_Reader_t *r)
{
    if (r->error)
    {
        return 0;
    }

    jr_skip_ws(r);
    return r->pos == r->len;
}
