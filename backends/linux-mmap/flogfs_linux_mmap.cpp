#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>
#include <cassert>
#include <cstdarg>
#include <cstdio>

#include <flogfs.h>
#include <flogfs_private.h>

#include "flogfs_linux_mmap.h"

#define fslog_trace(f, ...) flash_debug_warn(f, ##__VA_ARGS__)

#define fslog_debug(f, ...) flash_debug_warn(f, ##__VA_ARGS__)

#ifdef FLOG_ERASE_ZERO
constexpr uint8_t FS_ERASE_CHAR = 0x00;
#else
constexpr uint8_t FS_ERASE_CHAR = 0xff;
#endif
constexpr uint32_t FS_SECTORS_PER_PAGE_INTERNAL = (FS_SECTORS_PER_PAGE + 1);
constexpr uint32_t FS_SECTORS_PER_BLOCK_INTERNAL = FS_SECTORS_PER_PAGE_INTERNAL * FS_PAGES_PER_BLOCK;
constexpr uint32_t FS_MAPPED_SIZE = FS_SECTOR_SIZE * FS_SECTORS_PER_BLOCK_INTERNAL * FS_NUM_BLOCKS;

static void *mapped{ nullptr };
static int32_t fd{ -1 };
static uint16_t open_block{ 0 };
static uint16_t open_page{ 0 };

flog_result_t flogfs_linux_open(const char *path) {
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return FLOG_FAILURE;
    }

    if (lseek(fd, FS_MAPPED_SIZE, SEEK_SET) == -1) {
        return FLOG_FAILURE;
    }

    if (write(fd, "", 1) == -1) {
        return FLOG_FAILURE;
    }

    mapped = mmap(nullptr, FS_MAPPED_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        mapped = nullptr;
        return FLOG_FAILURE;
    }

    return FLOG_SUCCESS;
}

flog_result_t flogfs_linux_close() {
    if (mapped != nullptr) {
        munmap(mapped, FS_MAPPED_SIZE);
        mapped = nullptr;
    }
    if (fd < 0) {
        close(fd);
        fd = -1;
    }

    return FLOG_SUCCESS;
}

static inline uint32_t get_offset(uint16_t block, uint16_t page, uint8_t sector, uint16_t offset) {
    return (block * FS_SECTORS_PER_BLOCK_INTERNAL * FS_SECTOR_SIZE) +
        (page * FS_SECTORS_PER_PAGE_INTERNAL * FS_SECTOR_SIZE) +
        (sector * FS_SECTOR_SIZE) + offset;
}

static inline uint32_t get_offset_in_block(uint8_t sector, uint16_t offset) {
    return get_offset(open_block, open_page, sector, offset);
}

static inline void *mapped_sector_absolute_ptr(uint16_t block, uint16_t page, uint8_t sector, uint16_t offset) {
    return ((uint8_t *)mapped) + get_offset(block, page, sector, offset);
}

static inline void *mapped_sector_ptr(uint8_t sector, uint16_t offset) {
    return mapped_sector_absolute_ptr(open_block, open_page, sector, offset);
}

void fs_lock_init(fs_lock_t *lock) {
}

void fs_lock(fs_lock_t *lock) {
}

void fs_unlock(fs_lock_t *lock) {
}

flog_result_t flash_init() {
    return FLOG_RESULT(mapped != nullptr);
}

void flash_lock() {
}

void flash_unlock() {
}

flog_result_t flash_open_page(uint16_t block, uint16_t page) {
    fslog_debug("flash_open_page(%d, %d)", block, page);
    open_block = block;
    open_page = page;
    return FLOG_RESULT(FLOG_SUCCESS);
}

void flash_close_page() {
    fslog_debug("flash_close_page");
}

flog_result_t flash_erase_block(uint16_t block) {
    fslog_debug("flash_erase_block(%d)", block);
    memset(mapped_sector_absolute_ptr(block, 0, 0, 0), FS_ERASE_CHAR, FS_SECTORS_PER_BLOCK_INTERNAL * FS_SECTOR_SIZE);
    return FLOG_RESULT(FLOG_SUCCESS);
}

flog_result_t flash_block_is_bad() {
    return FLOG_RESULT(0);
}

void flash_set_bad_block() {
}

void flash_commit() {
}

static void verified_memcpy(void *dst, const void *src, size_t size) {
    fslog_debug("flash_write_verified(%d, %d)", (uint8_t *)dst - (uint8_t *)mapped, size);
    for (auto i = 0; i < size; ++i) {
        assert(((uint8_t *)dst)[i] == FS_ERASE_CHAR);
    }
    memcpy(dst, src, size);
}

flog_result_t flash_read_sector(uint8_t *dst, uint8_t sector, uint16_t offset, uint16_t n) {
    fslog_debug("flash_read_sector(%p, %d, %d, %d)", dst, sector, offset, n);
    sector = sector % FS_SECTORS_PER_PAGE;
    auto src = mapped_sector_ptr(sector, offset);
    memcpy(dst, src, n);
    return FLOG_SUCCESS;
}

flog_result_t flash_read_spare(uint8_t *dst, uint8_t sector) {
    fslog_debug("flash_read_spare(%p, %d)", dst, sector);
    sector = sector % FS_SECTORS_PER_PAGE;
    auto src = mapped_sector_ptr(0, 0x804 + sector * 0x10);
    memcpy(dst, src, sizeof(flog_file_sector_spare_t));
    return FLOG_SUCCESS;
}

void flash_write_sector(uint8_t const *src, uint8_t sector, uint16_t offset, uint16_t n) {
    fslog_debug("flash_write_sector(%p, %d, %d, %d)", src, sector, offset, n);
    sector = sector % FS_SECTORS_PER_PAGE;
    auto dst = mapped_sector_ptr(sector, offset);
    verified_memcpy(dst, src, n);
}

void flash_write_spare(uint8_t const *src, uint8_t sector) {
    fslog_debug("flash_write_spare(%p, %d)", src, sector);
    sector = sector % FS_SECTORS_PER_PAGE;
    auto dst = mapped_sector_ptr(0, 0x804 + sector * 0x10);
    verified_memcpy(dst, src, sizeof(flog_file_sector_spare_t));
}

constexpr uint16_t DebugLineMax = 256;

void flash_debug_warn(char const *f, ...) {
    char buffer[DebugLineMax];
    va_list args;

    va_start(args, f);
    auto w = vsnprintf(buffer, DebugLineMax, f, args);
    va_end(args);

    buffer[w] = '\r';
    buffer[w + 1] = '\n';
    buffer[w + 2] = 0;

    printf("%s", buffer);
}

void flash_debug_error(char const *f, ...) {
    char buffer[DebugLineMax];
    va_list args;

    va_start(args, f);
    auto w = vsnprintf(buffer, DebugLineMax, f, args);
    va_end(args);

    buffer[w] = '\r';
    buffer[w + 1] = '\n';
    buffer[w + 2] = 0;

    printf("%s", buffer);
}