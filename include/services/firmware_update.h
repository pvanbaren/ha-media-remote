#pragma once

#include <cstddef>

/**
 * Firmware from GitHub: whether a newer release is out, and installing it.
 *
 * Every release on config::kFirmwareRepo carries each board's image as
 * firmware-<env>.bin. The check asks GitHub which release is the latest and
 * whether it has an image for this board; the install downloads that image
 * straight into the app slot not running, and the next boot runs it -- the
 * same slot the portal's Update writes to. GitHub's certificate is checked
 * against the root certificates built into the firmware.
 *
 * Both block, over HTTPS: the check a few seconds, the install as long as
 * the image takes to come down -- half a minute, say. Neither draws; the app
 * shows what they are doing.
 */
namespace services::update {

/** This build's version: a release's tag ("v1.5.0"), or `git describe`'s
 *  name for a build after one ("v1.5.0-23-gc83026a"). */
const char* currentVersion();
/** The env this image was built for, which names its image in a release. */
const char* boardEnv();

enum class Check : unsigned char {
  kNewer,     // the latest release is newer than this build, with an image
  kUpToDate,  // this build is the latest release, or newer
  kNoImage,   // the latest release is newer but has no image for this board
  kFailed,    // GitHub could not be asked; the error says why
};

struct CheckResult {
  Check result = Check::kFailed;
  /** The latest release's tag, when GitHub gave one. */
  char tag[32] = {};
  /** Why it failed, for kFailed. */
  char error[64] = {};
};

/** Ask GitHub for the latest release, and whether this board can have it.
 *  Blocks. */
void check(CheckResult& out);

/** Download release `tag`'s image for this board and write it to the other
 *  app slot, calling `progress` with 0-100 as it comes down. True when it is
 *  written, checked and set to boot next: the caller restarts. False with
 *  `error` saying why, and the running firmware untouched. Blocks. */
bool install(const char* tag, void (*progress)(int percent), char* error,
             size_t error_len);

/** Serve the portal's /firmware page: this build, a check, and an install.
 *  Call before Wi-Fi comes up, as for any portal page. */
void init();

/** An install asked for on the portal page, which the loop runs: true once,
 *  with its tag in `tag`. The page only asks; the download is the loop's,
 *  where the screen can show it. */
bool takeInstallRequest(char* tag, size_t tag_len);

}  // namespace services::update
