//
// Copyright 2011-2015 Jeff Bush
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "device.h"
#include "sdmmc.h"
#include "util.h"

// SD/MMC interface, SPI mode.
// https://www.sdcard.org/downloads/pls/pdf/index.php?p=Part1_Physical_Layer_Simplified_Specification_Ver6.00.jpg

#define INIT_CLOCKS 80
#define SD_COMMAND_LENGTH 6
#define DATA_TOKEN 0xfe

// Commands
enum sd_command
{
    CMD_GO_IDLE_STATE = 0,
    CMD_SEND_OP_COND = 1,
    CMD_SET_BLOCKLEN = 16,
    CMD_READ_SINGLE_BLOCK = 17,
    CMD_WRITE_SINGLE_BLOCK = 24
};

enum sd_state
{
    STATE_INIT_WAIT,
    STATE_IDLE,
    STATE_RECEIVE_COMMAND,
    STATE_READ_CMD_RESPONSE,
    STATE_READ_DATA_TOKEN,
    STATE_SEND_RESULT,
    STATE_READ_TRANSFER,
    STATE_WRITE_CMD_RESPONSE,
    STATE_WRITE_DATA_TOKEN,
    STATE_WRITE_TRANSFER,
    STATE_WRITE_DATA_RESPONSE
};

static uint32_t block_dev_size;
static int block_fd = -1;
static enum sd_state current_state;
static uint32_t chip_select;
static uint32_t state_delay;
static uint32_t transfer_block_address;
static uint32_t transfer_count;
static uint32_t block_length;
static uint32_t init_clock_count;
static uint8_t current_command[SD_COMMAND_LENGTH];
static uint32_t current_command_length;
static char *block_buffer;
static bool in_idle_state = false;

int open_sdmmc_device(const char *filename)
{
    struct stat fs;
    if (block_fd != -1)
        return 0;	// Already open

    if (stat(filename, &fs) < 0)
    {
        perror("open_sdmmc_device: failed to stat block device file");
        return -1;
    }

    block_dev_size = (uint32_t) fs.st_size;
    block_fd = open(filename, O_RDWR);
    if (block_fd < 0)
    {
        perror("open_sdmmc_device: failed to open block device file");
        return -1;
    }

    return 0;
}

void close_sdmmc_device(void)
{
    assert(block_fd > 0);
    close(block_fd);
}

static uint32_t read_little_endian(const uint8_t *values)
{
    return (uint32_t)((values[0] << 24) | (values[1] << 16) | (values[2] << 8)
        | values[3]);
}

static void process_command(const uint8_t *command)
{
    switch (command[0] & 0x3f)
    {
        case CMD_GO_IDLE_STATE:
            // If a virtual block device wasn't specified, don't initialize
            if (block_fd > 0)
            {
                in_idle_state = true;
                current_state = STATE_SEND_RESULT;
            }

            break;

        case CMD_SEND_OP_COND:
            current_state = STATE_SEND_RESULT;
            in_idle_state = false;
            break;

        case CMD_SET_BLOCKLEN:
            if (in_idle_state)
            {
                printf("CMD_SET_BLOCKLEN: card not ready\n");
                exit(1);
            }

            block_length = read_little_endian(command + 1);
            block_buffer = realloc(block_buffer, block_length);
            current_state = STATE_SEND_RESULT;
            break;

        case CMD_READ_SINGLE_BLOCK:
            if (in_idle_state)
            {
                printf("CMD_READ_SINGLE_BLOCK: card not ready\n");
                exit(1);
            }

            transfer_block_address = read_little_endian(command + 1) * block_length;
            if (lseek(block_fd, transfer_block_address, SEEK_SET) < 0)
            {
                perror("CMD_READ_SINGLE_BLOCK: seek failed");
                exit(1);
            }

            if (read(block_fd, block_buffer, block_length) != block_length)
            {
                printf("CMD_READ_SINGLE_BLOCK: read failed for block\n");
                exit(1);
            }

            transfer_count = 0;
            current_state = STATE_READ_CMD_RESPONSE;
            state_delay = next_random() & 0xf; // Wait a random amount of time
            break;

        case CMD_WRITE_SINGLE_BLOCK:
            if (in_idle_state)
            {
                printf("CMD_READ_SINGLE_BLOCK: card not ready\n");
                exit(1);
            }

            transfer_block_address = read_little_endian(command + 1) * block_length;
            transfer_count = 0;
            current_state = STATE_WRITE_CMD_RESPONSE;
            state_delay = next_random() & 0xf; // Wait a random amount of time
            break;

        default:
            printf("sdmmc error: unknown command %02x\n", command[0]);
            exit(1);
    }
}

int transfer_sdmmc_byte(int value)
{
    int result = 0xff;
    switch (current_state)
    {
        case STATE_INIT_WAIT:
            init_clock_count += 8;
            if (!chip_select && init_clock_count < INIT_CLOCKS)
            {
                printf("sdmmc error: command posted before card initialized 1\n");
                exit(1);
            }

        // Falls through

        case STATE_IDLE:
            if (!chip_select && (value & 0xc0) == 0x40)
            {
                current_state = STATE_RECEIVE_COMMAND;
                current_command[0] = value & 0xff;
                current_command_length = 1;
            }

            break;

        case STATE_RECEIVE_COMMAND:
            if (!chip_select)
            {
                current_command[current_command_length++] = value & 0xff;
                if (current_command_length == SD_COMMAND_LENGTH)
                {
                    process_command(current_command);
                    current_command_length = 0;
                }
            }

            break;

        case STATE_SEND_RESULT:
            current_state = STATE_IDLE;
            result = (int) in_idle_state;
            break;

        case STATE_READ_CMD_RESPONSE:
            if (state_delay == 0)
            {
                current_state = STATE_READ_DATA_TOKEN;
                result = 0; // Signal ready
                state_delay = next_random() & 0xf;
            }
            else
                state_delay--;

            break;

        case STATE_READ_DATA_TOKEN:
            if (state_delay == 0)
            {
                current_state = STATE_READ_TRANSFER;
                result = DATA_TOKEN; // Send data token to start block
                state_delay = block_length + 2;
            }
            else
                state_delay--;

            break;

        case STATE_READ_TRANSFER:
            // This also adds a 2 byte checksum (which is ignored)
            if (transfer_count < block_length)
                result = block_buffer[transfer_count];
            else if (transfer_count == block_length + 1)
                current_state = STATE_IDLE;

            transfer_count++;
            break;

        case STATE_WRITE_CMD_RESPONSE:
            if (state_delay == 0)
            {
                current_state = STATE_WRITE_DATA_TOKEN;
                result = 0; // Signal ready
                state_delay = block_length + 2;
            }
            else
                state_delay--;

            break;

        case STATE_WRITE_DATA_TOKEN:
            // Wait until we see the data token
            if (value == DATA_TOKEN)
                current_state = STATE_WRITE_TRANSFER;

            break;

        case STATE_WRITE_TRANSFER:
            // This also adds a 2 byte checksum (which is ignored)
            if (transfer_count < block_length)
                block_buffer[transfer_count] = value & 0xff;
            else if (transfer_count == block_length + 1)
                current_state = STATE_WRITE_DATA_RESPONSE;

            transfer_count++;
            break;

        case STATE_WRITE_DATA_RESPONSE:
            current_state = STATE_IDLE;
            result = 0x05;  // Data accepted

            if (lseek(block_fd, transfer_block_address, SEEK_SET) < 0)
            {
                perror("CMD_READ_SINGLE_BLOCK: seek failed");
                exit(1);
            }

            if (write(block_fd, block_buffer, block_length) != block_length)
            {
                printf("CMD_READ_SINGLE_BLOCK: write failed for block\n");
                exit(1);
            }

            break;
    }

    return result;
}

void set_sdmmc_cs(int value)
{
    chip_select = value & 1;
}
