#include "game-capture-hook-install.h"

#include <windows.h>
#include <wincrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <util/base.h>

#undef warn
#define warn(format, ...) blog(LOG_WARNING, "update_hook_file: " format, ##__VA_ARGS__)

static bool file_exists(const wchar_t *path)
{
	const DWORD attr = GetFileAttributesW(path);
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool signer_is_obs(HANDLE state)
{
	CRYPT_PROVIDER_DATA *prov = WTHelperProvDataFromStateData(state);
	if (!prov)
		return false;
	CRYPT_PROVIDER_SGNR *sgnr = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0);
	if (!sgnr)
		return false;
	CRYPT_PROVIDER_CERT *cert = WTHelperGetProvCertFromChain(sgnr, 0);
	if (!cert || !cert->pCert)
		return false;

	wchar_t name[256];
	if (CertGetNameStringW(cert->pCert, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_COMMON_NAME, name, 256) <= 1 ||
	    wcscmp(name, HOOK_SIGNER_NAME) != 0)
		return false;
	if (CertGetNameStringW(cert->pCert, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_ORGANIZATION_NAME, name, 256) <= 1 ||
	    wcscmp(name, HOOK_SIGNER_NAME) != 0)
		return false;
	return true;
}

bool hook_signed_by_obs(const wchar_t *path)
{
	if (!path || !file_exists(path))
		return false;

	WINTRUST_FILE_INFO file = {0};
	file.cbStruct = sizeof(file);
	file.pcwszFilePath = path;

	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	WINTRUST_DATA data = {0};
	data.cbStruct = sizeof(data);
	data.dwUIChoice = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwUnionChoice = WTD_CHOICE_FILE;
	data.pFile = &file;
	data.dwStateAction = WTD_STATEACTION_VERIFY;
	data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_DISABLE_MD2_MD4;

	const LONG status = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
	const bool ok = status == ERROR_SUCCESS && signer_is_obs(data.hWVTStateData);

	data.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
	return ok;
}

void hook_file_info_get(const wchar_t *path, struct hook_file_info *info)
{
	memset(info, 0, sizeof(*info));
	info->exists = file_exists(path);
	if (!info->exists)
		return;
	info->signed_by_obs = hook_signed_by_obs(path);
	info->has_version = get_dll_ver(path, &info->version);
}

enum hook_install_action hook_install_decide(const struct hook_file_info *src, const struct hook_file_info *dst)
{
	if (!src->exists || !src->signed_by_obs || !src->has_version)
		return HOOK_INSTALL_NOT_TRUSTED;
	if (!dst->exists)
		return HOOK_INSTALL_COPY;
	if (dst->signed_by_obs) {
		if (!dst->has_version || win_version_compare(&dst->version, &src->version) < 0)
			return HOOK_INSTALL_COPY;
		return HOOK_INSTALL_KEEP;
	}
	/* an unsigned (or foreign) hook: replaced by the signed one unless that would be a downgrade */
	if (!dst->has_version || win_version_compare(&dst->version, &src->version) <= 0)
		return HOOK_INSTALL_COPY;
	return HOOK_INSTALL_KEEP;
}

bool hook_install_usable(const struct hook_file_info *src, const struct hook_file_info *dst)
{
	if (!dst->exists || !dst->signed_by_obs || !dst->has_version)
		return false;
	/* without a bundled hook to compare to, the protocol version cannot be checked */
	if (!src->has_version)
		return false;
	return dst->version.major <= src->version.major;
}

bool hook_install_files(const wchar_t *src_dll, const wchar_t *src_json, const wchar_t *dst_dll,
			const wchar_t *dst_json)
{
	struct hook_file_info src, dst;
	hook_file_info_get(src_dll, &src);
	hook_file_info_get(dst_dll, &dst);

	switch (hook_install_decide(&src, &dst)) {
	case HOOK_INSTALL_NOT_TRUSTED:
		if (src.exists)
			warn("%ls is not signed by " HOOK_SIGNER_NAME_A ", not installing it", src_dll);
		else
			warn("source %ls missing", src_dll);
		break;
	case HOOK_INSTALL_COPY:
		if (!file_exists(src_json)) {
			warn("source %ls missing", src_json);
			return false;
		}
		/* the manifest first: the dll is what decides whether the target is used */
		if (!CopyFileW(src_json, dst_json, false)) {
			warn("failed to install %ls (%lu)", dst_json, GetLastError());
			return false;
		}
		if (!CopyFileW(src_dll, dst_dll, false)) {
			warn("failed to install %ls (%lu)", dst_dll, GetLastError());
			return false;
		}
		hook_file_info_get(dst_dll, &dst);
		break;
	case HOOK_INSTALL_KEEP:
		break;
	}

	if (dst.exists && !dst.signed_by_obs)
		warn("%ls is not signed by " HOOK_SIGNER_NAME_A ", not using it", dst_dll);
	else if (dst.exists && src.has_version && dst.version.major > src.version.major)
		warn("target %ls has major version %d, source has major version %d", dst_dll, dst.version.major,
		     src.version.major);

	return hook_install_usable(&src, &dst);
}
