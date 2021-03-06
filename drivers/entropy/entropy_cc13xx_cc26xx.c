/*
 * Copyright (c) 2019 Brett Witherspoon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_cc13xx_cc26xx_trng

#include <kernel.h>
#include <device.h>
#include <drivers/entropy.h>
#include <irq.h>
#include <sys/ring_buffer.h>
#include <sys/sys_io.h>

#include <driverlib/prcm.h>
#include <driverlib/trng.h>

#define CPU_FREQ DT_PROP(DT_PATH(cpus, cpu_0), clock_frequency)

#define US_PER_SAMPLE (1000000ULL * \
	CONFIG_ENTROPY_CC13XX_CC26XX_SAMPLES_PER_CYCLE / CPU_FREQ + 1ULL)

struct entropy_cc13xx_cc26xx_data {
	struct k_sem lock;
	struct k_sem sync;
	struct ring_buf pool;
	uint8_t data[CONFIG_ENTROPY_CC13XX_CC26XX_POOL_SIZE];
};

DEVICE_DECLARE(entropy_cc13xx_cc26xx);

static inline struct entropy_cc13xx_cc26xx_data *
get_dev_data(struct device *dev)
{
	return dev->driver_data;
}

static void handle_shutdown_ovf(void)
{
	uint32_t off;

	/* Clear shutdown */
	TRNGIntClear(TRNG_FRO_SHUTDOWN);
	/* Disabled FROs */
	off = sys_read32(TRNG_BASE + TRNG_O_ALARMSTOP);
	/* Clear alarms */
	sys_write32(0, TRNG_BASE + TRNG_O_ALARMMASK);
	sys_write32(0, TRNG_BASE + TRNG_O_ALARMSTOP);
	/* De-tune FROs */
	sys_write32(off, TRNG_BASE + TRNG_O_FRODETUNE);
	/* Re-enable FROs */
	sys_write32(off, TRNG_BASE + TRNG_O_FROEN);
}

static int entropy_cc13xx_cc26xx_get_entropy(struct device *dev, uint8_t *buf,
					     uint16_t len)
{
	struct entropy_cc13xx_cc26xx_data *data = get_dev_data(dev);
	uint32_t cnt;

	TRNGIntEnable(TRNG_NUMBER_READY);

	while (len) {
		k_sem_take(&data->lock, K_FOREVER);
		cnt = ring_buf_get(&data->pool, buf, len);
		k_sem_give(&data->lock);

		if (cnt) {
			buf += cnt;
			len -= cnt;
		} else {
			k_sem_take(&data->sync, K_FOREVER);
		}
	}

	return 0;
}

static void entropy_cc13xx_cc26xx_isr(void *arg)
{
	struct entropy_cc13xx_cc26xx_data *data = get_dev_data(arg);
	uint32_t src, cnt;
	uint32_t num[2];

	/* Interrupt service routine as described in TRM section 18.6.1.3.2 */
	src = TRNGStatusGet();

	if (src & TRNG_NUMBER_READY) {
		/* This function acknowledges the ready status */
		num[1] = TRNGNumberGet(TRNG_HI_WORD);
		num[0] = TRNGNumberGet(TRNG_LOW_WORD);

		cnt = ring_buf_put(&data->pool, (uint8_t *)num, sizeof(num));

		/* When pool is full disable interrupt and stop reading numbers */
		if (cnt != sizeof(num)) {
			TRNGIntDisable(TRNG_NUMBER_READY);
		}

		k_sem_give(&data->sync);
	}

	/* Change the shutdown FROs' oscillating frequncy in an attempt to
	 * prevent further locking on to the sampling clock frequncy.
	 */
	if (src & TRNG_FRO_SHUTDOWN) {
		handle_shutdown_ovf();
	}
}

static int entropy_cc13xx_cc26xx_get_entropy_isr(struct device *dev,
	uint8_t *buf, uint16_t len, uint32_t flags)
{
	struct entropy_cc13xx_cc26xx_data *data = get_dev_data(dev);
	uint16_t cnt;
	uint16_t read = len;
	uint32_t src;
	uint32_t num[2];
	unsigned int key;

	key = irq_lock();
	cnt = ring_buf_get(&data->pool, buf, len);
	irq_unlock(key);

	if ((cnt != len) && ((flags & ENTROPY_BUSYWAIT) != 0U)) {
		buf += cnt;
		len -= cnt;

		/* Allowed to busy-wait. We should use a polling approach */
		while (len) {
			key = irq_lock();

			src = TRNGStatusGet();
			if (src & TRNG_NUMBER_READY) {
				/*
				 * This function acknowledges the ready
				 * status
				 */
				num[1] = TRNGNumberGet(TRNG_HI_WORD);
				num[0] = TRNGNumberGet(TRNG_LOW_WORD);

				ring_buf_put(&data->pool, (uint8_t *)num,
					sizeof(num));
			}

			/*
			 * If interrupts were enabled during busy wait, this
			 * would allow us to pick up anything that has been put
			 * in by the ISR as well.
			 */
			cnt = ring_buf_get(&data->pool, buf, len);

			if (src & TRNG_FRO_SHUTDOWN) {
				handle_shutdown_ovf();
			}

			irq_unlock(key);

			if (cnt) {
				buf += cnt;
				len -= cnt;
			} else {
				k_busy_wait(US_PER_SAMPLE);
			}
		}

	} else {
		read = cnt;
	}

	return read;
}

static int entropy_cc13xx_cc26xx_init(struct device *dev)
{
	struct entropy_cc13xx_cc26xx_data *data = get_dev_data(dev);

	/* Power TRNG domain */
	PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

	/* Enable TRNG peripheral clocks */
	PRCMPeripheralRunEnable(PRCM_PERIPH_TRNG);
	/* Enabled the TRNG while in sleep mode to keep the entropy pool full. After
	 * the pool is full the TRNG will enter idle mode when random numbers are no
	 * longer being read. */
	PRCMPeripheralSleepEnable(PRCM_PERIPH_TRNG);
	PRCMPeripheralDeepSleepEnable(PRCM_PERIPH_TRNG);


	/* Load PRCM settings */
	PRCMLoadSet();
	while (!PRCMLoadGet()) {
		continue;
	}

	/* Peripherals should not be accessed until power domain is on. */
	while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) !=
	       PRCM_DOMAIN_POWER_ON) {
		continue;
	}

	/* Initialize driver data */
	ring_buf_init(&data->pool, sizeof(data->data), data->data);

	/* Initialization as described in TRM section 18.6.1.2 */
	TRNGReset();
	while (sys_read32(TRNG_BASE + TRNG_O_SWRESET)) {
		continue;
	}

	/* Set samples per cycle */
	TRNGConfigure(0, CONFIG_ENTROPY_CC13XX_CC26XX_SAMPLES_PER_CYCLE, 0);
	/* De-tune FROs */
	sys_write32(TRNG_FRODETUNE_FRO_MASK_M, TRNG_BASE + TRNG_O_FRODETUNE);
	/* Enable FROs */
	sys_write32(TRNG_FROEN_FRO_MASK_M, TRNG_BASE + TRNG_O_FROEN);
	/* Set shutdown and alarm thresholds */
	sys_write32((CONFIG_ENTROPY_CC13XX_CC26XX_SHUTDOWN_THRESHOLD << 16) |
			    CONFIG_ENTROPY_CC13XX_CC26XX_ALARM_THRESHOLD,
		    TRNG_BASE + TRNG_O_ALARMCNT);

	TRNGEnable();
	TRNGIntEnable(TRNG_NUMBER_READY | TRNG_FRO_SHUTDOWN);

	IRQ_CONNECT(DT_INST_IRQN(0),
		    DT_INST_IRQ(0, priority),
		    entropy_cc13xx_cc26xx_isr,
		    DEVICE_GET(entropy_cc13xx_cc26xx), 0);
	irq_enable(DT_INST_IRQN(0));

	return 0;
}

static const struct entropy_driver_api entropy_cc13xx_cc26xx_driver_api = {
	.get_entropy = entropy_cc13xx_cc26xx_get_entropy,
	.get_entropy_isr = entropy_cc13xx_cc26xx_get_entropy_isr,
};

static struct entropy_cc13xx_cc26xx_data entropy_cc13xx_cc26xx_data = {
	.lock = Z_SEM_INITIALIZER(entropy_cc13xx_cc26xx_data.lock, 1, 1),
	.sync = Z_SEM_INITIALIZER(entropy_cc13xx_cc26xx_data.sync, 0, 1),
};

DEVICE_AND_API_INIT(entropy_cc13xx_cc26xx, DT_INST_LABEL(0),
		    entropy_cc13xx_cc26xx_init, &entropy_cc13xx_cc26xx_data,
		    NULL, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &entropy_cc13xx_cc26xx_driver_api);
