/*
 * Copyright (c) 2017 Erwin Rol <erwin@erwinrol.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_rng

#include <kernel.h>
#include <device.h>
#include <drivers/entropy.h>
#include <random/rand32.h>
#include <init.h>
#include <sys/__assert.h>
#include <sys/util.h>
#include <errno.h>
#include <soc.h>
#include <sys/printk.h>
#include <drivers/clock_control.h>
#include <drivers/clock_control/stm32_clock_control.h>

struct entropy_stm32_rng_dev_cfg {
	struct stm32_pclken pclken;
};

struct entropy_stm32_rng_dev_data {
	RNG_TypeDef *rng;
	struct device *clock;
};

#define DEV_DATA(dev) \
	((struct entropy_stm32_rng_dev_data *)(dev)->data)

#define DEV_CFG(dev) \
	((const struct entropy_stm32_rng_dev_cfg *)(dev)->config)

static void entropy_stm32_rng_reset(RNG_TypeDef *rng)
{
	__ASSERT_NO_MSG(rng != NULL);

	/* Reset RNG as described in RM0090 Reference manual
	 * section 24.3.2 Error management.
	 */
	LL_RNG_ClearFlag_CEIS(rng);
	LL_RNG_ClearFlag_SEIS(rng);

	LL_RNG_Disable(rng);
	LL_RNG_Enable(rng);
}

static int entropy_stm32_got_error(RNG_TypeDef *rng)
{
	__ASSERT_NO_MSG(rng != NULL);

	if (LL_RNG_IsActiveFlag_CECS(rng)) {
		return 1;
	}

	if (LL_RNG_IsActiveFlag_SECS(rng)) {
		return 1;
	}

	return 0;
}

static int entropy_stm32_wait_ready(RNG_TypeDef *rng)
{
	/* Agording to the reference manual it takes 40 periods
	 * of the RNG_CLK clock signal between two consecutive
	 * random numbers. Also RNG_CLK may not be smaller than
	 * HCLK/16. So it should not take more than 640 HCLK
	 * ticks. Assuming the CPU can do 1 instruction per HCLK
	 * the number of times to loop before the RNG is ready
	 * is less than 1000. And that is when assumming the loop
	 * only takes 1 instruction. So looping a million times
	 * should be more than enough.
	 */

	int timeout = 1000000;

	__ASSERT_NO_MSG(rng != NULL);

	while (!LL_RNG_IsActiveFlag_DRDY(rng)) {
		if (entropy_stm32_got_error(rng)) {
			return -EIO;
		}

		if (timeout-- == 0) {
			return -ETIMEDOUT;
		}

		k_yield();
	}

	if (entropy_stm32_got_error(rng)) {
		return -EIO;
	} else {
		return 0;
	}
}

static int entropy_stm32_rng_get_entropy(struct device *dev, uint8_t *buffer,
					 uint16_t length)
{
	struct entropy_stm32_rng_dev_data *dev_data;
	int n = sizeof(uint32_t);
	int res;

	__ASSERT_NO_MSG(dev != NULL);
	__ASSERT_NO_MSG(buffer != NULL);

	dev_data = DEV_DATA(dev);

	__ASSERT_NO_MSG(dev_data != NULL);

	/* if the RNG has errors reset it before use */
	if (entropy_stm32_got_error(dev_data->rng)) {
		entropy_stm32_rng_reset(dev_data->rng);
	}

	while (length > 0) {
		uint32_t rndbits;
		uint8_t *p_rndbits = (uint8_t *)&rndbits;

		res = entropy_stm32_wait_ready(dev_data->rng);
		if (res < 0)
			return res;

		rndbits = LL_RNG_ReadRandData32(dev_data->rng);

		if (length < sizeof(uint32_t))
			n = length;

		for (int i = 0; i < n; i++) {
			*buffer++ = *p_rndbits++;
		}

		length -= n;
	}

	return 0;
}

static int entropy_stm32_rng_init(struct device *dev)
{
	struct entropy_stm32_rng_dev_data *dev_data;
	const struct entropy_stm32_rng_dev_cfg *dev_cfg;
	int res;

	__ASSERT_NO_MSG(dev != NULL);

	dev_data = DEV_DATA(dev);
	dev_cfg = DEV_CFG(dev);

	__ASSERT_NO_MSG(dev_data != NULL);
	__ASSERT_NO_MSG(dev_cfg != NULL);

#if CONFIG_SOC_SERIES_STM32L4X
	/* Configure PLLSA11 to enable 48M domain */
	LL_RCC_PLLSAI1_ConfigDomain_48M(LL_RCC_PLLSOURCE_MSI,
					LL_RCC_PLLM_DIV_1,
					24, LL_RCC_PLLSAI1Q_DIV_2);

	/* Enable PLLSA1 */
	LL_RCC_PLLSAI1_Enable();

	/*  Enable PLLSAI1 output mapped on 48MHz domain clock */
	LL_RCC_PLLSAI1_EnableDomain_48M();

	/* Wait for PLLSA1 ready flag */
	while (LL_RCC_PLLSAI1_IsReady() != 1) {
	}

	/*  Write the peripherals independent clock configuration register :
	 *  choose PLLSAI1 source as the 48 MHz clock is needed for the RNG
	 *  Linear Feedback Shift Register
	 */
	 LL_RCC_SetRNGClockSource(LL_RCC_RNG_CLKSOURCE_PLLSAI1);
#elif defined(RCC_HSI48_SUPPORT)

#if CONFIG_SOC_SERIES_STM32L0X
	/* We need SYSCFG to control VREFINT, so make sure it is clocked */
	if (!LL_APB2_GRP1_IsEnabledClock(LL_APB2_GRP1_PERIPH_SYSCFG)) {
		return -EINVAL;
	}
	/* HSI48 requires VREFINT (see RM0376 section 7.2.4). */
	LL_SYSCFG_VREFINT_EnableHSI48();
#endif /* CONFIG_SOC_SERIES_STM32L0X */

	/* Use the HSI48 for the RNG */
	LL_RCC_HSI48_Enable();
	while (!LL_RCC_HSI48_IsReady()) {
		/* Wait for HSI48 to become ready */
	}

	LL_RCC_SetRNGClockSource(LL_RCC_RNG_CLKSOURCE_HSI48);
#endif /* CONFIG_SOC_SERIES_STM32L4X */

	dev_data->clock = device_get_binding(STM32_CLOCK_CONTROL_NAME);
	__ASSERT_NO_MSG(dev_data->clock != NULL);

	res = clock_control_on(dev_data->clock,
		(clock_control_subsys_t *)&dev_cfg->pclken);
	__ASSERT_NO_MSG(res == 0);

	LL_RNG_Enable(dev_data->rng);

	return 0;
}

static const struct entropy_driver_api entropy_stm32_rng_api = {
	.get_entropy = entropy_stm32_rng_get_entropy
};

static const struct entropy_stm32_rng_dev_cfg entropy_stm32_rng_config = {
	.pclken	= { .bus = DT_INST_CLOCKS_CELL(0, bus),
		    .enr = DT_INST_CLOCKS_CELL(0, bits) },
};

static struct entropy_stm32_rng_dev_data entropy_stm32_rng_data = {
	.rng = (RNG_TypeDef *)DT_INST_REG_ADDR(0),
};

DEVICE_AND_API_INIT(entropy_stm32_rng, DT_INST_LABEL(0),
		    entropy_stm32_rng_init,
		    &entropy_stm32_rng_data, &entropy_stm32_rng_config,
		    PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &entropy_stm32_rng_api);
