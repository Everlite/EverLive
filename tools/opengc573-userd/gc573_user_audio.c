/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_audio.c — Userspace VFIO HDMI audio DMA for the GC573
 *
 * The FPGA HDMI audio engine uses two fixed host buffers.  Every completed
 * 20 ms period raises GC573_DMA_IRQ_AUDIO_COMPLETE and publishes the buffer
 * number in GC573_DMA_AUDIO_CHANNEL_STATUS.  Register values and ordering
 * mirror gc555_dma_start_audio()/gc555_dma_quiesce_audio_locked().
 */

#include "gc573_user_audio.h"

#define GC573_DMA_AUDIO_STREAM_CONTROL	0x00008
#define GC573_DMA_RESET			0x0000c
#define GC573_DMA_INTERRUPT_STATUS	0x00010
#define GC573_DMA_AUDIO_CHANNEL_STATUS	0x00014
#define GC573_DMA_INTERRUPT_ENABLE	0x0001c
#define GC573_DMA_AUDIO_FORMAT		0x0200
#define GC573_DMA_AUDIO_BUFFER_DWORDS	0x0204
#define GC573_DMA_AUDIO_BUFFER0_LOW	0x0208
#define GC573_DMA_AUDIO_BUFFER0_HIGH	0x020c
#define GC573_DMA_AUDIO_BUFFER1_LOW	0x0210
#define GC573_DMA_AUDIO_BUFFER1_HIGH	0x0214
#define GC573_DMA_AUDIO_BUFFER_CONTROL	0x0218
#define GC573_DMA_AUDIO_RATE		0x021c
#define GC573_DMA_AUDIO_MAP0		0x02b4
#define GC573_DMA_AUDIO_MAP1		0x02b8
#define GC573_DMA_AUDIO_MAP2		0x02bc
#define GC573_DMA_AUDIO_MAP3		0x02c0
#define GC573_BRIDGE_AUDIO_DIVISOR	0x010a0

#define GC573_DMA_IRQ_AUDIO_COMPLETE	BIT(5)
#define GC573_DMA_AUDIO_ENGINE_RESET	BIT(8)
#define GC573_DMA_AUDIO_STREAM_ENABLE	BIT(1)

#define GC573_AUDIO_CLOCK_HZ		100000000U
#define GC573_AUDIO_RATE_STEP_HZ	100U
#define GC573_AUDIO_RATE_ATTEMPTS	5U
#define GC573_AUDIO_PERIOD_UNITS	2U

static int gc573_user_audio_read_rate(void *bar0, uint32_t *rate_hz)
{
	u8 *base = (u8 *)bar0;

	for (unsigned int attempt = 0; attempt < GC573_AUDIO_RATE_ATTEMPTS;
	     attempt++) {
		uint32_t divisor = readl(base + GC573_BRIDGE_AUDIO_DIVISOR);
		uint32_t measured;
		uint32_t rounded;

		if (!divisor)
			continue;

		measured = GC573_AUDIO_CLOCK_HZ / divisor;
		rounded = ((measured + GC573_AUDIO_RATE_STEP_HZ / 2) /
			   GC573_AUDIO_RATE_STEP_HZ) * GC573_AUDIO_RATE_STEP_HZ;
		switch (rounded) {
		case 32000:
		case 44100:
		case 48000:
		case 96000:
		case 192000:
			*rate_hz = rounded;
			return 0;
		default:
			break;
		}
	}

	return -EAGAIN;
}

static bool gc573_user_audio_format_valid(unsigned int rate_hz,
					  unsigned int channels,
					  unsigned int sample_bits)
{
	bool rate_valid = rate_hz == 32000 || rate_hz == 44100 ||
			  rate_hz == 48000;
	bool channels_valid = channels == 2 || channels == 6 || channels == 8;

	return rate_valid && channels_valid &&
	       (sample_bits == 16 || sample_bits == 24);
}

static void gc573_user_audio_clear_interrupt(struct gc573_user_audio *audio)
{
	u8 *base = (u8 *)audio->bar0;
	uint32_t pending;

	pending = readl(base + GC573_DMA_INTERRUPT_STATUS);
	if (pending & GC573_DMA_IRQ_AUDIO_COMPLETE)
		writel(GC573_DMA_IRQ_AUDIO_COMPLETE,
		       base + GC573_DMA_INTERRUPT_STATUS);
}

int gc573_user_audio_init(struct gc573_user_audio *audio,
			  struct gc573_vfio_ctx *vfio, void *bar0)
{
	int ret;

	if (!audio || !vfio || !bar0)
		return -EINVAL;

	memset(audio, 0, sizeof(*audio));
	audio->vfio = vfio;
	audio->bar0 = bar0;
	audio->last_buffer = -1;
	audio->map_size = GC573_USER_AUDIO_MAP_SIZE;
	audio->iova = GC573_USER_AUDIO_IOVA;

	audio->vaddr = mmap(NULL, audio->map_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (audio->vaddr == MAP_FAILED) {
		audio->vaddr = NULL;
		return -errno;
	}

	memset(audio->vaddr, 0, audio->map_size);
	ret = gc573_vfio_map_dma(vfio, audio->vaddr, audio->map_size,
				 audio->iova);
	if (ret < 0) {
		munmap(audio->vaddr, audio->map_size);
		audio->vaddr = NULL;
		return ret;
	}
	audio->mapped = true;

	pr_info("Audio DMA: %u x %u KB ping-pong buffers mapped at IOVA 0x%llx",
		GC573_USER_AUDIO_BUFFER_COUNT,
		GC573_USER_AUDIO_BUFFER_CAPACITY / 1024,
		(unsigned long long)audio->iova);
	return 0;
}

int gc573_user_audio_start(struct gc573_user_audio *audio,
			   unsigned int rate_hz, unsigned int channels,
			   unsigned int sample_bits)
{
	u8 *base;
	unsigned int dma_channels;
	unsigned int sample_bytes;
	unsigned int buffer_bytes;
	uint32_t observed_rate = 0;
	uint32_t pending;
	int ret;

	if (!audio || !audio->bar0 || !audio->mapped)
		return -EINVAL;
	if (!gc573_user_audio_format_valid(rate_hz, channels, sample_bits))
		return -EINVAL;
	if (audio->streaming)
		return -EBUSY;

	/*
	 * 5.1 uses the eight FPGA slots just like the kernel driver.  Only
	 * the first two slots are played, so the DMA layout stays identical.
	 */
	dma_channels = channels == 6 ? 8 : channels;
	sample_bytes = sample_bits / 8U;
	buffer_bytes = rate_hz / 100U * GC573_AUDIO_PERIOD_UNITS *
		       dma_channels * sample_bytes;
	if (!buffer_bytes || (buffer_bytes & 3U) ||
	    buffer_bytes > GC573_USER_AUDIO_BUFFER_CAPACITY)
		return -EINVAL;

	ret = gc573_user_audio_read_rate(audio->bar0, &observed_rate);
	if (!ret && observed_rate != rate_hz)
		pr_warn("Audio DMA: observed FPGA rate %u Hz differs from requested %u Hz",
			observed_rate, rate_hz);

	base = (u8 *)audio->bar0;

	/* Same quiesce order as the kernel before (re)programming. */
	writel(readl(base + GC573_DMA_AUDIO_STREAM_CONTROL) &
	       ~GC573_DMA_AUDIO_STREAM_ENABLE,
	       base + GC573_DMA_AUDIO_STREAM_CONTROL);
	writel(readl(base + GC573_DMA_INTERRUPT_ENABLE) &
	       ~GC573_DMA_IRQ_AUDIO_COMPLETE,
	       base + GC573_DMA_INTERRUPT_ENABLE);
	gc573_user_audio_clear_interrupt(audio);

	memset(audio->vaddr, 0, audio->map_size);

	writel((dma_channels == 8 ? 2U : 0U) |
	       (sample_bits == 24 ? 1U : 0U),
	       base + GC573_DMA_AUDIO_FORMAT);
	writel(0, base + GC573_DMA_AUDIO_MAP0);
	writel(1, base + GC573_DMA_AUDIO_MAP1);
	writel(2, base + GC573_DMA_AUDIO_MAP2);
	writel(3, base + GC573_DMA_AUDIO_MAP3);
	writel(buffer_bytes / sizeof(uint32_t),
	       base + GC573_DMA_AUDIO_BUFFER_DWORDS);
	writel(rate_hz / 100U * GC573_AUDIO_PERIOD_UNITS,
	       base + GC573_DMA_AUDIO_RATE);
	writel((uint32_t)audio->iova, base + GC573_DMA_AUDIO_BUFFER0_LOW);
	writel((uint32_t)(audio->iova >> 32),
	       base + GC573_DMA_AUDIO_BUFFER0_HIGH);
	writel((uint32_t)(audio->iova + GC573_USER_AUDIO_BUFFER_CAPACITY),
	       base + GC573_DMA_AUDIO_BUFFER1_LOW);
	writel((uint32_t)((audio->iova + GC573_USER_AUDIO_BUFFER_CAPACITY) >> 32),
	       base + GC573_DMA_AUDIO_BUFFER1_HIGH);
	writel(0, base + GC573_DMA_AUDIO_BUFFER_CONTROL);

	/* Publish buffer contents before enabling the engine. */
	__sync_synchronize();

	pending = readl(base + GC573_DMA_INTERRUPT_ENABLE);
	writel(pending | GC573_DMA_IRQ_AUDIO_COMPLETE,
	       base + GC573_DMA_INTERRUPT_ENABLE);

	/*
	 * The kernel keeps the completion IRQ armed for 100 ms before it
	 * releases the stream.  Keep the same guard time.
	 */
	msleep(100);

	audio->rate_hz = rate_hz;
	audio->channels = channels;
	audio->dma_channels = dma_channels;
	audio->sample_bits = sample_bits;
	audio->sample_bytes = sample_bytes;
	audio->buffer_bytes = buffer_bytes;
	audio->frames_per_block = buffer_bytes /
				  (dma_channels * sample_bytes);
	audio->last_buffer = -1;
	audio->blocks = 0;
	audio->streaming = true;

	writel(readl(base + GC573_DMA_AUDIO_STREAM_CONTROL) |
	       GC573_DMA_AUDIO_STREAM_ENABLE,
	       base + GC573_DMA_AUDIO_STREAM_CONTROL);

	pr_info("Audio DMA: started %u Hz, %u ch (%u DMA slots), %u-bit, %u bytes/period",
		rate_hz, channels, dma_channels, sample_bits, buffer_bytes);
	return 0;
}

int gc573_user_audio_poll(struct gc573_user_audio *audio,
			  const void **data, size_t *bytes)
{
	u8 *base;
	uint32_t status;
	uint32_t channel_status;
	unsigned int code;
	int index;

	if (!audio || !audio->bar0 || !data || !bytes)
		return -EINVAL;
	if (!audio->streaming)
		return 0;

	base = (u8 *)audio->bar0;
	status = readl(base + GC573_DMA_INTERRUPT_STATUS);
	channel_status = readl(base + GC573_DMA_AUDIO_CHANNEL_STATUS);

	if (status & GC573_DMA_IRQ_AUDIO_COMPLETE)
		writel(GC573_DMA_IRQ_AUDIO_COMPLETE,
		       base + GC573_DMA_INTERRUPT_STATUS);

	/*
	 * The video path acknowledges the shared interrupt status register
	 * with a broad mask, so the audio bit may already be gone by the time
	 * we look.  The channel-status buffer number still identifies the
	 * completed period; ignore a repeated value unless a fresh audio IRQ
	 * was observed.
	 */
	code = channel_status & 0x3U;
	if (code < 1 || code > GC573_USER_AUDIO_BUFFER_COUNT)
		return 0;
	index = (int)code - 1;
	if (!(status & GC573_DMA_IRQ_AUDIO_COMPLETE) &&
	    index == audio->last_buffer)
		return 0;

	__sync_synchronize();

	audio->last_buffer = index;
	audio->blocks++;
	*data = (uint8_t *)audio->vaddr +
		(size_t)index * GC573_USER_AUDIO_BUFFER_CAPACITY;
	*bytes = audio->buffer_bytes;
	return 1;
}

void gc573_user_audio_stop(struct gc573_user_audio *audio)
{
	u8 *base;
	uint32_t status = 0;
	bool reset_seen = false;

	if (!audio || !audio->bar0)
		return;

	base = (u8 *)audio->bar0;
	audio->streaming = false;

	writel(readl(base + GC573_DMA_AUDIO_STREAM_CONTROL) &
	       ~GC573_DMA_AUDIO_STREAM_ENABLE,
	       base + GC573_DMA_AUDIO_STREAM_CONTROL);
	writel(readl(base + GC573_DMA_INTERRUPT_ENABLE) &
	       ~GC573_DMA_IRQ_AUDIO_COMPLETE,
	       base + GC573_DMA_INTERRUPT_ENABLE);
	gc573_user_audio_clear_interrupt(audio);

	/* Mirror gc555_dma_quiesce_audio_locked(): reset the audio engine. */
	writel(GC573_DMA_AUDIO_ENGINE_RESET, base + GC573_DMA_RESET);
	for (unsigned int attempt = 0; attempt < 11; attempt++) {
		usleep_range(1000, 2000);
		status = readl(base + GC573_DMA_RESET);
		if (status & GC573_DMA_AUDIO_ENGINE_RESET) {
			reset_seen = true;
			break;
		}
	}
	msleep(30);
	if (!reset_seen)
		pr_debug("Audio DMA: reset bit not observed, status 0x%08x",
			 status);

	audio->last_buffer = -1;
}

void gc573_user_audio_cleanup(struct gc573_user_audio *audio)
{
	if (!audio)
		return;

	gc573_user_audio_stop(audio);
	if (audio->vaddr) {
		if (audio->mapped)
			gc573_vfio_unmap_dma(audio->vfio, audio->iova,
					     audio->map_size);
		munmap(audio->vaddr, audio->map_size);
		audio->vaddr = NULL;
		audio->mapped = false;
	}
}
