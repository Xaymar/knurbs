#pragma once

/* Installing the game capture hook into C:\ProgramData\obs-studio-hook, where every game (through the Vulkan layer
 * registered there) and every OBS Studio installation on the machine loads it from. Only a hook that is validly
 * Authenticode-signed by OBS Project, LLC is ever copied there, never one older than what is there, and only a signed
 * hook found there is used. */

#include <stdbool.h>
#include <wchar.h>

#include <util/windows/win-version.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The subject of the certificate OBS Studio's release binaries are signed with. */
#define HOOK_SIGNER_NAME_A "OBS Project, LLC"
#define HOOK_SIGNER_NAME L"" HOOK_SIGNER_NAME_A

/* True when the file has a valid Authenticode signature (chain to a trusted root, not tampered with) whose signer is
 * OBS Project, LLC. No network access: revocation is not checked. */
bool hook_signed_by_obs(const wchar_t *path);

struct hook_file_info {
	bool exists;
	bool signed_by_obs;
	bool has_version;
	struct win_version_info version;
};

/* Reads existence, signature and version of a hook dll. */
void hook_file_info_get(const wchar_t *path, struct hook_file_info *info);

enum hook_install_action {
	HOOK_INSTALL_NOT_TRUSTED, /* the source is missing, unsigned or not signed by OBS: never copied */
	HOOK_INSTALL_COPY,        /* the source is signed and newer than the target (or replaces an unsigned one) */
	HOOK_INSTALL_KEEP,        /* the target is as new as (or newer than) the source: left alone */
};

/* Whether the source hook is to be copied over the target. A source is copied only when it is signed by OBS and:
 * there is no target; or the target is signed by OBS and older; or the target is not signed by OBS and not newer
 * (a signed hook replaces an unsigned one, but never by downgrading it). */
enum hook_install_action hook_install_decide(const struct hook_file_info *src, const struct hook_file_info *dst);

/* Whether the installed target may be loaded instead of the bundled source: only a target signed by OBS, and not of
 * a newer major version than the source (the protocol may have changed). */
bool hook_install_usable(const struct hook_file_info *src, const struct hook_file_info *dst);

/* Installs src_dll (with its Vulkan layer manifest src_json) as dst_dll / dst_json according to hook_install_decide.
 * The target directory must exist. Returns true when the target is usable afterwards (hook_install_usable). */
bool hook_install_files(const wchar_t *src_dll, const wchar_t *src_json, const wchar_t *dst_dll,
			const wchar_t *dst_json);

#ifdef __cplusplus
}
#endif
