/*
 * iso_reader_c.cpp — C wrapper for the C++ ISOReader class
 *
 * Provides iso_open() / iso_read_sector() / iso_close() for use by cdrom.c
 */

#include "iso_reader.h"
#include "mod_runtime.h"
#include <array>
#include <cstring>
#include <cstdio>

extern "C" {

void* iso_open(const char* path) {
    if (!path) return nullptr;
    try {
        if (PS1::ISOReader* verified =
                PSXRecompV4::mod_runtime_take_verified_disc(path))
            return verified;
        if (PSXRecompV4::mod_runtime_requires_verified_disc()) return nullptr;
        auto* reader = new PS1::ISOReader();
        if (!reader->Open(path)) {
            delete reader;
            return nullptr;
        }
        return reader;
    } catch (...) {
        return nullptr;
    }
}

int iso_read_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle || !buffer || size < 2048) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    std::array<uint8_t, 2352> raw{};
    if (mod_runtime_read_virtual_raw_sector(
            lba, raw.data(), (uint32_t)raw.size())) {
        mod_runtime_patch_disc_sector(
            lba, 1, raw.data(), (uint32_t)raw.size());
        std::memcpy(buffer, raw.data() + 24, 2048);
        return 1;
    }
    if (!reader->ReadSector(lba, buffer)) return 0;
    mod_runtime_patch_disc_sector(lba, 0, buffer, 2048);
    return 1;
}

int iso_read_raw_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle || !buffer || size < 2352) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (mod_runtime_read_virtual_raw_sector(lba, buffer, (uint32_t)size)) {
        mod_runtime_patch_disc_sector(lba, 1, buffer, 2352);
        return 1;
    }
    if (!reader->ReadRawSector(lba, buffer)) return 0;
    mod_runtime_patch_disc_sector(lba, 1, buffer, 2352);
    return 1;
}

int iso_read_subq(void* handle, uint32_t lba, uint8_t* buffer, int size,
                  int* valid) {
    if (!handle || !buffer || size < 12 || !valid) return 0;
    bool crc_valid = false;
    if (!static_cast<PS1::ISOReader*>(handle)->ReadSubChannelQ(
            lba, buffer, &crc_valid)) return 0;
    *valid = crc_valid ? 1 : 0;
    return 1;
}

int iso_has_subq_replacements(void* handle) {
    return handle && static_cast<PS1::ISOReader*>(handle)->HasSubChannelReplacements();
}

uint32_t iso_sector_count(void* handle) {
    if (!handle) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return mod_runtime_effective_sector_count(reader->GetSectorCount());
}

/* CD-track TOC accessors (multi-track / CD-DA support). track is 1-based. */
int iso_track_count(void* handle) {
    if (!handle) return 1;
    return static_cast<PS1::ISOReader*>(handle)->TrackCount();
}

uint32_t iso_track_start_lba(void* handle, int track) {
    if (!handle) return 0;
    return static_cast<PS1::ISOReader*>(handle)->TrackStartLBA(track);
}

uint32_t iso_track_pregap_lba(void* handle, int track) {
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return reader ? reader->TrackPregapLBA(track) : 0;
}

int iso_track_is_audio(void* handle, int track) {
    if (!handle) return 0;
    return static_cast<PS1::ISOReader*>(handle)->TrackIsAudio(track) ? 1 : 0;
}

void iso_close(void* handle) {
    if (!handle) return;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (PSXRecompV4::mod_runtime_return_verified_disc(reader)) return;
    reader->Close();
    delete reader;
}

} /* extern "C" */
