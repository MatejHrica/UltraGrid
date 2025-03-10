/**
 * @file   blackmagic_common.cpp
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2014-2021 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>

#include "blackmagic_common.hpp"
#include "debug.h"
#include "host.h"
#include "DeckLinkAPIVersion.h"
#include "utils/worker.h"

#define MOD_NAME "[DeckLink] "

using namespace std;

static unordered_map<HRESULT, string> bmd_hresult_to_string_map = {
        {S_OK, "success"},
        {S_FALSE, "false"},
        {E_UNEXPECTED, "unexpected value"},
        {E_NOTIMPL, "not implemented"},
        {E_OUTOFMEMORY, "out of memory"},
        {E_INVALIDARG, "invalid argument"},
        {E_NOINTERFACE, "interface was not found"},
        {E_POINTER, "invalid pointer"},
        {E_HANDLE, "invalid handle"},
        {E_ABORT, "operation aborted"},
        {E_FAIL, "failure"},
        {E_ACCESSDENIED, "access denied"},
};

string bmd_hresult_to_string(HRESULT res)
{
        ostringstream oss;
        auto it = bmd_hresult_to_string_map.find(res);
        if (it != bmd_hresult_to_string_map.end()) {
                oss << it->second;
        }
        oss << " " << "(0x" << hex << setfill('0') << setw(8) << res << ")";
        return oss.str();
}

/**
 * returned c-sring needs to be freed when not used
 */
char *get_cstr_from_bmd_api_str(BMD_STR bmd_string)
{
        if (!bmd_string) {
                return strdup("(NULL!)");
        }
       char *cstr;
#ifdef HAVE_MACOSX
       size_t len = CFStringGetMaximumSizeForEncoding(CFStringGetLength(bmd_string), kCFStringEncodingUTF8) + 1;
       cstr = (char *) malloc(len);
       CFStringGetCString(bmd_string, (char *) cstr, len, kCFStringEncodingUTF8);
#elif defined WIN32
       size_t len = SysStringLen(bmd_string) * 4 + 1;
       cstr = (char *) malloc(len);
       wcstombs((char *) cstr, bmd_string, len);
#else // Linux
       cstr = strdup(bmd_string);
#endif

       return cstr;
}

BMD_STR get_bmd_api_str_from_cstr(const char *cstr)
{
#ifdef __APPLE__
        return CFStringCreateWithCString(kCFAllocatorMalloc, cstr, kCFStringEncodingUTF8);
#elif defined _WIN32
        mbstate_t mbstate{};
        const char *tmp = cstr;
        size_t required_size = mbsrtowcs(NULL, &tmp, 0, &mbstate) + 1;
        BMD_STR out = (wchar_t *) malloc(required_size * sizeof(wchar_t));
        mbsrtowcs(out, &tmp, required_size, &mbstate);
	return out;
#else
        return strdup(cstr);
#endif
}

void release_bmd_api_str(BMD_STR string)
{
        if (!string) {
                return;
        }
#ifdef HAVE_MACOSX
        CFRelease(string);
#elif defined WIN32
        SysFreeString(string);
#else
        free(const_cast<char *>(string));
#endif
}

std::string get_str_from_bmd_api_str(BMD_STR string)
{
        char *displayModeCString = get_cstr_from_bmd_api_str(string);
        std::string out = displayModeCString;
        free(displayModeCString);
        return out;
}

/**
 * @note
 * Each successful call (returning non-null pointer) of this function with coinit == true
 * should be followed by decklink_uninitialize() when done with DeckLink (not when releasing
 * IDeckLinkIterator!), typically on application shutdown.
 */
IDeckLinkIterator *create_decklink_iterator(bool verbose, bool coinit)
{
        IDeckLinkIterator *deckLinkIterator = nullptr;
#ifdef WIN32
        if (coinit) {
                // Initialize COM on this thread
                HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
                if(FAILED(result)) {
                        log_msg(LOG_LEVEL_ERROR, "Initialize of COM failed - result = "
                                        "%08lx.\n", result);
                        if (result == S_FALSE) {
                                CoUninitialize();
                        }
                        return NULL;
                }
        }
        HRESULT result = CoCreateInstance(CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL,
                        IID_IDeckLinkIterator, (void **) &deckLinkIterator);
        if (FAILED(result)) {
                deckLinkIterator = nullptr;
        }
#else
        UNUSED(coinit);
        deckLinkIterator = CreateDeckLinkIteratorInstance();
#endif

        if (!deckLinkIterator && verbose) {
                log_msg(LOG_LEVEL_ERROR, "\nA DeckLink iterator could not be created. The DeckLink drivers may not be installed or are outdated.\n");
                log_msg(LOG_LEVEL_INFO, "This UltraGrid version was compiled with DeckLink drivers %s. You should have at least this version.\n\n",
                                BLACKMAGIC_DECKLINK_API_VERSION_STRING);

        }

        return deckLinkIterator;
}

void decklink_uninitialize()
{
#ifdef WIN32
        CoUninitialize();
#endif
}

bool blackmagic_api_version_check()
{
        bool ret = false;
        IDeckLinkAPIInformation *APIInformation = NULL;
        HRESULT result;

#ifdef WIN32
        // Initialize COM on this thread
        result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if(FAILED(result)) {
                log_msg(LOG_LEVEL_ERROR, "Initialize of COM failed - result = "
                                "%08lx.\n", result);
                goto cleanup;
        }

        result = CoCreateInstance(CLSID_CDeckLinkAPIInformation, NULL, CLSCTX_ALL,
                IID_IDeckLinkAPIInformation, (void **) &APIInformation);
        if(FAILED(result)) {
#else
        APIInformation = CreateDeckLinkAPIInformationInstance();
        if(APIInformation == NULL) {
#endif
                log_msg(LOG_LEVEL_ERROR, "Cannot get API information! Perhaps drivers not installed.\n");
                goto cleanup;
        }
        int64_t value;
        result = APIInformation->GetInt(BMDDeckLinkAPIVersion, &value);
        if(result != S_OK) {
                log_msg(LOG_LEVEL_ERROR, "Cannot get API version!\n");
                goto cleanup;
        }

        if (BLACKMAGIC_DECKLINK_API_VERSION > value) { // this is safe comparision, for internal structure please see SDK documentation
                log_msg(LOG_LEVEL_ERROR, "The DeckLink drivers may not be installed or are outdated.\n");
                log_msg(LOG_LEVEL_ERROR, "You should have at least the version UltraGrid has been linked with.\n");
                log_msg(LOG_LEVEL_ERROR, "Vendor download page is http://www.blackmagic-design.com/support\n");
                print_decklink_version();
                ret = false;
        } else {
                ret = true;
        }

cleanup:
        if (APIInformation) {
                APIInformation->Release();
        }
#ifdef WIN32
        CoUninitialize();
#endif

        return ret;
}

void print_decklink_version()
{
        BMD_STR current_version = NULL;
        IDeckLinkAPIInformation *APIInformation = NULL;
        HRESULT result;

#ifdef WIN32
        // Initialize COM on this thread
        result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if(FAILED(result)) {
                fprintf(stderr, "Initialize of COM failed - result = "
                                "%08lx.\n", result);
                goto cleanup;
        }

        result = CoCreateInstance(CLSID_CDeckLinkAPIInformation, NULL, CLSCTX_ALL,
                IID_IDeckLinkAPIInformation, (void **) &APIInformation);
        if(FAILED(result)) {
#else
        APIInformation = CreateDeckLinkAPIInformationInstance();
        if(APIInformation == NULL) {
#endif
                fprintf(stderr, "Cannot get API information! Perhaps drivers not installed.\n");
                goto cleanup;
        }

        result = APIInformation->GetString(BMDDeckLinkAPIVersion, &current_version);
        if (result != S_OK) {
                fprintf(stderr, "Cannot get API version string!\n");
                goto cleanup;
        } else {
                fprintf(stderr, "This UltraGrid version was compiled against DeckLink SDK %s. ", BLACKMAGIC_DECKLINK_API_VERSION_STRING);
                const char *currentVersionCString = get_cstr_from_bmd_api_str(current_version);
                fprintf(stderr, "System version is %s.\n", currentVersionCString);
                release_bmd_api_str(current_version);
                free(const_cast<char *>(currentVersionCString));
        }

cleanup:
        if (APIInformation) {
                APIInformation->Release();
        }
#ifdef WIN32
        CoUninitialize();
#endif
}

#define EXIT_IF_FAILED(cmd, name) \
        do {\
                HRESULT result = cmd;\
                if (FAILED(result)) {;\
                        LOG(LOG_LEVEL_ERROR) << MOD_NAME << name << ": " << bmd_hresult_to_string(result) << "\n";\
			ret = false;\
			goto cleanup;\
                }\
        } while (0)

#define RELEASE_IF_NOT_NULL(x) if (x != nullptr) { x->Release(); x = nullptr; }



/**
 * @param a value from BMDProfileID or bmdDuplexHalf (maximize number of IOs)
 */
bool decklink_set_duplex(IDeckLink *deckLink, uint32_t profileID)
{
        bool ret = true;
        IDeckLinkProfileManager *manager = nullptr;
        IDeckLinkProfileIterator *it = nullptr;
        IDeckLinkProfile *profile = nullptr;
        bool found = false;

        EXIT_IF_FAILED(deckLink->QueryInterface(IID_IDeckLinkProfileManager, (void**)&manager), "Cannot set duplex - query profile manager");

        EXIT_IF_FAILED(manager->GetProfiles(&it), "Cannot set duplex - get profiles");

        while (it->Next(&profile) == S_OK) {
                IDeckLinkProfileAttributes *attributes;
                int64_t id;
                if (profile->QueryInterface(IID_IDeckLinkProfileAttributes,
                                        (void**)&attributes) != S_OK) {
                        LOG(LOG_LEVEL_WARNING) << "[DeckLink] Cannot get profile attributes!\n";
                        continue;
                }
                if (attributes->GetInt(BMDDeckLinkProfileID, &id) == S_OK) {
                        if (profileID == bmdDuplexHalf) {
                                if (id == bmdProfileTwoSubDevicesHalfDuplex || id == bmdProfileFourSubDevicesHalfDuplex) {
                                        found = true;
                                }
                        } else if (profileID == id) {
                                found = true;
                        }
                        if (found) {
                                if (profile->SetActive() != S_OK) {
                                        LOG(LOG_LEVEL_ERROR) << "[DeckLink] Cannot set profile!\n";
                                        ret = false;
                                }
                        }
                } else {
                        LOG(LOG_LEVEL_WARNING) << "[DeckLink] Cannot get profile ID!\n";
                }
                attributes->Release();
                profile->Release();
                if (found) {
                        break;
                }
        }

        if (!found && ret) { // no err but not found
                LOG(LOG_LEVEL_WARNING) << "[DeckLink] did not find suitable duplex profile!\n";
                ret = false;
        }

cleanup:
        RELEASE_IF_NOT_NULL(it);
        RELEASE_IF_NOT_NULL(manager);
	return ret;
}

string bmd_get_device_name(IDeckLink *decklink) {
	BMD_STR         deviceNameString = NULL;
	char *          deviceNameCString = NULL;
	string 		ret;

	if (decklink->GetDisplayName((BMD_STR *) &deviceNameString) == S_OK) {
		deviceNameCString = get_cstr_from_bmd_api_str(deviceNameString);
		ret = deviceNameCString;
		release_bmd_api_str(deviceNameString);
		free(deviceNameCString);
	}

        return ret;
}

uint32_t bmd_read_fourcc(const char *str) {
        union {
                uint32_t fourcc;
                char c4[4];
        } u;
        memset(u.c4, ' ', 4);
        memcpy(u.c4, str, min(strlen(str), sizeof u.c4));
        return htonl(u.fourcc);
}

std::ostream &operator<<(std::ostream &output, REFIID iid)
{
#ifdef _WIN32
        OLECHAR* guidString;
        StringFromCLSID(iid, &guidString);
        char buffer[128];
        int ret = wcstombs(buffer, guidString, sizeof buffer);
        if (ret == sizeof buffer) {
                buffer[sizeof buffer - 1] = '\0';
        }
        output << buffer;
        ::CoTaskMemFree(guidString);
#else
        auto flags = output.flags();
        output << hex << uppercase << setfill('0') <<
                setw(2) << static_cast<int>(iid.byte0) << setw(2) << static_cast<int>(iid.byte1) <<
                setw(2) << static_cast<int>(iid.byte2) << setw(2) << static_cast<int>(iid.byte3) << "-" <<
                setw(2) << static_cast<int>(iid.byte4) << setw(2) << static_cast<int>(iid.byte5) << "-" <<
                setw(2) << static_cast<int>(iid.byte6) << setw(2) << static_cast<int>(iid.byte7) << "-" <<
                setw(2) << static_cast<int>(iid.byte8) << setw(2) << static_cast<int>(iid.byte9) << "-" <<
                setw(2) << static_cast<int>(iid.byte10) << setw(2) << static_cast<int>(iid.byte11) <<
                setw(2) << static_cast<int>(iid.byte12) << setw(2) << static_cast<int>(iid.byte13) <<
                setw(2) << static_cast<int>(iid.byte14) << setw(2) << static_cast<int>(iid.byte15);
        output.setf(flags);
#endif
        return output;
}

/**
 * @note
 * Returns true also for empty/NULL val - this allow specifying the flag without explicit value
 */
int parse_bmd_flag(const char *val)
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnull-pointer-arithmetic"
#endif // defined __clang__
        if (val == nullptr || val == static_cast<char *>(nullptr) + 1 // allow constructions like parse_bmd_flag(strstr(opt, '=') + 1)
                        || strlen(val) == 0 || strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "on") == 0  || strcasecmp(val, "yes") == 0) {
                return BMD_OPT_TRUE;
        }
        if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0 || strcasecmp(val, "off") == 0  || strcasecmp(val, "no") == 0) {
                return BMD_OPT_FALSE;
        }
        if (strcasecmp(val, "keep") == 0) {
                return BMD_OPT_KEEP;
        }

        LOG(LOG_LEVEL_ERROR) << "Value " << val << " not recognized for a flag, use one of: " R"_("false", "true" or "keep")_" "\n";
        return -1;
#ifdef __clang__
#pragma clang diagnostic pop
#endif // defined __clang
}

int invert_bmd_flag(int val)
{
        if (val == BMD_OPT_TRUE) {
                return BMD_OPT_FALSE;
        }
        if (val == BMD_OPT_FALSE) {
                return BMD_OPT_TRUE;
        }
        return val;
}

static void apply_r10k_lut(void *i, void *o, size_t len, void *udata)
{
        auto lut = (const unsigned int * __restrict) udata;
        auto *in = (const unsigned char *) i;
        auto *out = (unsigned char *) o;
        const unsigned char *in_end = in + len;
        while (in < in_end) {
                unsigned r = in[0] << 2U | in[1] >> 6U;
                unsigned g = (in[1] & 0x3FU) << 4U | in[2] >> 4U;
                unsigned b = (in[2] & 0xFU) << 6U | in[3] >> 2U;
                r = lut[r];
                g = lut[g];
                b = lut[b];
                out[0] = r >> 2U;
                out[1] = (r & 0x3U) << 6U | (g >> 4U);
                out[2] = (g & 0xFU) << 4U | (b >> 6U);
                out[3] = (b & 0x3FU) << 2U;
                in += 4;
                out += 4;
        }
}

static void fill_limited_to_full_lut(unsigned int *lut) {
        for (int i = 0; i < 1024; ++i) {
                int val = clamp(i, 64, 960);
                val = 4 + (val - 64) * 1015 / 896;
                lut[i] = val;
        }
}
/**
 * converts from range 64-960 to 4-1019
 *
 * in and out pointers can point to the same address
 */
void r10k_limited_to_full(const char *in, char *out, size_t len)
{
        static unsigned int lut[1024];
        if (lut[1023] == 0) {
                fill_limited_to_full_lut(lut);
        }
        DEBUG_TIMER_START(r10k_limited_to_full);
        respawn_parallel(const_cast<char *>(in), out, len / 4, 4, apply_r10k_lut, lut);
        DEBUG_TIMER_STOP(r10k_limited_to_full);
}

static void fill_full_to_limited_lut(unsigned int *lut) {
        for (int i = 0; i < 1024; ++i) {
                int val = clamp(i, 4, 1019);
                val = 64 + (val - 4) * 896 / 1015;
                lut[i] = val;
        }
}
/**
 * converts from full range (4-1019) to  64-960
 *
 * in and out pointers can point to the same address
 */
void r10k_full_to_limited(const char *in, char *out, size_t len)
{
        static unsigned int lut[1024];
        if (lut[1023] == 0) {
                fill_full_to_limited_lut(lut);
        }
        DEBUG_TIMER_START(r10k_limited_to_full);
        respawn_parallel(const_cast<char *>(in), out, len / 4, 4, apply_r10k_lut, lut);
        DEBUG_TIMER_STOP(r10k_limited_to_full);
}

string bmd_get_audio_connection_name(BMDAudioOutputAnalogAESSwitch audioConnection) {
        switch(audioConnection) {
                case bmdAudioOutputSwitchAESEBU:
                        return "AES/EBU";
                case bmdAudioOutputSwitchAnalog:
                        return "analog";
                default:
                        return "default";
        }
}

ADD_TO_PARAM(R10K_FULL_OPT, "* " R10K_FULL_OPT "\n"
                "  Do not do conversion from/to limited range on in/out for R10k on BMD devs.\n");

