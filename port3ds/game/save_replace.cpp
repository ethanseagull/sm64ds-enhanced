// libctru's SD rename refuses to overwrite an existing destination. Keep the
// previous image recoverable while replacing it; never unlink the only copy.
#include <cstdio>
#include <cstring>

namespace {
bool exists(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}
bool previous_path(const char *path, char (&previous)[1088])
{
    if (std::strlen(path) + sizeof ".previous" > sizeof previous) return false;
    std::snprintf(previous, sizeof previous, "%s.previous", path);
    return true;
}
}

extern "C" int game3ds_save_recover(const char *path)
{
    char previous[1088];
    if (!previous_path(path, previous)) return -1;
    if (!exists(previous)) return 0;
    // A completed replacement owns path. Otherwise restore the previous image
    // before the backup medium is read, including after a forced power-off.
    if (exists(path)) return std::remove(previous);
    return std::rename(previous, path);
}

extern "C" int game3ds_save_replace(const char *temp, const char *path)
{
    char previous[1088];
    if (!previous_path(path, previous) || game3ds_save_recover(path)) return -1;
    if (!exists(path)) return std::rename(temp, path);
    if (std::rename(path, previous)) return -1;
    if (std::rename(temp, path)) {
        // Preserve .previous even if restoration itself fails; the next open
        // retries it. The caller may remove temp, but never this recovery copy.
        std::rename(previous, path);
        return -1;
    }
    // A failed cleanup does not undo a successful replacement. Recovery will
    // remove the stale previous image on the next write or process launch.
    std::remove(previous);
    return 0;
}
