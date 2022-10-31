/****************************************************************************
 *
 * Copyright (C) 2019-2022 PX4 Development Team. All rights reserved.
 * Author: Igor Misic <igy1000mb@gmail.com>
 * Author: Julian Oes <julian@oes.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *	notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	notice, this list of conditions and the following disclaimer in
 *	the documentation and/or other materials provided with the
 *	distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *	used to endorse or promote products derived from this software
 *	without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/


#if (CONFIG_STM32_HAVE_IP_DMA_V1)
//Do nothing. IP DMA V1 MCUs are not supported.
#else

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/micro_hal.h>
#include <stm32_dma.h>
#include <stm32_tim.h>
#include <px4_arch/dshot.h>
#include <px4_arch/io_timer.h>
#include <drivers/drv_dshot.h>

#include <stdio.h>
#include <drivers/drv_input_capture.h>
#include <drivers/drv_hrt.h>


#define MOTOR_PWM_BIT_1				14u
#define MOTOR_PWM_BIT_0				7u
#define DSHOT_TIMERS				MAX_IO_TIMERS
#define MOTORS_NUMBER				DIRECT_PWM_OUTPUT_CHANNELS
#define ONE_MOTOR_DATA_SIZE			16u
#define ONE_MOTOR_BUFF_SIZE			17u
#define ALL_MOTORS_BUF_SIZE			(MOTORS_NUMBER * ONE_MOTOR_BUFF_SIZE)
#define DSHOT_THROTTLE_POSITION		5u
#define DSHOT_TELEMETRY_POSITION	4u
#define NIBBLES_SIZE 				4u
#define DSHOT_NUMBER_OF_NIBBLES		3u
#define DSHOT_END_OF_STREAM 		16u
#define MAX_NUM_CHANNELS_PER_TIMER	4u // CCR1-CCR4

#define DSHOT_DMA_SCR (DMA_SCR_PRIHI | DMA_SCR_MSIZE_32BITS | DMA_SCR_PSIZE_32BITS | DMA_SCR_MINC | \
		       DMA_SCR_DIR_M2P | DMA_SCR_TCIE | DMA_SCR_HTIE | DMA_SCR_TEIE | DMA_SCR_DMEIE)

#define DSHOT_TELEMETRY_DMA_SCR (DMA_SCR_PRIHI | DMA_SCR_MSIZE_16BITS | DMA_SCR_PSIZE_16BITS | DMA_SCR_MINC | \
				 DMA_SCR_DIR_P2M | DMA_SCR_TCIE | DMA_SCR_TEIE | DMA_SCR_DMEIE)

typedef struct dshot_handler_t {
	bool			init;
	DMA_HANDLE		dma_handle;
	uint32_t		dma_size;
} dshot_handler_t;

#if defined(CONFIG_ARMV7M_DCACHE)
#  define DMA_BUFFER_MASK    (ARMV7M_DCACHE_LINESIZE - 1)
#  define DMA_ALIGN_UP(n)    (((n) + DMA_BUFFER_MASK) & ~DMA_BUFFER_MASK)
#else
#define DMA_ALIGN_UP(n) (n)
#endif
#define DSHOT_BURST_BUFFER_SIZE(motors_number) (DMA_ALIGN_UP(sizeof(uint32_t)*ONE_MOTOR_BUFF_SIZE*motors_number))

static dshot_handler_t dshot_handler[DSHOT_TIMERS] = {};
static uint8_t dshot_burst_buffer_array[DSHOT_TIMERS * DSHOT_BURST_BUFFER_SIZE(MAX_NUM_CHANNELS_PER_TIMER)]
px4_cache_aligned_data() = {};
static uint32_t *dshot_burst_buffer[DSHOT_TIMERS] = {};

static uint16_t dshot_capture_buffer[32] px4_cache_aligned_data() = {};
static size_t dshot_capture_buffer_size = sizeof(dshot_capture_buffer) * sizeof(dshot_capture_buffer[0]);

static struct hrt_call _call;

static void do_capture(DMA_HANDLE handle, uint8_t status, void *arg);
static void process_capture_results(void *arg);
static unsigned calculate_period(void);

static uint32_t read_ok = 0;
static uint32_t read_fail_nibble = 0;
static uint32_t read_fail_crc = 0;
static uint32_t read_fail_zero = 0;

static bool enable_bidirectional_dshot = true;

static uint32_t _dshot_frequency = 0;

static uint32_t _motor_to_capture = 0;
static uint32_t _periods[4] = {0, 0, 0, 0};
static bool _periods_ready = false;

uint8_t nibbles_from_mapped(uint8_t mapped)
{
	switch (mapped) {
	case 0x19:
		return 0x0;

	case 0x1B:
		return 0x1;

	case 0x12:
		return 0x2;

	case 0x13:
		return 0x3;

	case 0x1D:
		return 0x4;

	case 0x15:
		return 0x5;

	case 0x16:
		return 6;

	case 0x17:
		return 7;

	case 0x1a:
		return 8;

	case 0x09:
		return 9;

	case 0x0A:
		return 0x0A;

	case 0x0B:
		return 0x0B;

	case 0x1E:
		return 0x0C;

	case 0x0D:
		return 0x0D;

	case 0x0E:
		return 0x0E;

	case 0x0F:
		return 0x0F;

	default:
		// Unknown mapped
		return 0xff;
	}
}

unsigned calculate_period(void)
{
	uint32_t value = 0;

	// We start off with high
	uint32_t high = 1;

	unsigned shifted = 0;
	unsigned previous = 0;

	for (unsigned i = 1; i < (32); ++i) {

		// We can ignore the very first data point as it's the pulse before it starts.
		if (i > 1) {

			if (dshot_capture_buffer[i] == 0) {
				// Once we get zeros we're through.
				break;
			}

			const uint32_t bits = (dshot_capture_buffer[i] - previous + 5) / 20;

			for (unsigned bit = 0; bit < bits; ++bit) {
				value = value << 1;
				value |= high;
				++shifted;
			}

			// The next edge toggles.
			high = !high;
		}

		previous = dshot_capture_buffer[i];
	}

	if (shifted == 0) {
		// no data yet, or this time
		++read_fail_zero;
		return 0;
	}

	// We need to make sure we shifted 21 times. We might have missed some low "pulses" at the very end.
	value = value << (21 - shifted);

	// Note: At 0 throttle, the value is 0x1AD6AE, so 0b110101101011010101110

	// From GCR to eRPM according to:
	// https://brushlesswhoop.com/dshot-and-bidirectional-dshot/#erpm-transmission
	unsigned gcr = (value ^ (value >> 1));

	uint32_t data = 0;

	// 20bits -> 5 mapped -> 4 nibbles
	for (unsigned i = 0; i < 4; ++i) {
		uint32_t nibble = nibbles_from_mapped(gcr & (0x1F)) << (4 * i);

		if (nibble == 0xff) {
			++read_fail_nibble;
			return 0;
		}

		data |= nibble;
		gcr = gcr >> 5;
	}

	unsigned shift = (data & 0xE000) >> 13;
	unsigned period = ((data & 0x1FF0) >> 4) << shift;
	unsigned crc = (data & 0xf);

	unsigned payload = (data & 0xFFF0) >> 4;
	unsigned calculated_crc = (~(payload ^ (payload >> 4) ^ (payload >> 8))) & 0x0F;

	if (crc != calculated_crc) {
		++read_fail_crc;
		return 0;
	}

	++read_ok;
	return period;
}


int up_dshot_init(uint32_t channel_mask, unsigned dshot_pwm_freq)
{
	_dshot_frequency = dshot_pwm_freq;

	unsigned buffer_offset = 0;

	for (int timer_index = 0; timer_index < DSHOT_TIMERS; timer_index++) {
		dshot_handler[timer_index].init = false;
	}

	for (unsigned timer = 0; timer < DSHOT_TIMERS; ++timer) {
		if (io_timers[timer].base == 0) { // no more timers configured
			break;
		}

		// we know the uint8_t* cast to uint32_t* is fine, since we're aligned to cache line size
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
		dshot_burst_buffer[timer] = (uint32_t *)&dshot_burst_buffer_array[buffer_offset];
#pragma GCC diagnostic pop
		buffer_offset += DSHOT_BURST_BUFFER_SIZE(io_timers_channel_mapping.element[timer].channel_count_including_gaps);

		if (buffer_offset > sizeof(dshot_burst_buffer_array)) {
			return -EINVAL; // something is wrong with the board configuration or some other logic
		}
	}

	int channels_init_mask = 0xf;
	int ret_val = 0;

	return ret_val == OK ? channels_init_mask : ret_val;
}

void up_dshot_trigger(void)
{
	/* Init channels */
	int ret_val = OK;
	int channel_mask = 0xf;
	int channels_init_mask = 0;

	for (unsigned channel = 0; (channel_mask != 0) && (channel < MAX_TIMER_IO_CHANNELS) && (OK == ret_val); channel++) {
		if (channel_mask & (1 << channel)) {
			uint8_t timer = timer_io_channels[channel].timer_index;

			if (io_timers[timer].dshot.dma_base == 0) { // board does not configure dshot on this timer
				continue;
			}

			ret_val = io_timer_unallocate_channel(channel);
			ret_val = io_timer_channel_init(channel,
							enable_bidirectional_dshot ? IOTimerChanMode_DshotInverted : IOTimerChanMode_Dshot, NULL, NULL);
			channel_mask &= ~(1 << channel);

			if (OK == ret_val) {
				dshot_handler[timer].init = true;
				channels_init_mask |= 1 << channel;

			} else if (ret_val == -EBUSY) {
				/* either timer or channel already used - this is not fatal */
				ret_val = 0;
			}
		}
	}

	for (uint8_t timer_index = 0; (timer_index < 1) && (OK == ret_val); timer_index++) {

		if (true == dshot_handler[timer_index].init) {
			dshot_handler[timer_index].dma_size = io_timers_channel_mapping.element[timer_index].channel_count_including_gaps *
							      ONE_MOTOR_BUFF_SIZE;
			io_timer_set_dshot_mode(timer_index, _dshot_frequency,
						io_timers_channel_mapping.element[timer_index].channel_count_including_gaps);


			if (dshot_handler[timer_index].dma_handle != NULL) {
				stm32_dmafree(dshot_handler[timer_index].dma_handle);
			}

			dshot_handler[timer_index].dma_handle = stm32_dmachannel(io_timers[timer_index].dshot.dmamap);
		}


		if (NULL == dshot_handler[timer_index].dma_handle) {
			ret_val = ERROR;
		}
	}

	for (uint8_t timer = 0; (timer < 1); timer++) {

		if (true == dshot_handler[timer].init) {

			// Flush cache so DMA sees the data
			up_clean_dcache((uintptr_t)dshot_burst_buffer[timer],
					(uintptr_t)dshot_burst_buffer[timer] +
					DSHOT_BURST_BUFFER_SIZE(io_timers_channel_mapping.element[timer].channel_count_including_gaps));

			px4_stm32_dmasetup(dshot_handler[timer].dma_handle,
					   io_timers[timer].base + STM32_GTIM_DMAR_OFFSET,
					   (uint32_t)(dshot_burst_buffer[timer]),
					   dshot_handler[timer].dma_size,
					   DSHOT_DMA_SCR);

			// Clean UDE flag before DMA is started
			io_timer_update_dma_req(timer, false);
			// Trigger DMA (DShot Outputs)
			stm32_dmastart(dshot_handler[timer].dma_handle, do_capture, NULL, false);
			io_timer_update_dma_req(timer, true);
		}
	}

	io_timer_set_enable(true, enable_bidirectional_dshot ? IOTimerChanMode_DshotInverted : IOTimerChanMode_Dshot,
			    IO_TIMER_ALL_MODES_CHANNELS);
}

void do_capture(DMA_HANDLE handle, uint8_t status, void *arg)
{
	(void)handle;
	(void)status;
	(void)arg;

	if (_periods_ready) {
		// The periods need to be collected first, so we have to skip it this time.
		return;
	}

	if (_motor_to_capture >= 4) {
		// We only support the first 4 for now.
		return;
	}

	// TODO: this things are still somewhat hard-coded. And I'm probably confused
	// regarding channels and indexes.
	const int capture_timer = 0;
	const int capture_channel = _motor_to_capture;

	for (unsigned timer = 0; timer < DSHOT_TIMERS; ++timer) {
		if (dshot_handler[timer].dma_handle != NULL) {
			stm32_dmastop(dshot_handler[timer].dma_handle);
		}
	}

	dshot_handler[capture_timer].dma_size = 32;

	if (dshot_handler[0].dma_handle != NULL) {
		stm32_dmafree(dshot_handler[0].dma_handle);
	}

	// TODO: We should probably do this at another level board specific.
	//       However, right now the dma handles are all hard-coded to the UP(date) source
	//       rather than the capture compare one.
	switch (timer_io_channels[_motor_to_capture].timer_channel) {
	case 1:
		dshot_handler[capture_timer].dma_handle = stm32_dmachannel(DMAMAP_DMA12_TIM5CH1_0);
		break;

	case 2:
		dshot_handler[capture_timer].dma_handle = stm32_dmachannel(DMAMAP_DMA12_TIM5CH2_0);
		break;

	case 3:
		dshot_handler[capture_timer].dma_handle = stm32_dmachannel(DMAMAP_DMA12_TIM5CH3_0);
		break;

	case 4:
		dshot_handler[capture_timer].dma_handle = stm32_dmachannel(DMAMAP_DMA12_TIM5CH4_0);
		break;
	}

	memset(dshot_capture_buffer, 0, sizeof(dshot_capture_buffer));
	up_clean_dcache((uintptr_t)dshot_capture_buffer,
			(uintptr_t)dshot_capture_buffer +
			dshot_capture_buffer_size);

	up_invalidate_dcache((uintptr_t)dshot_capture_buffer,
			     (uintptr_t)dshot_capture_buffer +
			     dshot_capture_buffer_size);

	px4_stm32_dmasetup(dshot_handler[0].dma_handle,
			   io_timers[capture_timer].base + STM32_GTIM_DMAR_OFFSET,
			   (uint32_t)(&dshot_capture_buffer[0]),
			   32,
			   DSHOT_TELEMETRY_DMA_SCR);

	// TODO: check retval?
	io_timer_unallocate_channel(capture_channel);
	io_timer_channel_init(capture_channel, IOTimerChanMode_CaptureDMA, NULL, NULL);
	io_timer_set_enable(true, IOTimerChanMode_CaptureDMA, 1 << capture_channel);

	up_input_capture_set(capture_channel, Both, 0, NULL, NULL);

	io_timer_capture_update_dma_req(capture_timer, false);
	io_timer_set_capture_mode(capture_timer, _dshot_frequency, capture_channel);

	stm32_dmastart(dshot_handler[capture_timer].dma_handle, NULL, NULL, false);
	io_timer_capture_update_dma_req(capture_timer, true);

	// It takes around 85 us for the ESC to respond, so we should have a result after 150 us, surely.
	hrt_call_after(&_call, 150, process_capture_results, NULL);
}

void process_capture_results(void *arg)
{
	(void)arg;

	up_invalidate_dcache((uintptr_t)dshot_capture_buffer,
			     (uintptr_t)dshot_capture_buffer +
			     dshot_capture_buffer_size);

	if (dshot_handler[0].dma_handle != NULL) {

		stm32_dmastop(dshot_handler[0].dma_handle);
	}

	/* Init channels */
	int ret_val = OK;
	int channel_mask = 0xf;
	int channels_init_mask = 0;

	for (unsigned channel = 0; (channel_mask != 0) && (channel < MAX_TIMER_IO_CHANNELS) && (OK == ret_val); channel++) {
		if (channel_mask & (1 << channel)) {
			uint8_t timer = timer_io_channels[channel].timer_index;

			if (io_timers[timer].dshot.dma_base == 0) { // board does not configure dshot on this timer
				continue;
			}

			ret_val = io_timer_unallocate_channel(channel);
			ret_val = io_timer_channel_init(channel,
							enable_bidirectional_dshot ? IOTimerChanMode_DshotInverted : IOTimerChanMode_Dshot, NULL, NULL);
			channel_mask &= ~(1 << channel);

			if (OK == ret_val) {
				dshot_handler[timer].init = true;
				channels_init_mask |= 1 << channel;

			} else if (ret_val == -EBUSY) {
				/* either timer or channel already used - this is not fatal */
				ret_val = 0;
			}
		}
	}

	// TODO: fix order
	_periods[_motor_to_capture] = calculate_period();

	if (_motor_to_capture == 3) {
		_periods_ready = true;
	}

	_motor_to_capture = (_motor_to_capture + 1) % 4;
}


/**
* bits 	1-11	- throttle value (0-47 are reserved, 48-2047 give 2000 steps of throttle resolution)
* bit 	12		- dshot telemetry enable/disable
* bits 	13-16	- XOR checksum
**/
void dshot_motor_data_set(unsigned motor_number, uint16_t throttle, bool telemetry)
{
	uint16_t packet = 0;
	uint16_t checksum = 0;

	packet |= throttle << DSHOT_THROTTLE_POSITION;
	packet |= ((uint16_t)telemetry & 0x01) << DSHOT_TELEMETRY_POSITION;

	uint16_t csum_data = packet;

	/* XOR checksum calculation */
	csum_data >>= NIBBLES_SIZE;

	for (unsigned i = 0; i < DSHOT_NUMBER_OF_NIBBLES; i++) {
		checksum ^= (csum_data & 0x0F); // XOR data by nibbles
		csum_data >>= NIBBLES_SIZE;
	}

	if (enable_bidirectional_dshot) {
		packet |= ((~checksum) & 0x0F);

	} else {
		packet |= ((checksum) & 0x0F);
	}

	unsigned timer = timer_io_channels[motor_number].timer_index;
	uint32_t *buffer = dshot_burst_buffer[timer];
	const io_timers_channel_mapping_element_t *mapping = &io_timers_channel_mapping.element[timer];
	unsigned num_motors = mapping->channel_count_including_gaps;
	unsigned timer_channel_index = timer_io_channels[motor_number].timer_channel - mapping->lowest_timer_channel;

	for (unsigned motor_data_index = 0; motor_data_index < ONE_MOTOR_DATA_SIZE; motor_data_index++) {
		buffer[motor_data_index * num_motors + timer_channel_index] =
			(packet & 0x8000) ? MOTOR_PWM_BIT_1 : MOTOR_PWM_BIT_0;  // MSB first
		packet <<= 1;
	}
}

int up_dshot_arm(bool armed)
{
	return io_timer_set_enable(armed, enable_bidirectional_dshot ? IOTimerChanMode_DshotInverted : IOTimerChanMode_Dshot,
				   IO_TIMER_ALL_MODES_CHANNELS);
}

bool up_dshot_get_periods(uint32_t periods[], size_t num_periods)
{
	// TODO: hardcoded for now.
	if (num_periods != 4) {
		return false;
	}

	if (!_periods_ready) {
		return false;
	}

	for (unsigned i = 0; i < 4; ++i) {
		periods[i] = _periods[i];
	}

	_periods_ready = false;

	return true;
}

#endif
