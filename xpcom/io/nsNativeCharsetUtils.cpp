/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2002
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Darin Fisher <darin@netscape.com>
 *   Brian Stell <bstell@ix.netcom.com>
 *   Frank Tang <ftang@netscape.com>
 *   Brendan Eich <brendan@mozilla.org>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#if defined(XP_UNIX) || defined(XP_BEOS)

#include <stdlib.h>   // mbtowc, wctomb
#include <locale.h>   // setlocale
#include "nscore.h"
#include "prlock.h"
#include "nsAString.h"

//
// choose a conversion library.  under linux we prefer using wcrtomb/mbrtowc
// to improve performance.  other platforms in which wchar_t is unicode might
// benefit from this optimization as well.
//
#if defined(__linux) && defined(HAVE_WCRTOMB) && defined(HAVE_MBRTOWC)
#define USE_STDCONV 1
#elif defined(HAVE_ICONV) && defined(HAVE_NL_TYPES_H) && defined(HAVE_NL_LANGINFO)
#define USE_ICONV 1
#else
#define USE_STDCONV 1
#endif

static void
isolatin1_to_ucs2(const char **input, PRUint32 *inputLeft, PRUnichar **output, PRUint32 *outputLeft)
{
    while (*inputLeft && *outputLeft) {
        **output = (unsigned char) **input;
        (*input)++;
        (*inputLeft)--;
        (*output)++;
        (*outputLeft)--;
    }
}

static void
ucs2_to_isolatin1(const PRUnichar **input, PRUint32 *inputLeft, char **output, PRUint32 *outputLeft)
{
    while (*inputLeft && *outputLeft) {
        **output = (unsigned char) **input;
        (*input)++;
        (*inputLeft)--;
        (*output)++;
        (*outputLeft)--;
    }
}

//-----------------------------------------------------------------------------
// conversion using iconv
//-----------------------------------------------------------------------------
#if defined(USE_ICONV)
#include <nl_types.h> // CODESET
#include <langinfo.h> // nl_langinfo
#include <iconv.h>    // iconv_open, iconv, iconv_close
#include <errno.h>

#if defined(HAVE_ICONV_WITH_CONST_INPUT)
#define ICONV_INPUT(x) (x)
#else
#define ICONV_INPUT(x) ((char **)x)
#endif

// solaris definitely needs this, but we'll enable it by default
// just in case...
#define ENABLE_UTF8_FALLBACK_SUPPORT

#define INVALID_ICONV_T ((iconv_t) -1)

static inline size_t
xp_iconv(iconv_t converter,
         const char **input,
         size_t      *inputLeft,
         char       **output,
         size_t      *outputLeft)
{
    size_t res, outputAvail = outputLeft ? *outputLeft : 0;
    res = iconv(converter, ICONV_INPUT(input), inputLeft, output, outputLeft);
    if (res == (size_t) -1) {
        // on some platforms (e.g., linux) iconv will fail with
        // E2BIG if it cannot convert _all_ of its input.  it'll
        // still adjust all of the in/out params correctly, so we
        // can ignore this error.  the assumption is that we will
        // be called again to complete the conversion.
        if ((errno == E2BIG) && (*outputLeft < outputAvail))
            res = 0;
    }
    return res;
}

static inline iconv_t
xp_iconv_open(const char **to_list, const char **from_list)
{
    iconv_t res;
    const char **from_name;
    const char **to_name;

    // try all possible combinations to locate a converter.
    to_name = to_list;
    while (*to_name) {
        if (**to_name) {
            from_name = from_list;
            while (*from_name) {
                if (**from_name) {
                    res = iconv_open(*to_name, *from_name);
                    if (res != INVALID_ICONV_T)
                        return res;
                }
                from_name++;
            }
        }
        to_name++;
    }

    return INVALID_ICONV_T;
}

static const char *UCS_2_NAMES[] = {
    "UCS-2",
    "UCS2",
    "UCS_2",
    "ucs-2",
    "ucs2",
    "ucs_2",
    NULL
};

static const char *UTF_8_NAMES[] = {
    "UTF-8",
    "UTF8",
    "UTF_8",
    "utf-8",
    "utf8",
    "utf_8",
    NULL
};

static const char *ISO_8859_1_NAMES[] = {
    "ISO-8859-1",
    "ISO8859-1",
    "ISO88591",
    "ISO_8859_1",
    "ISO8859_1",
    "iso-8859-1",
    "iso8859-1",
    "iso88591",
    "iso_8859_1",
    "iso8859_1",
    NULL
};

class nsNativeCharsetConverter
{
public:
    nsNativeCharsetConverter();
   ~nsNativeCharsetConverter();

    nsresult NativeToUnicode(const char      **input , PRUint32 *inputLeft,
                             PRUnichar       **output, PRUint32 *outputLeft);
    nsresult UnicodeToNative(const PRUnichar **input , PRUint32 *inputLeft,
                             char            **output, PRUint32 *outputLeft);

    static void GlobalInit();
    static void GlobalShutdown();

private:
    static iconv_t gNativeToUnicode;
    static iconv_t gUnicodeToNative;
#if defined(ENABLE_UTF8_FALLBACK_SUPPORT)
    static iconv_t gNativeToUTF8;
    static iconv_t gUTF8ToNative;
    static iconv_t gUnicodeToUTF8;
    static iconv_t gUTF8ToUnicode;
#endif
    static PRLock *gLock;
    static PRBool  gInitialized;

    static void LazyInit();

    static void Lock()   { if (gLock) PR_Lock(gLock);   }
    static void Unlock() { if (gLock) PR_Unlock(gLock); }
};

iconv_t nsNativeCharsetConverter::gNativeToUnicode = INVALID_ICONV_T;
iconv_t nsNativeCharsetConverter::gUnicodeToNative = INVALID_ICONV_T;
#if defined(ENABLE_UTF8_FALLBACK_SUPPORT)
iconv_t nsNativeCharsetConverter::gNativeToUTF8    = INVALID_ICONV_T;
iconv_t nsNativeCharsetConverter::gUTF8ToNative    = INVALID_ICONV_T;
iconv_t nsNativeCharsetConverter::gUnicodeToUTF8   = INVALID_ICONV_T;
iconv_t nsNativeCharsetConverter::gUTF8ToUnicode   = INVALID_ICONV_T;
#endif
PRLock *nsNativeCharsetConverter::gLock            = nsnull;
PRBool  nsNativeCharsetConverter::gInitialized     = PR_FALSE;

void
nsNativeCharsetConverter::LazyInit()
{
    const char  *blank_list[] = { "", NULL };
    const char **native_charset_list = blank_list;
    const char  *native_charset = nl_langinfo(CODESET);
    if (native_charset == nsnull) {
        NS_ERROR("native charset is unknown");
        // fallback to ISO-8859-1
        native_charset_list = ISO_8859_1_NAMES;
    }
    else
        native_charset_list[0] = native_charset;

    gNativeToUnicode = xp_iconv_open(UCS_2_NAMES, native_charset_list);
    gUnicodeToNative = xp_iconv_open(native_charset_list, UCS_2_NAMES);

#if defined(ENABLE_UTF8_FALLBACK_SUPPORT)
    if (gNativeToUnicode == INVALID_ICONV_T) {
        gNativeToUTF8 = xp_iconv_open(UTF_8_NAMES, native_charset_list);
        gUTF8ToUnicode = xp_iconv_open(UCS_2_NAMES, UTF_8_NAMES);
        NS_ASSERTION(gNativeToUTF8 != INVALID_ICONV_T, "no native to utf-8 converter");
        NS_ASSERTION(gUTF8ToUnicode != INVALID_ICONV_T, "no utf-8 to ucs-2 converter");
    }
    if (gUnicodeToNative == INVALID_ICONV_T) {
        gUnicodeToUTF8 = xp_iconv_open(UTF_8_NAMES, UCS_2_NAMES);
        gUTF8ToNative = xp_iconv_open(native_charset_list, UTF_8_NAMES);
        NS_ASSERTION(gUnicodeToUTF8 != INVALID_ICONV_T, "no unicode to utf-8 converter");
        NS_ASSERTION(gUTF8ToNative != INVALID_ICONV_T, "no utf-8 to native converter");
    }
#else
    NS_ASSERTION(gNativeToUnicode != INVALID_ICONV_T, "no native to ucs-2 converter");
    NS_ASSERTION(gUnicodeToNative != INVALID_ICONV_T, "no ucs-2 to native converter");
#endif

    gInitialized = PR_TRUE;
}

void
nsNativeCharsetConverter::GlobalInit()
{
    gLock = PR_NewLock();
    NS_ASSERTION(gLock, "lock creation failed");
}

void
nsNativeCharsetConverter::GlobalShutdown()
{
    if (gLock) {
        PR_DestroyLock(gLock);
        gLock = nsnull;
    }

    if (gNativeToUnicode != INVALID_ICONV_T) {
        iconv_close(gNativeToUnicode);
        gNativeToUnicode = INVALID_ICONV_T;
    }

    if (gUnicodeToNative != INVALID_ICONV_T) {
        iconv_close(gUnicodeToNative);
        gUnicodeToNative = INVALID_ICONV_T;
    }

#if defined(ENABLE_UTF8_FALLBACK_SUPPORT)
    if (gNativeToUTF8 != INVALID_ICONV_T) {
        iconv_close(gNativeToUTF8);
        gNativeToUTF8 = INVALID_ICONV_T;
    }
    if (gUTF8ToNative != INVALID_ICONV_T) {
        iconv_close(gUTF8ToNative);
        gUTF8ToNative = INVALID_ICONV_T;
    }
    if (gUnicodeToUTF8 != INVALID_ICONV_T) {
        iconv_close(gUnicodeToUTF8);
        gUnicodeToUTF8 = INVALID_ICONV_T;
    }
    if (gUTF8ToUnicode != INVALID_ICONV_T) {
        iconv_close(gUTF8ToUnicode);
        gUTF8ToUnicode = INVALID_ICONV_T;
    }
#endif

    gInitialized = PR_FALSE;
}

nsNativeCharsetConverter::nsNativeCharsetConverter()
{
    Lock();
    if (!gInitialized)
        LazyInit();
}

nsNativeCharsetConverter::~nsNativeCharsetConverter()
{
    // reset converters for next time
    if (gNativeToUnicode != INVALID_ICONV_T)
        xp_iconv(gNativeToUnicode, NULL, NULL, NULL, NULL);
    if (gUnicodeToNative != INVALID_ICONV_T)
        xp_iconv(gUnicodeToNative, NULL, NULL, NULL, NULL);
#if defined(ENABLE_UTF8_FALLBACK_SUPPORT)
    if (gNativeToUTF8 != INVALID_ICONV_T)
        xp_iconv(gNativeToUTF8, NULL, NULL, NULL, NULL);
    if (gUTF8ToNative != INVALID_ICONV_T)
        xp_iconv(gUTF8ToNative, NULL, NULL, NULL, NULL);
    if (gUnicodeToUTF8 != INVALID_ICONV_T)
        xp_iconv(gUnicodeToUTF8, NULL, NULL, NULL, NULL);
    if (gUTF8ToUnicode != INVALID_ICONV_T)
        xp_iconv(gUTF8ToUnicode, NULL, NULL, NULL, NULL);
#endif
    Unlock();
}

nsresult
nsNativeCharsetConverter::NativeToUnicode(const char **input,
                                          PRUint32    *inputLeft,
                                          PRUnichar  **output,
                                          PRUint32    *outputLeft)
{
    size_t res = 0;
    size_t inLeft = (size_t) *inputLeft;
    size_t outLeft = (size_t) *outputLeft * 2;

    if (gNativeToUnicode != INVALID_ICONV_T) {

        res = xp_iconv(gNativeToUnicode, input, &inLeft, (char **) output, &outLeft);

        if (res != (size_t) -1) {
            *inputLeft = inLeft;
            *outputLeft = outLeft / 2;
            return NS_OK;
        }

        NS_WARNING("conversion from native to ucs-2 failed");

        // reset converter
        xp_iconv(gNativeToUnicode, NULL, NULL, NULL, NULL);
    }
#if defined(ENABLE_UTF8_FALLBACK_SUPPORT)
    else if ((gNativeToUTF8 != INVALID_ICONV_T) &&
             (gUTF8ToUnicode != INVALID_ICONV_T)) {
        // convert first to UTF8, then from UTF8 to UCS2
        const char *in = *input;

        char ubuf[1024];

        // we assume we're always called with enough space in |output|,
        // so convert many chars at a time...
        while (inLeft) {
            char *p = ubuf;
            size_t n = sizeof(ubuf);
            res = xp_iconv(gNativeToUTF8, &in, &inLeft, &p, &n);
            if (res == (size_t) -1) {
                NS_ERROR("conversion from native to utf-8 failed");
                break;
            }
            NS_ASSERTION(outLeft > 0, "bad assumption");
            p = ubuf;
            n = sizeof(ubuf) - n;
            res = xp_iconv(gUTF8ToUnicode, (const char **) &p, &n, (char **) output, &outLeft);
            if (res == (size_t) -1) {
                NS_ERROR("conversion from utf-8 to ucs-2 failed");
                break;
            }
        }

        if (res != (size_t) -1) {
            (*input) += (*inputLeft - inLeft);
            *inputLeft = inLeft;
            *outputLeft = outLeft / 2;
            return NS_OK;
        }

        // reset converters
        xp_iconv(gNativeToUTF8, NULL, NULL, NULL, NULL);
        xp_iconv(gUTF8ToUnicode, NULL, NULL, NULL, NULL);
    }
#endif

    // fallback: zero-pad and hope for the best
    isolatin1_to_ucs2(input, inputLeft, output, outputLeft);

    return NS_OK;
}

nsresult
nsNativeCharsetConverter::UnicodeToNative(const PRUnichar **input,
                                          PRUint32         *inputLeft,
                                          char            **output,
                                          PRUint32         *outputLeft)
{
    size_t res = 0;
    size_t inLeft = (size_t) *inputLeft * 2;
    size_t outLeft = (size_t) *outputLeft;

    if (gUnicodeToNative != INVALID_ICONV_T) {
        res = xp_iconv(gUnicodeToNative, (const char **) input, &inLeft, output, &outLeft);

        if (res != (size_t) -1) {
            *inputLeft = inLeft / 2;
            *outputLeft = outLeft;
            return NS_OK;
        }

        NS_ERROR("iconv failed");

        // reset converter
        xp_iconv(gUnicodeToNative, NULL, NULL, NULL, NULL);
    }
#if defined(ENABLE_UTF8_FALLBACK_SUPPORT)
    else if ((gUnicodeToUTF8 != INVALID_ICONV_T) &&
             (gUTF8ToNative != INVALID_ICONV_T)) {
        const char *in = (const char *) *input;

        char ubuf[6]; // max utf-8 char length (really only needs to be 4 bytes)

        // convert one uchar at a time...
        while (inLeft && outLeft) {
            char *p = ubuf;
            size_t n = sizeof(ubuf), one_uchar = sizeof(PRUnichar);
            res = xp_iconv(gUnicodeToUTF8, &in, &one_uchar, &p, &n);
            if (res == (size_t) -1) {
                NS_ERROR("conversion from ucs-2 to utf-8 failed");
                break;
            }
            p = ubuf;
            n = sizeof(ubuf) - n;
            res = xp_iconv(gUTF8ToNative, (const char **) &p, &n, output, &outLeft);
            if (res == (size_t) -1) {
                if (errno == E2BIG) {
                    // not enough room for last uchar... back up and return.
                    in -= sizeof(PRUnichar);
                    res = 0;
                }
                else
                    NS_ERROR("conversion from utf-8 to native failed");
                break;
            }
            inLeft -= sizeof(PRUnichar);
        }

        if (res != (size_t) -1) {
            (*input) += (*inputLeft - inLeft/2);
            *inputLeft = inLeft/2;
            *outputLeft = outLeft;
            return NS_OK;
        }

        // reset converters
        xp_iconv(gUnicodeToUTF8, NULL, NULL, NULL, NULL);
        xp_iconv(gUTF8ToNative, NULL, NULL, NULL, NULL);
    }
#endif

    // fallback: truncate and hope for the best
    ucs2_to_isolatin1(input, inputLeft, output, outputLeft);

    return NS_OK;
}

#endif // USE_ICONV

//-----------------------------------------------------------------------------
// conversion using mb[r]towc/wc[r]tomb
//-----------------------------------------------------------------------------
#if defined(USE_STDCONV)
#if defined(HAVE_WCRTOMB) || defined(HAVE_MBRTOWC)
#include <wchar.h>    // mbrtowc, wcrtomb
#endif

class nsNativeCharsetConverter
{
public:
    nsNativeCharsetConverter();

    nsresult NativeToUnicode(const char      **input , PRUint32 *inputLeft,
                             PRUnichar       **output, PRUint32 *outputLeft);
    nsresult UnicodeToNative(const PRUnichar **input , PRUint32 *inputLeft,
                             char            **output, PRUint32 *outputLeft);

    static void GlobalInit();
    static void GlobalShutdown() { }

private:
    static PRBool gWCharIsUnicode;

#if defined(HAVE_WCRTOMB) || defined(HAVE_MBRTOWC)
    mbstate_t ps;
#endif
};

PRBool nsNativeCharsetConverter::gWCharIsUnicode = PR_FALSE;

nsNativeCharsetConverter::nsNativeCharsetConverter()
{
#if defined(HAVE_WCRTOMB) || defined(HAVE_MBRTOWC)
    memset(&ps, 0, sizeof(ps));
#endif
}

void
nsNativeCharsetConverter::GlobalInit()
{
    // verify that wchar_t for the current locale is actually unicode.
    // if it is not, then we should avoid calling mbtowc/wctomb and
    // just fallback on zero-pad/truncation conversion.
    //
    // this test cannot be done at build time because the encoding of
    // wchar_t may depend on the runtime locale.  sad, but true!!
    //
    // so, if wchar_t is unicode then converting an ASCII character
    // to wchar_t should not change its numeric value.  we'll just
    // check what happens with the ASCII 'a' character.
    //
    // this test is not perfect... obviously, it could yield false
    // positives, but then at least ASCII text would be converted
    // properly (or maybe just the 'a' character) -- oh well :(

    char a = 'a';
    unsigned int w = 0;

    int res = mbtowc((wchar_t *) &w, &a, 1);

    gWCharIsUnicode = (res != -1 && w == 'a');

#ifdef DEBUG
    if (!gWCharIsUnicode)
        NS_WARNING("wchar_t is not unicode (unicode conversion will be lossy)");
#endif
}

nsresult
nsNativeCharsetConverter::NativeToUnicode(const char **input,
                                          PRUint32    *inputLeft,
                                          PRUnichar  **output,
                                          PRUint32    *outputLeft)
{
    if (gWCharIsUnicode) {
        int incr;

        // cannot use wchar_t here since it may have been redefined (e.g.,
        // via -fshort-wchar).  hopefully, sizeof(tmp) is sufficient XP.
        unsigned int tmp = 0;
        while (*inputLeft && *outputLeft) {
#ifdef HAVE_MBRTOWC
            incr = (int) mbrtowc((wchar_t *) &tmp, *input, *inputLeft, &ps);
#else
            // XXX is this thread-safe?
            incr = (int) mbtowc((wchar_t *) &tmp, *input, *inputLeft);
#endif
            if (incr < 0) {
                NS_WARNING("mbtowc failed: possible charset mismatch");
                // zero-pad and hope for the best
                tmp = (unsigned char) **input;
                incr = 1;
            }
            **output = (PRUnichar) tmp;
            (*input) += incr;
            (*inputLeft) -= incr;
            (*output)++;
            (*outputLeft)--;
        }
    }
    else {
        // wchar_t isn't unicode, so the best we can do is treat the
        // input as if it is isolatin1 :(
        isolatin1_to_ucs2(input, inputLeft, output, outputLeft);
    }

    return NS_OK;
}

nsresult
nsNativeCharsetConverter::UnicodeToNative(const PRUnichar **input,
                                          PRUint32         *inputLeft,
                                          char            **output,
                                          PRUint32         *outputLeft)
{
    if (gWCharIsUnicode) {
        int incr;

        while (*inputLeft && *outputLeft >= MB_CUR_MAX) {
#ifdef HAVE_WCRTOMB
            incr = (int) wcrtomb(*output, (wchar_t) **input, &ps);
#else
            // XXX is this thread-safe?
            incr = (int) wctomb(*output, (wchar_t) **input);
#endif
            if (incr < 0) {
                NS_WARNING("mbtowc failed: possible charset mismatch");
                **output = (unsigned char) **input; // truncate
                incr = 1;
            }
            // most likely we're dead anyways if this assertion should fire
            NS_ASSERTION(PRUint32(incr) <= *outputLeft, "wrote beyond end of string");
            (*output) += incr;
            (*outputLeft) -= incr;
            (*input)++;
            (*inputLeft)--;
        }
    }
    else {
        // wchar_t isn't unicode, so the best we can do is treat the
        // input as if it is isolatin1 :(
        ucs2_to_isolatin1(input, inputLeft, output, outputLeft);
    }

    return NS_OK;
}

#endif // USE_STDCONV

//-----------------------------------------------------------------------------
// API implementation
//-----------------------------------------------------------------------------

NS_COM nsresult
NS_CopyNativeToUnicode(const nsACString &input, nsAString &output)
{
    nsNativeCharsetConverter conv;
    nsresult rv;

    PRUint32 inputLen = input.Length();

    output.Truncate();

    nsACString::const_iterator iter, end;
    input.BeginReading(iter);
    input.EndReading(end);

    //
    // OPTIMIZATION: preallocate space for largest possible result; convert
    // directly into the result buffer to avoid intermediate buffer copy.
    //
    // this will generally result in a larger allocation, but that seems
    // better than an extra buffer copy.
    //
    output.SetLength(inputLen);
    nsAString::iterator out_iter;
    output.BeginWriting(out_iter);

    PRUnichar *result = out_iter.get();
    PRUint32 resultLeft = inputLen;

    PRUint32 size;
    for (; iter != end; iter.advance(size)) {
        const char *buf = iter.get();
        PRUint32 bufLeft = size = iter.size_forward();

        rv = conv.NativeToUnicode(&buf, &bufLeft, &result, &resultLeft);
        if (NS_FAILED(rv)) return rv;

        NS_ASSERTION(bufLeft == 0, "did not consume entire input buffer");
    }
    output.SetLength(inputLen - resultLeft);
    return NS_OK;
}

NS_COM nsresult
NS_CopyUnicodeToNative(const nsAString &input, nsACString &output)
{
    nsNativeCharsetConverter conv;
    nsresult rv;

    output.Truncate();

    nsAString::const_iterator iter, end;
    input.BeginReading(iter);
    input.EndReading(end);

    // cannot easily avoid intermediate buffer copy.
    char temp[4096];

    PRUint32 size;
    for (; iter != end; iter.advance(size)) {
        const PRUnichar *buf = iter.get();
        PRUint32 bufLeft = size = iter.size_forward();
        while (bufLeft) {
            char *p = temp;
            PRUint32 tempLeft = sizeof(temp);

            rv = conv.UnicodeToNative(&buf, &bufLeft, &p, &tempLeft);
            if (NS_FAILED(rv)) return rv;

            if (tempLeft < sizeof(temp))
                output.Append(temp, sizeof(temp) - tempLeft);
        }
    }
    return NS_OK;
}

void
NS_StartupNativeCharsetUtils()
{
    //
    // need to initialize the locale or else charset conversion will fail.
    // better not delay this in case some other component alters the locale
    // settings.
    //
    // XXX we assume that we are called early enough that we should
    // always be the first to care about the locale's charset.
    //
    setlocale(LC_CTYPE, "");

    nsNativeCharsetConverter::GlobalInit();
}

void
NS_ShutdownNativeCharsetUtils()
{
    nsNativeCharsetConverter::GlobalShutdown();
}

#else // non XP_UNIX implementations go here...

#include "nsDebug.h"
#include "nsAString.h"

NS_COM nsresult
NS_CopyNativeToUnicode(const nsACString &input, nsAString  &output)
{
    NS_NOTREACHED("NS_CopyNativeToUnicode");
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_COM nsresult
NS_CopyUnicodeToNative(const nsAString  &input, nsACString &output)
{
    NS_NOTREACHED("NS_CopyUnicodeToNative");
    return NS_ERROR_NOT_IMPLEMENTED;
}

void
NS_StartupNativeCharsetUtils()
{
}

void
NS_ShutdownNativeCharsetUtils()
{
}

#endif
