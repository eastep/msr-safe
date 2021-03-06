/*
 * Copyright (c) 2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "msrsave.h"

/* Random 64 bit integer to mark bad reads for MSRs with ~0 write mask */
enum {G_COOKIE_BAD_READ = 0xFB6A58813AEA28CF};

static int is_good_value(uint64_t write_val, uint64_t mask)
{
    /* Check if value in save file represents a bad read. */
    int result = 1;
    if (~mask)
    {
        result = (write_val == (mask & write_val));
    }
    else
    {
        result = (write_val != G_COOKIE_BAD_READ);
    }
    return result;
}

static int msr_parse_whitelist(const char *whitelist_path, size_t *num_msr_ptr, uint64_t **msr_offset_ptr, uint64_t **msr_mask_ptr, FILE *error_log)
{
    enum {BUFFER_SIZE = 8192};
    int err = 0;
    int tmp_err = 0;
    int i;
    int tmp_fd = -1;
    int whitelist_fd = -1;
    size_t num_scan = 0;
    size_t num_msr = 0;
    ssize_t num_read = 0;
    ssize_t num_write = 0;
    char *whitelist_buffer = NULL;
    char *whitelist_ptr = NULL;
    uint64_t *msr_offset = NULL;
    uint64_t *msr_mask = NULL;
    struct stat whitelist_stat;
    char tmp_path[NAME_MAX] = "/tmp/msrsave_whitelist_XXXXXX";
    char err_msg[NAME_MAX];
    char copy_buffer[BUFFER_SIZE];

    /* Copy whitelist into temporary file */
    tmp_fd = mkstemp(tmp_path);
    if (tmp_fd == -1)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Creation of temporary file named\"%s\" failed!: %s\n", tmp_path, strerror(errno));
        goto exit;
    }

    whitelist_fd = open(whitelist_path, O_RDONLY);
    if (whitelist_fd == -1)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Open of whitelist file named \"%s\" failed!: %s\n", whitelist_path, strerror(errno));
        goto exit;
    }

    while ((num_read = read(whitelist_fd, copy_buffer, sizeof(copy_buffer))))
    {
        if (num_read == -1)
        {
            err = errno ? errno : -1;
            fprintf(error_log, "Read of whitelist file \"%s\" failed!: %s\n", whitelist_path, strerror(errno));
            goto exit;
        }

        num_write = write(tmp_fd, copy_buffer, num_read);
        if (num_write != num_read)
        {
            err = errno ? errno : -1;
            fprintf(error_log, "Write to temporary file \"%s\" failed!: %s\n", tmp_path, strerror(errno));
            goto exit;
        }
    }

    tmp_err = close(tmp_fd);
    tmp_fd = -1;
    if (tmp_err == -1)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Close of temporary file named\"%s\" failed!: %s\n", tmp_path, strerror(errno));
        goto exit;
    }

    tmp_err = close(whitelist_fd);
    whitelist_fd = -1;
    if (tmp_err == -1)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Close of whitelist file named\"%s\" failed!: %s\n", whitelist_path, strerror(errno));
        goto exit;
    }

    *msr_offset_ptr = NULL;
    *msr_mask_ptr = NULL;

    /* Figure out how big the whitelist file is */
    tmp_err = stat(tmp_path, &whitelist_stat);
    if (tmp_err != 0)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "stat() of %s failed!: %s\n", tmp_path, strerror(errno));
        goto exit;
    }

    if (whitelist_stat.st_size == 0)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Whitelist file (%s) size is zero!: %s\n", tmp_path, strerror(errno));
        goto exit;
    }

    /* Allocate buffer for file contents and null terminator */
    whitelist_buffer = (char*)malloc(whitelist_stat.st_size + 1);
    if (!whitelist_buffer)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Could not allocate array of size %zu!: %s\n", whitelist_stat.st_size, strerror(errno));
        goto exit;
    }
    whitelist_buffer[whitelist_stat.st_size] = '\0';

    /* Open file */
    tmp_fd = open(tmp_path, O_RDONLY);
    if (tmp_fd == -1)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Could not open whitelist temporary file \"%s\"!: %s\n", tmp_path, strerror(errno));
        goto exit;
    }

    /* Read contents */
    num_read = read(tmp_fd, whitelist_buffer, whitelist_stat.st_size);
    if (num_read != whitelist_stat.st_size)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Contents read from whitelist file is too small: %zu < %zu!: %s\n", num_read, whitelist_stat.st_size, strerror(errno));
        goto exit;
    }

    /* close file */
    tmp_err = close(tmp_fd);
    tmp_fd = -1;
    if (tmp_err)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Unable to close temporary whitelist file called \"%s\": %s\n", tmp_path, strerror(errno));
        goto exit;
    }

    /* Count the number of new lines in the file that do not start with # */
    whitelist_ptr = whitelist_buffer;
    while (whitelist_ptr && *whitelist_ptr)
    {
        if (*whitelist_ptr != '#')
        {
            ++num_msr;
        }
        whitelist_ptr = strchr(whitelist_ptr, '\n');
        if (whitelist_ptr)
        {
            ++whitelist_ptr;
        }
    }
    *num_msr_ptr = num_msr;

    /* Allocate buffers for parsed whitelist */
    msr_offset = *msr_offset_ptr = (uint64_t *)malloc(sizeof(uint64_t) * num_msr);
    if (!msr_offset)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Unable to allocate msr offset data of size: %zu!: %s\n", sizeof(uint64_t) * num_msr, strerror(errno));
        goto exit;
    }

    msr_mask = *msr_mask_ptr = (uint64_t *)malloc(sizeof(uint64_t) * num_msr);
    if (!msr_mask)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Unable to allocate msr mask data of size: %zu!: %s\n", sizeof(uint64_t) * num_msr, strerror(errno));
        goto exit;
    }

    /* Parse the whitelist */
    const char *whitelist_format = "%zX %zX\n";
    whitelist_ptr = whitelist_buffer;
    for (i = 0; i < num_msr; ++i)
    {
        while (*whitelist_ptr == '#')
        {
            /* '#' is on first position means line is a comment, treat next line. */
            whitelist_ptr = strchr(whitelist_ptr, '\n');
            if (whitelist_ptr)
            {
                whitelist_ptr++; /* Move the pointer to the next line */
            }
            else
            {
                err = -1;
                fprintf(stderr, "Error: Failed to parse whitelist file named \"%s\"\n", whitelist_path);
                goto exit;
            }
        }
        num_scan = sscanf(whitelist_ptr, whitelist_format, msr_offset + i, msr_mask + i);
        if (num_scan != 2)
        {
            err = -1;
            fprintf(stderr, "Error: Failed to parse whitelist file named \"%s\"\n", whitelist_path);
            goto exit;
        }
        whitelist_ptr = strchr(whitelist_ptr, '\n');
        whitelist_ptr++; /* Move the pointer to the next line */
        if (!whitelist_ptr)
        {
            err = -1;
            fprintf(stderr, "Error: Failed to parse whitelist file named \"%s\"\n", whitelist_path);
            goto exit;
        }
    }

exit:
    unlink(tmp_path);
    if (tmp_fd != -1)
    {
        close(tmp_fd);
    }
    if (whitelist_fd != -1)
    {
        close(whitelist_fd);
    }
    if (whitelist_buffer)
    {
        free(whitelist_buffer);
    }
    return err;
}

int msr_save(const char *save_path, const char *whitelist_path, const char *msr_path_format, int num_cpu, FILE *output_log, FILE *error_log)
{
    int err = 0;
    int tmp_err = 0;
    int i, j;
    int msr_fd = -1;
    size_t num_msr = 0;
    uint64_t *msr_offset = NULL;
    uint64_t *msr_mask = NULL;
    uint64_t *save_buffer = NULL;
    FILE *save_fid = NULL;

    err = msr_parse_whitelist(whitelist_path, &num_msr, &msr_offset, &msr_mask, error_log);
    if (err)
    {
        goto exit;
    }
    if (!msr_offset || !msr_mask)
    {
        err = -1;
        goto exit;
    }

    /* Allocate save buffer, a 2-D array over msr offset and then CPU
       (offset major ordering) */
    save_buffer = (uint64_t *)malloc(num_msr * num_cpu * sizeof(uint64_t));
    if (!save_buffer)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Unable to allocate msr save state buffer of size: %zu!: %s\n", num_msr * num_cpu * sizeof(uint64_t), strerror(errno));
        goto exit;
    }

    /* Open all MSR files.
     * Read ALL existing data
     * Pass through the whitelist mask. */
    for (i = 0; i < num_cpu; ++i)
    {
        char msr_file_name[NAME_MAX];
        snprintf(msr_file_name, NAME_MAX, msr_path_format, i);
        msr_fd = open(msr_file_name, O_RDWR);
        if (msr_fd == -1)
        {
            err = errno ? errno : -1;
            fprintf(error_log, "Could not open MSR file \"%s\"!: %s\n", msr_file_name, strerror(errno));
            goto exit;
        }
        for (j = 0; j < num_msr; ++j)
        {
            ssize_t read_count = pread(msr_fd, save_buffer + i * num_msr + j,
                                       sizeof(uint64_t), msr_offset[j]);
            if (read_count != sizeof(uint64_t))
            {
                fprintf(error_log, "Warning: Failed to read msr value 0x%zX from MSR file \"%s\"!\n", msr_offset[j], msr_file_name);
                errno = 0;
                if (~(msr_mask[j]))
                {
                    save_buffer[i * num_msr + j] = ~(msr_mask[j]);
                }
                else
                {
                    save_buffer[i * num_msr + j] = G_COOKIE_BAD_READ;
                }
            }
            else
            {
                save_buffer[i * num_msr + j] &= msr_mask[j];
                if (~(msr_mask[j]) == 0ULL &&
                    save_buffer[i * num_msr + j] == G_COOKIE_BAD_READ)
                {
                    fprintf(error_log, "Error: Extremely unlikely event, read value from MSR that matches our random 64 bit integer used to mark bad reads.\n");
                    err = -1;
                    goto exit;
                }
            }
        }
        tmp_err = close(msr_fd);
        msr_fd = -1;
        if (tmp_err)
        {
            err = errno ? errno : -1;
            fprintf(error_log, "Could not close MSR file \"%s\"!: %s\n", msr_file_name, strerror(errno));
            goto exit;
        }
    }

    /* Open output file. */
    save_fid = fopen(save_path, "w");
    if (!save_fid)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Could not open output file \"%s\"!: %s\n", save_path, strerror(errno));
        goto exit;
    }

    size_t num_write = fwrite(save_buffer, sizeof(uint64_t), num_msr * num_cpu, save_fid);
    if (num_write != num_msr * num_cpu)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Could not write all values to output file \"%s\"!: %s\n", save_path, strerror(errno));
        goto exit;
    }

    tmp_err = fclose(save_fid);
    save_fid = NULL;
    if (tmp_err)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Could not close MSR file \"%s\"!: %s\n", save_path, strerror(errno));
        goto exit;
    }

    /* Clean up memory and files */
exit:
    if (save_buffer)
    {
        free(save_buffer);
    }
    if (msr_offset)
    {
        free(msr_offset);
    }
    if (msr_mask)
    {
        free(msr_mask);
    }
    if (save_fid)
    {
        fclose(save_fid);
    }
    if (msr_fd != -1)
    {
        close(msr_fd);
    }
    return err;
}

int msr_restore(const char *restore_path, const char *whitelist_path, const char *msr_path_format, int num_cpu, FILE *output_log, FILE *error_log)
{
    int err = 0;
    int tmp_err = 0;
    int i, j;
    int msr_fd = -1;
    int do_print_header = 1;
    size_t num_msr = 0;
    uint64_t read_val = 0;
    uint64_t masked_val = 0;
    uint64_t write_val = 0;
    uint64_t *msr_offset = NULL;
    uint64_t *msr_mask = NULL;
    uint64_t *restore_buffer = NULL;
    FILE *restore_fid = NULL;
    struct stat restore_stat;
    struct stat whitelist_stat;
    char err_msg[NAME_MAX];

    err = msr_parse_whitelist(whitelist_path, &num_msr, &msr_offset, &msr_mask, error_log);
    if (err)
    {
        goto exit;
    }
    if (!msr_offset || !msr_mask)
    {
        err = -1;
        goto exit;
    }

    /* Check that the timestamp of the restore file is after the timestamp
       for the whitelist file */
    tmp_err = stat(restore_path, &restore_stat);
    if (tmp_err != 0)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "stat() of %s failed!: %s\n", restore_path, strerror(errno));
        goto exit;
    }

    tmp_err = stat(whitelist_path, &whitelist_stat);
    if (tmp_err != 0)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "stat() of %s failed!: %s\n", whitelist_path, strerror(errno));
        goto exit;
    }

    if (restore_stat.st_mtime < whitelist_stat.st_mtime)
    {
        err = -1;
        fprintf(error_log, "Error: whitelist was modified after restore file was written!\n");
        goto exit;
    }

    /* Allocate restore buffer, a 2-D array over msr offset and then CPU
       (offset major ordering) */
    restore_buffer = (uint64_t *)malloc(num_msr * num_cpu * sizeof(uint64_t));
    if (!restore_buffer)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Unable to allocate msr restore state buffer of size: %zu!: %s\n", num_msr * num_cpu * sizeof(uint64_t), strerror(errno));
        goto exit;
    }

    restore_fid = fopen(restore_path, "r");
    if (!restore_fid)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Could not open restore file \"%s\"!: %s\n", restore_path, strerror(errno));
        goto exit;
    }

    size_t num_read = fread(restore_buffer, sizeof(uint64_t), num_msr * num_cpu, restore_fid);
    if (num_read != num_msr * num_cpu || fgetc(restore_fid) != EOF)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Could not read all values from input file \"%s\"!: %s\n", restore_path, strerror(errno));
        goto exit;
    }

    tmp_err = fclose(restore_fid);
    restore_fid = NULL;
    if (tmp_err)
    {
        err = errno ? errno : -1;
        fprintf(error_log, "Could not close MSR file \"%s\"!: %s\n", restore_path, strerror(errno));
        goto exit;
    }

    /* Open all MSR files.
     * Read ALL existing data
     * Pass through the whitelist mask
     * Or in restore values
     * Write back to MSR files. */
    for (i = 0; i < num_cpu; ++i)
    {
        char msr_file_name[NAME_MAX];
        snprintf(msr_file_name, NAME_MAX, msr_path_format, i);
        msr_fd = open(msr_file_name, O_RDWR);
        if (msr_fd == -1)
        {
            err = errno ? errno : -1;
            fprintf(error_log, "Could not open MSR file \"%s\"!: %s\n", msr_file_name, strerror(errno));
            goto exit;
        }
        for (j = 0; j < num_msr; ++j)
        {
            write_val = restore_buffer[i * num_msr + j];
            if (is_good_value(write_val, msr_mask[j]))
            {
                /* Value was read properly when save file was created */
                ssize_t count = pread(msr_fd, &read_val, sizeof(uint64_t), msr_offset[j]);
                masked_val = (read_val & msr_mask[j]);
                if (count != sizeof(uint64_t))
                {
		    fprintf(error_log, "Warning: Failed to read msr value at offset 0x%016zX from MSR file \"%s\"!: %s\n", msr_offset[j], msr_file_name, strerror(errno));
                    errno = 0;
                }
                else if (write_val != masked_val)
                {
                    /* Value has changed, and needs to be restored */
                    write_val |= (read_val & ~(msr_mask[j]));
                    count = pwrite(msr_fd, &write_val, sizeof(uint64_t), msr_offset[j]);
                    if (count != sizeof(uint64_t))
                    {
                        fprintf(error_log, "Warning: Failed to write msr value at offset 0x%016zX to MSR file \"%s\"!: %s\n", msr_offset[j], msr_file_name, strerror(errno));
                        errno = 0;
                    }
                    else
                    {
                        if (do_print_header)
                        {
                            fprintf(output_log, "offset, read, restored\n");
                            do_print_header = 0;
                        }
                        fprintf(output_log, "0x%016zX, 0x%016zX, 0x%016zX\n", msr_offset[j], read_val, write_val);
                    }
                }
            }
        }
        tmp_err = close(msr_fd);
        msr_fd = -1;
        if (tmp_err)
        {
            err = errno ? errno : -1;
            fprintf(error_log, "Could not close MSR file \"%s\"!: %s\n", msr_file_name, strerror(errno));
            goto exit;
        }
    }

    /* Clean up memory and files */
exit:
    if (restore_buffer)
    {
        free(restore_buffer);
    }
    if (restore_fid)
    {
        fclose(restore_fid);
    }
    if (msr_offset)
    {
        free(msr_offset);
    }
    if (msr_mask)
    {
        free(msr_mask);
    }
    if (msr_fd != -1)
    {
        close(msr_fd);
    }
    return err;
}
