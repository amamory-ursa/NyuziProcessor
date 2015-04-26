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

#include <stdio.h>
#include "sdmmc.h"

#define SYS_CLOCK_HZ 50000000
#define MAX_RETRIES 100

typedef enum {
	SD_CMD_RESET = 0,
	SD_CMD_INIT = 1,
	SD_CMD_SET_BLOCK_LEN = 0x16,
	SD_CMD_READ_BLOCK = 0x17
} SDCommand;

static volatile unsigned int * const REGISTERS = (volatile unsigned int*) 0xffff0000;

// Note that the hardware signal is active low, but hardware inverts it automatically.
// So, asserted = 1 means CS=low
static void setCs(int asserted)
{
	REGISTERS[0x50 / 4] = asserted;
}

static void setClockRate(int hz)
{
	REGISTERS[0x54 / 4] = ((SYS_CLOCK_HZ / hz) / 2) - 1;
}

// Transfer a single byte bidirectionally.
static int spiTransfer(int value)
{
	REGISTERS[0x44 / 4] = value & 0xff;
	while ((REGISTERS[0x4c / 4] & 1) == 0)
		;	// Wait for transfer to finish

	return REGISTERS[0x48 / 4];
}

static void sendSdCommand(SDCommand command, unsigned int parameter)
{
	spiTransfer(0x40 | command);	
	spiTransfer((parameter >> 24) & 0xff);
	spiTransfer((parameter >> 16) & 0xff);
	spiTransfer((parameter >> 8) & 0xff);
	spiTransfer(parameter & 0xff);
	spiTransfer(0x95);	// Checksum (ignored for all but first command)
}

static int getResult()
{
	int result;
	int retryCount = 0;

	// Wait while card is busy
	do
	{
		result = spiTransfer(0xff);
		if (retryCount++ == MAX_RETRIES)
			return -1;
	}
	while (result == 0xff);
	
	return result;
}

int initSdmmcDevice()
{
	int result;
	
	setClockRate(400000);	// Slow clock rate 400khz

	// After power on, send a bunch of clocks to initialize the chip
	setCs(0);
	for (int i = 0; i < 10; i++)
		spiTransfer(0xff);

	setCs(1);

	// Reset the card
	sendSdCommand(SD_CMD_RESET, 0);
	result = getResult();
	if (result != 1)
	{
		printf("initSdmmcDevice: error %d SD_CMD_RESET\n", result);
		return -1;
	}

	// Poll until it is ready
	while (1)
	{
		sendSdCommand(SD_CMD_INIT, 0);
		result = getResult();
		if (result == 0)
			break;
		
		if (result != 1)
		{
			printf("initSdmmcDevice: error %d SD_CMD_INIT\n", result);
			return -1;
		}
	}

	// Configure the block size
	sendSdCommand(SD_CMD_SET_BLOCK_LEN, BLOCK_SIZE);
	result = getResult();
	if (result != 0)
	{
		printf("initSdmmcDevice: error %d SD_CMD_SET_BLOCK_LEN\n", result);
		return -1;
	}
		
	setClockRate(5000000);	// Increase clock rate to 5Mhz
	
	return 0;
}

int readSdmmcDevice(unsigned int blockAddress, void *ptr)
{
	int result;
	
	sendSdCommand(SD_CMD_READ_BLOCK, blockAddress);
	result = getResult();
	if (result != 0)
	{
		printf("readSdmmcDevice: error %d SD_CMD_READ_BLOCK\n", result);
		return -1;
	}
	
	for (int i = 0; i < BLOCK_SIZE; i++)
		((char*) ptr)[i] = spiTransfer(0xff);
	
	spiTransfer(0xff);	// checksum (ignored)
	return BLOCK_SIZE;
}
