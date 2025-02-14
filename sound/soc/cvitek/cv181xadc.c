// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CVITEK CV181X ADC driver
 *
 * Copyright 2020 CVITEK Inc.
 *
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/pm.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/control.h>
#include "cv1835_ioctl.h"
#include "../codecs/cv181xadac.h"
#include "cv1835_i2s_subsys.h"

static DEFINE_MUTEX(cv181xadc_mutex);

static int adc_vol_list[25] = {
	ADC_VOL_GAIN_0,
	ADC_VOL_GAIN_1,
	ADC_VOL_GAIN_2,
	ADC_VOL_GAIN_3,
	ADC_VOL_GAIN_4,
	ADC_VOL_GAIN_5,
	ADC_VOL_GAIN_6,
	ADC_VOL_GAIN_7,
	ADC_VOL_GAIN_8,
	ADC_VOL_GAIN_9,
	ADC_VOL_GAIN_10,
	ADC_VOL_GAIN_11,
	ADC_VOL_GAIN_12,
	ADC_VOL_GAIN_13,
	ADC_VOL_GAIN_14,
	ADC_VOL_GAIN_15,
	ADC_VOL_GAIN_16,
	ADC_VOL_GAIN_17,
	ADC_VOL_GAIN_18,
	ADC_VOL_GAIN_19,
	ADC_VOL_GAIN_20,
	ADC_VOL_GAIN_21,
	ADC_VOL_GAIN_22,
	ADC_VOL_GAIN_23,
	ADC_VOL_GAIN_24
};

u32 old_adc_voll;
u32 old_adc_volr;

static int adc_open(struct inode *inode, struct file *file)
{
	if (mutex_lock_interruptible(&cv181xadc_mutex))
		return -EINTR;
	mutex_unlock(&cv181xadc_mutex);
	pr_debug("%s\n", __func__);
	return 0;
}

static int adc_close(struct inode *inode, struct file *file)
{
	if (mutex_lock_interruptible(&cv181xadc_mutex))
		return -EINTR;
	mutex_unlock(&cv181xadc_mutex);
	pr_debug("%s\n", __func__);
	return 0;
}

static inline void adc_write_reg(void __iomem *io_base, int reg, u32 val)
{
	writel(val, io_base + reg);
}

static inline u32 adc_read_reg(void __iomem *io_base, int reg)
{
	return readl(io_base + reg);
}

static void cv181xadc_clk_on(struct cv181xadc *adc)
{

	u32 clk_ctrl0;

	mutex_lock(&cv181xadc_mutex);

	clk_ctrl0 = readl(adc->mclk_source + CVI_I2S_CLK_CTRL0);

	if (!(clk_ctrl0 & CVI_I2S_AU_EN_MASK)) {
		dev_info(adc->dev, "turn I2S3 aud_en on\n");
		clk_ctrl0 |= CVI_I2S_AU_EN;
	}

	if (!(clk_ctrl0 & CVI_I2S_MCLK_OUT_EN_MASK)) {
		dev_info(adc->dev, "turn I2S3 mclk_out_en on\n");
		clk_ctrl0 |= CVI_I2S_MCLK_OUT_EN;
	}

	writel(clk_ctrl0, adc->mclk_source + CVI_I2S_CLK_CTRL0);
	dev_info(adc->dev, "adc_clk_on, I2S3 clk_ctrl0 = 0x%x\n", readl(adc->mclk_source + CVI_I2S_CLK_CTRL0));

	mutex_unlock(&cv181xadc_mutex);
}

static void cv181xadc_set_mclk(struct cv181xadc *adc, u32 rate)
{

	u32 clk_ctrl1, audio_clk;

	mutex_lock(&cv181xadc_mutex);

	clk_ctrl1 = readl(adc->mclk_source + CVI_I2S_CLK_CTRL1) & ~CVI_I2S_MCLK_MASK;

	dev_dbg(adc->dev, "adc_set_mclk, I2S3 ctrl1=0x%x\n", clk_ctrl1);

	switch (rate) {
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		audio_clk = CVI_22579_MHZ;
		break;
	case 8000:
	case 16000:
	case 32000:
		audio_clk = CVI_16384_MHZ;
		break;
	case 12000:
	case 24000:
	case 48000:
	case 96000:
	case 192000:
		audio_clk = CVI_24576_MHZ;
		break;
	default:
		dev_err(adc->dev, "Warning!!! this sample rate is not supported\n");
		return;
	}

	dev_dbg(adc->dev, "Audio system clk=%d, sample rate=%d\n", audio_clk, rate);
	cv1835_set_mclk(audio_clk);

	/* cv182xa internal adc codec need dynamic MCLK frequency input */
	switch (rate) {
	case 8000:
	case 16000:
	case 32000:
		clk_ctrl1 |= CVI_I2S_MCLK_DIV(1);
		break;
	case 11025:
	case 22050:
	case 44100:
	case 48000:
		clk_ctrl1 |= CVI_I2S_MCLK_DIV(2);
		break;
	default:
		dev_err(adc->dev, "adc_set_mclk doesn't support this sample rate\n");
		break;
	}
	writel(clk_ctrl1, adc->mclk_source + CVI_I2S_CLK_CTRL1);
	dev_dbg(adc->dev, "adc_set_mclk I2S3 clk_ctrl1 = 0x%x\n", readl(adc->mclk_source + CVI_I2S_CLK_CTRL1));
	mutex_unlock(&cv181xadc_mutex);
}

static void cv181xadc_clk_off(struct cv181xadc *adc)
{
	u32 i2s_en;
	u32 clk_ctrl0;

	mutex_lock(&cv181xadc_mutex);

	i2s_en = readl(adc->mclk_source + CVI_I2S_EN);
	clk_ctrl0 = readl(adc->mclk_source + CVI_I2S_CLK_CTRL0);

	if (!i2s_en) {
		if ((clk_ctrl0 & CVI_I2S_AU_EN_MASK)) {
			dev_info(adc->dev, "turn I2S3 aud_en off\n");
			clk_ctrl0 &= CVI_I2S_AU_OFF;
		}
	}

	if (!(clk_ctrl0 & CVI_I2S_MCLK_OUT_EN_MASK)) {
		dev_info(adc->dev, "turn I2S3 mclk_out_en on\n");
		clk_ctrl0 &= CVI_I2S_MCLK_OUT_OFF;
	}

	writel(clk_ctrl0, adc->mclk_source + CVI_I2S_CLK_CTRL0);
	dev_info(adc->dev, "adc_clk_off, I2S3 clk_ctrl0 = 0x%x\n", readl(adc->mclk_source + CVI_I2S_CLK_CTRL0));

	mutex_unlock(&cv181xadc_mutex);
}

static int cv181xadc_set_dai_fmt(struct snd_soc_dai *dai,
					unsigned int fmt)
{

	struct cv181xadc *adc = snd_soc_dai_get_drvdata(dai);

	if (!adc->dev)
		dev_err(adc->dev, "dev is NULL\n");

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		dev_dbg(adc->dev, "Set ADC to MASTER mode\n");
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		dev_err(adc->dev, "Cannot set DAC to SLAVE mode, only support MASTER mode\n");
		break;
	default:
		dev_err(adc->dev, "Cannot support this role mode\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_IF:
		dev_dbg(adc->dev, "set codec to NB_IF\n");
		break;
	case SND_SOC_DAIFMT_IB_NF:
		dev_dbg(adc->dev, "set codec to IB_NF\n");
		break;
	case SND_SOC_DAIFMT_IB_IF:
		dev_dbg(adc->dev, "set codec to IB_IF\n");
		break;
	case SND_SOC_DAIFMT_NB_NF:
		dev_dbg(adc->dev, "set codec to NB_NF\n");
		break;
	default:
		dev_err(adc->dev, "Cannot support this format\n");
		break;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		dev_dbg(adc->dev, "set codec to I2S mode\n");
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		dev_dbg(adc->dev, "set codec to LEFT-JUSTIFY mode\n");
		break;
	default:
		dev_err(adc->dev, "Cannot support this mode\n");
		break;
	}
	return 0;
}

static int cv181xadc_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct cv181xadc *adc = snd_soc_dai_get_drvdata(dai);
	int rate;
	u32 ctrl1 = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL1) & ~AUDIO_PHY_REG_RXADC_CIC_OPT_MASK;
	u32 ana3 = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA3) & ~AUDIO_PHY_REG_CTUNE_RXADC_MASK;
	u32 ana0;
	u32 clk = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CLK) &
			~(AUDIO_RXADC_SCK_DIV_MASK | AUDIO_RXADC_DLYEN_MASK);
	void __iomem *dac;

	/* ECO function, register naming is not corrected, use ioremap to access register of DAC */
	dac = ioremap(0x0300A000, 0x30);
	ana0 = readl(dac + AUDIO_PHY_TXDAC_ANA0) & ~AUDIO_PHY_REG_ADDI_TXDAC_MASK;

	rate = params_rate(params);
	if (rate >= 8000 && rate <= 48000) {
		dev_info(adc->dev, "adc_hw_params, set rate to %d\n", rate);
		cv181xadc_set_mclk(adc, rate);

		switch (rate) {
		case 8000:
			ctrl1 |= RXADC_CIC_DS_512;
			ana3 |= RXADC_CTUNE_MCLK_16384;
			ana0 &= ADDI_TXDAC_GAIN_RATIO_1;
			clk |= RXADC_SCK_DIV(32) | RXADC_DLYEN(0x21); /* 16384 / 8 / 32 / 2 */
			break;
		case 11025:
			ctrl1 |= RXADC_CIC_DS_256;
			ana3 |= RXADC_CTUNE_MCLK_11298;
			ana0 &= ADDI_TXDAC_GAIN_RATIO_1;
			clk |= RXADC_SCK_DIV(16) | RXADC_DLYEN(0x17); /* 112896 / 11.025 / 32 / 2 */
			break;
		case 16000:
			ctrl1 |= RXADC_CIC_DS_256;
			ana3 |= RXADC_CTUNE_MCLK_16384;
			ana0 &= ADDI_TXDAC_GAIN_RATIO_1;
			clk |= RXADC_SCK_DIV(16) | RXADC_DLYEN(0x21); /* 16384 / 16 / 32 / 2 */
			break;
		case 22050:
			ctrl1 |= RXADC_CIC_DS_128;
			ana3 |= RXADC_CTUNE_MCLK_11298;
			ana0 &= ADDI_TXDAC_GAIN_RATIO_1;
			clk |= RXADC_SCK_DIV(8) | RXADC_DLYEN(0x17); /* 112896 / 22.05 / 32 / 2 */
			break;
		case 32000:
			ctrl1 |= RXADC_CIC_DS_128;
			ana3 |= RXADC_CTUNE_MCLK_16384;
			ana0 &= ADDI_TXDAC_GAIN_RATIO_1;
			clk |= RXADC_SCK_DIV(8) | RXADC_DLYEN(0x21); /* 16384 / 32 / 32 / 2 */
			break;
		case 44100:
			ctrl1 &= RXADC_CIC_DS_64;
			ana3 |= RXADC_CTUNE_MCLK_11298;
			ana0 &= ADDI_TXDAC_GAIN_RATIO_1;
			clk |= RXADC_SCK_DIV(4) | RXADC_DLYEN(0x17); /* 112896 / 44.1 / 32 / 2 */
			break;
		case 48000:
			ctrl1 &= RXADC_CIC_DS_64;
			ana3 |= RXADC_CTUNE_MCLK_12288;
			ana0 &= ADDI_TXDAC_GAIN_RATIO_1;
			clk |= RXADC_SCK_DIV(4) | RXADC_DLYEN(0x19); /* 16384 / 16 / 32 / 2 */
			break;
		default:
			ctrl1 |= RXADC_CIC_DS_256;
			ana3 |= RXADC_CTUNE_MCLK_16384;
			ana0 &= ADDI_TXDAC_GAIN_RATIO_1;
			clk |= RXADC_SCK_DIV(16) | RXADC_DLYEN(0x21); /* 16384 / 16 / 32 / 2 */
			dev_dbg(adc->dev, "adc_hw_params, unsupported sample rate. Set with default 16KHz\n");
			break;
		}

		adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL1, ctrl1);
		adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA3, ana3);
		adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CLK, clk);
		writel(ana0, dac + AUDIO_PHY_TXDAC_ANA0);

		iounmap(dac);
	} else {
		dev_err(adc->dev, "adc_hw_params, unsupported sample rate\n");
		return 0;
	}

	if (params_width(params) != 16) {
		dev_err(adc->dev, "Only support I2S channel width with 16 bits\n");
		dev_err(adc->dev, "Set I2S channel width with 16bits\n");
		return 0;
	}
	return 0;
}

static int cv181xadc_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct cv181xadc *adc = snd_soc_dai_get_drvdata(dai);

	dev_dbg(adc->dev, "adc_startup\n");
	cv181xadc_clk_on(adc);

	return 0;
}

static void cv181xadc_on(struct cv181xadc *adc)
{

	u32 val = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);

	dev_info(adc->dev, "adc_on, before rxadc reg val=0x%08x\n",
	adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0));

	if ((val & AUDIO_PHY_REG_RXADC_EN_ON) | (val & AUDIO_PHY_REG_I2S_TX_EN_ON))
		dev_info(adc->dev, "ADC or I2S TX already switched ON!!, val=0x%08x\n", val);

	val |= AUDIO_PHY_REG_RXADC_EN_ON | AUDIO_PHY_REG_I2S_TX_EN_ON;
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, val);

	dev_info(adc->dev, "adc_on, after rxadc reg val=0x%08x\n",
	adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0));

}

static void cv181xadc_off(struct cv181xadc *adc)
{

	u32 val = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);

	val &= AUDIO_PHY_REG_RXADC_EN_OFF & AUDIO_PHY_REG_I2S_TX_EN_OFF;
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, val);

	dev_dbg(adc->dev, "adc_off, after rxadc reg val=0x%08x\n",
	adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0));

}

static void cv181xadc_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct cv181xadc *adc = snd_soc_dai_get_drvdata(dai);

	dev_dbg(adc->dev, "adc_shutdown\n");
	cv181xadc_off(adc);
	cv182xa_reset_adc();
	cv181xadc_clk_off(adc);
}

static int cv181xadc_trigger(struct snd_pcm_substream *substream,
		int cmd, struct snd_soc_dai *dai)
{
	struct cv181xadc *adc = snd_soc_dai_get_drvdata(dai);
	int ret = 0;

	dev_dbg(adc->dev, "adc_trigger, cmd=%d\n", cmd);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		//cv182xaadc_on(adc);//move to prepare function to meet adc on(clock out) before i2s reset
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		//cv182xaadc_off(adc);//move to shutdown function as adc on move to prepare
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int cv181xadc_prepare(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct cv181xadc *adc = snd_soc_dai_get_drvdata(dai);
	u32 val;

	//need to rewrite the register if called cv182xa_reset_adc
	val = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2);
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, val);
	val = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0);
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0, val);
	cv181xadc_on(adc);
#ifdef CONFIG_CVI_ADC_OV_MOD
	// chang overflow mode to bypass
	val = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL1);
	val &= ~AUDIO_PHY_REG_RXADC_DCB_OPT_MASK;
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL1, val);
#endif
	return 0;
}


static struct cv181xadc *file_adc_dev(struct file *file)
{
	return container_of(file->private_data, struct cv181xadc, miscdev);
}

static void adc_set_volume(struct cv181xadc *adc, u32 val) {
	u32 temp;
	u32 val2;

	pr_info("adc: set volume %d\n", val);
	pr_debug("adc: ACODEC_SET_INPUT_VOL\n");
	if ((val < 0) | (val > 24))
		pr_err("Only support range 0 [0dB] ~ 24 [48dB]\n");
	else if (val == 0) {
		/* set mute */
		temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2)
			| AUDIO_PHY_REG_MUTEL_ON
			| AUDIO_PHY_REG_MUTER_ON;
		adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, temp);
		temp = (adc_vol_list[val] | (adc_vol_list[val] << 16));
		adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0, temp);
	} else {
		val2 = (adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & AUDIO_PHY_REG_ADC_VOLL_MASK);
		for (temp = 0; temp < 25; temp++) {
			if (val2 == adc_vol_list[temp])
				break;
		}
		if (temp == 0) {
			/* unmute */
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2)
				& AUDIO_PHY_REG_MUTEL_OFF
				& AUDIO_PHY_REG_MUTEL_OFF;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, temp);
		}
		temp = (adc_vol_list[val] | (adc_vol_list[val] << 16));
		adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0, temp);
	}
};

static long adc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{

	unsigned int __user *argp = (unsigned int __user *)arg;
	struct cv181xadc *adc = file_adc_dev(file);
	struct cvi_vol_ctrl vol;
	u32 val, val2;
	u32 temp;

	if (argp != NULL) {
		if (!copy_from_user(&val, argp, sizeof(val))) {
			if (mutex_lock_interruptible(&cv181xadc_mutex)) {
				pr_debug("cvitekaadc: signal arrives while waiting for lock\n");
				return -EINTR;
			}
		} else
			return -EFAULT;
	}

	pr_debug("%s, received cmd=%u, val=%d\n", __func__, cmd, val);

	switch (cmd) {
	case ACODEC_SOFT_RESET_CTRL:
		cv182xa_reset_adc();
		break;

	case ACODEC_SET_INPUT_VOL:
		adc_set_volume(adc, val);
		break;

	case ACODEC_GET_INPUT_VOL:
		pr_debug("adc: ACODEC_GET_INPUT_VOL\n");
		val = (adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & AUDIO_PHY_REG_ADC_VOLL_MASK);
		for (temp = 0; temp < 25; temp++) {
			if (val == adc_vol_list[temp])
				break;
		}
		if (temp == 25)
			pr_info("adc: cannot find, out of range\n");

		if (copy_to_user(argp, &temp, sizeof(temp)))
			pr_err("adc, failed to return input vol\n");
		break;

	case ACODEC_SET_I2S1_FS:
		pr_info("adc: ACODEC_SET_I2S1_FS is not support\n");
		break;

	case ACODEC_SET_MIXER_MIC:
		pr_info("ACODEC_SET_MIXER_MIC is not support\n");
		break;
	case ACODEC_SET_GAIN_MICL:
		pr_debug("adc: ACODEC_SET_GAIN_MICL\n");
		if ((val < 0) | (val > 24))
			pr_err("Only support range 0 [0dB] ~ 24 [48dB]\n");
		else {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & ~AUDIO_PHY_REG_ADC_VOLL_MASK;
			temp |= adc_vol_list[val];
			old_adc_voll = val;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0, temp);
		}
		break;
	case ACODEC_SET_GAIN_MICR:
		pr_debug("adc: ACODEC_SET_GAIN_MICR\n");
		if ((val < 0) | (val > 24))
			pr_err("Only support range 0 [0dB] ~ 24 [48dB]\n");
		else {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & ~AUDIO_PHY_REG_ADC_VOLR_MASK;
			temp |= (adc_vol_list[val] << 16);
			old_adc_volr = val;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0, temp);
		}
		break;

	case ACODEC_SET_ADCL_VOL:

		if (copy_from_user(&vol, argp, sizeof(vol))) {
			if (mutex_is_locked(&cv181xadc_mutex))
				mutex_unlock(&cv181xadc_mutex);

			return -EFAULT;
		}

		pr_info("adc: ACODEC_SET_ADCL_VOL to %d, mute=%d\n", vol.vol_ctrl, vol.vol_ctrl_mute);

		if (vol.vol_ctrl_mute == 1) {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) | AUDIO_PHY_REG_MUTEL_ON;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, temp);
		} else if ((vol.vol_ctrl < 0) | (vol.vol_ctrl > 24))
			pr_err("adc-L: Only support range 0 [0dB] ~ 24 [48dB]\n");
		else {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & ~AUDIO_PHY_REG_ADC_VOLL_MASK;
			temp |= adc_vol_list[val];
			old_adc_voll = val;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0, temp);

			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) & AUDIO_PHY_REG_MUTEL_OFF;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, temp);
		}

		break;

	case ACODEC_SET_ADCR_VOL:
		if (copy_from_user(&vol, argp, sizeof(vol))) {
			if (mutex_is_locked(&cv181xadc_mutex))
				mutex_unlock(&cv181xadc_mutex);

			return -EFAULT;
		}

		pr_debug("adc: ACODEC_SET_ADCR_VOL to %d, mute=%d\n", vol.vol_ctrl, vol.vol_ctrl_mute);

		if (vol.vol_ctrl_mute == 1) {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) | AUDIO_PHY_REG_MUTER_ON;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, temp);
		} else if ((vol.vol_ctrl < 0) | (vol.vol_ctrl > 24))
			pr_err("adc-R: Only support range 0 [0dB] ~ 24 [48dB]\n");
		else {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & ~AUDIO_PHY_REG_ADC_VOLR_MASK;
			temp |= (adc_vol_list[val] << 16);
			old_adc_volr = val;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0, temp);

			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) & AUDIO_PHY_REG_MUTER_OFF;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, temp);
		}
		break;
	case ACODEC_SET_MICL_MUTE:
		pr_debug("adc: ACODEC_SET_MICL_MUTE\n");
		if (val == 0)
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) & AUDIO_PHY_REG_MUTEL_OFF;
		else
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) | AUDIO_PHY_REG_MUTEL_ON;

		adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, temp);
		break;
	case ACODEC_SET_MICR_MUTE:
		pr_debug("adc: ACODEC_SET_MICR_MUTE\n");
		if (val == 0)
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) & AUDIO_PHY_REG_MUTER_OFF;
		else
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) | AUDIO_PHY_REG_MUTER_ON;

		adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, temp);
		break;

	case ACODEC_GET_GAIN_MICL:
		pr_debug("adc: ACODEC_GET_GAIN_MICL\n");
		val = (adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & AUDIO_PHY_REG_ADC_VOLL_MASK);

		for (temp = 0; temp < 25; temp++) {
			if (val == adc_vol_list[temp])
				break;
		}

		if (copy_to_user(argp, &temp, sizeof(temp)))
			pr_err("failed to return MICL gain\n");
		break;
	case ACODEC_GET_GAIN_MICR:
		pr_debug("adc: ACODEC_GET_GAIN_MICR\n");
		val = (adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & AUDIO_PHY_REG_ADC_VOLR_MASK) >> 16;

		for (temp = 0; temp < 25; temp++) {
			if (val == adc_vol_list[temp])
				break;
		}
		if (copy_to_user(argp, &temp, sizeof(temp)))
			pr_err("failed to return MICR gain\n");
		break;

	case ACODEC_GET_ADCL_VOL:
		pr_debug("adc: ACODEC_GET_ADCL_VOL\n");

		val = (adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & AUDIO_PHY_REG_ADC_VOLL_MASK);
		for (temp = 0; temp < 25; temp++) {
			if (val == adc_vol_list[temp])
				break;
		}
		vol.vol_ctrl = temp;
		vol.vol_ctrl_mute = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) & AUDIO_PHY_REG_MUTEL_RXPGA_MASK;

		if (copy_to_user(argp, &vol, sizeof(vol)))
			pr_err("failed to return ADCL vol\n");

		break;
	case ACODEC_GET_ADCR_VOL:
		pr_debug("adc: ACODEC_GET_ADCR_VOL\n");

		val = (adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0) & AUDIO_PHY_REG_ADC_VOLR_MASK) >> 16;
		for (temp = 0; temp < 25; temp++) {
			if (val == adc_vol_list[temp])
				break;
		}
		vol.vol_ctrl = temp;
		vol.vol_ctrl_mute = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2) & AUDIO_PHY_REG_MUTER_RXPGA_MASK;

		if (copy_to_user(argp, &vol, sizeof(vol)))
			pr_err("failed to return ADCR vol\n");

		break;

	case ACODEC_SET_PD_ADCL:
		pr_debug("adc: ACODEC_SET_PD_ADCL, val=%d\n", val);
		if (val == 0) {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
			temp |= AUDIO_PHY_REG_RXADC_EN_ON | AUDIO_PHY_REG_I2S_TX_EN_ON;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, temp);
		} else {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
			temp &= AUDIO_PHY_REG_RXADC_EN_OFF & AUDIO_PHY_REG_I2S_TX_EN_OFF;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, temp);
		}
		break;
	case ACODEC_SET_PD_ADCR:
		pr_debug("adc: ACODEC_SET_PD_ADCR, val=%d\n", val);
		if (val == 0) {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
			temp |= AUDIO_PHY_REG_RXADC_EN_ON | AUDIO_PHY_REG_I2S_TX_EN_ON;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, temp);
		} else {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
			temp &= AUDIO_PHY_REG_RXADC_EN_OFF & AUDIO_PHY_REG_I2S_TX_EN_OFF;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, temp);
		}
		break;

	case ACODEC_SET_PD_LINEINL:
		pr_debug("adc: ACODEC_SET_PD_LINEINL, val=%d\n", val);
		if (val == 0) {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
			temp |= AUDIO_PHY_REG_RXADC_EN_ON | AUDIO_PHY_REG_I2S_TX_EN_ON;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, temp);
		} else {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
			temp &= AUDIO_PHY_REG_RXADC_EN_OFF & AUDIO_PHY_REG_I2S_TX_EN_OFF;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, temp);
		}
		break;
	case ACODEC_SET_PD_LINEINR:
		pr_debug("adc: ACODEC_SET_PD_LINEINR, val=%d\n", val);
		if (val == 0) {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
			temp |= AUDIO_PHY_REG_RXADC_EN_ON | AUDIO_PHY_REG_I2S_TX_EN_ON;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, temp);
		} else {
			temp = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
			temp &= AUDIO_PHY_REG_RXADC_EN_OFF & AUDIO_PHY_REG_I2S_TX_EN_OFF;
			adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, temp);
		}
		break;
	case ACODEC_SET_ADC_HP_FILTER:
		pr_info("adc: ACODEC_SET_ADC_HP_FILTER is not support\n");
		break;
	default:
		pr_info("%s, received unsupport cmd=%u\n", __func__, cmd);
		break;
	}

	if (mutex_is_locked(&cv181xadc_mutex))
		mutex_unlock(&cv181xadc_mutex);

	return 0;
}

static const struct snd_soc_dai_ops cv181xadc_dai_ops = {
	.hw_params	= cv181xadc_hw_params,
	.set_fmt	= cv181xadc_set_dai_fmt,
	.startup	= cv181xadc_startup,
	.shutdown	= cv181xadc_shutdown,
	.trigger	= cv181xadc_trigger,
	.prepare	= cv181xadc_prepare,
};

static struct snd_soc_dai_driver cv181xadc_dai = {
	.name		= "cvitekaadc",
	.capture	= {
		.stream_name	= "Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops		= &cv181xadc_dai_ops,
};

static const struct snd_kcontrol_new cv181xadc_controls[] = {
	SOC_DOUBLE("ADC Power", AUDIO_PHY_RXADC_CTRL0, 0, 1, 1, 0),
	SOC_DOUBLE("ADC Capture Volume", AUDIO_PHY_RXADC_ANA0, 0, 16, 24, 0),
	SOC_DOUBLE("ADC Capture Mute", AUDIO_PHY_RXADC_ANA2, 0, 1, 1, 0),
};

unsigned int cv181xadc_reg_read(struct snd_soc_component *codec, unsigned int reg)
{
	int ret, lidx, ridx;
	struct cv181xadc *adc = dev_get_drvdata(codec->dev);

	ret = adc_read_reg(adc->adc_base, reg);
	if (reg == AUDIO_PHY_RXADC_ANA0) {
		for (lidx = 0; lidx < 25; lidx++)
			if ((ret & 0xffff) == adc_vol_list[lidx])
				break;
		for (ridx = 0; ridx < 25; ridx++)
			if (((ret>>16) & 0xffff) == adc_vol_list[ridx])
				break;
		dev_info(adc->dev, "ADC get Vol, reg:%d,ret:%#x, idx=%d.\n", reg, ret, lidx);
		ret = (lidx << 16) | ridx;
	}

	dev_dbg(adc->dev, "adc_reg_read reg:%d,ret:%#x.\n", reg, ret);

	return ret;
}

int cv181xadc_reg_write(struct snd_soc_component *codec, unsigned int reg, unsigned int value)
{
	struct cv181xadc *adc = dev_get_drvdata(codec->dev);
	u32 temp_lval;
	u32 temp_rval;

	if (reg == AUDIO_PHY_RXADC_ANA0) {
		temp_lval = value & 0xffff;
		temp_rval = (value >> 16) & 0xffff;
		if (temp_lval > 24)
			temp_lval = 24;
		if (temp_rval > 24)
			temp_rval = 24;
		value = (adc_vol_list[temp_rval]<<16) | adc_vol_list[temp_lval];
		dev_info(adc->dev, "Set ADC Vol, get input val=%d, output val=0x%x\n", value, temp_lval);
	}


	adc_write_reg(adc->adc_base, reg, value);
	dev_dbg(adc->dev, "adc_reg_write reg:%d,value:%#x.\n", reg, value);

	return 0;
}

static const struct snd_soc_component_driver soc_component_dev_cv181xadc = {
	.controls = cv181xadc_controls,
	.num_controls = ARRAY_SIZE(cv181xadc_controls),
	.read = cv181xadc_reg_read,
	.write = cv181xadc_reg_write,
};

static const struct file_operations adc_fops = {
	.owner = THIS_MODULE,
	.open = adc_open,
	.release = adc_close,
	.unlocked_ioctl = adc_ioctl,
	.compat_ioctl = adc_ioctl,
};

static int adc_device_register(struct cv181xadc *adc)
{
	struct miscdevice *miscdev = &adc->miscdev;
	int ret;

	miscdev->minor = MISC_DYNAMIC_MINOR;
	miscdev->name = "cvitekaadc";
	miscdev->fops = &adc_fops;
	miscdev->parent = NULL;

	ret = misc_register(miscdev);
	if (ret) {
		pr_err("adc: failed to register misc device.\n");
		return ret;
	}

	return 0;
}

static int cv181xadc_probe(struct platform_device *pdev)
{
	struct cv181xadc *adc;
	struct resource *res;
	u32 mclk_source_addr = 0x0;
	u32 ctrl1;
	int ret;

	dev_info(&pdev->dev, "cvitekaadc_probe\n");

	adc = devm_kzalloc(&pdev->dev, sizeof(*adc), GFP_KERNEL);
	if (!adc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	adc->adc_base = devm_ioremap_resource(&pdev->dev, res);
	dev_dbg(&pdev->dev, "cvitekaadc get adc_base=0x%p\n", adc->adc_base);
	if (IS_ERR(adc->adc_base))
		return PTR_ERR(adc->adc_base);

	dev_set_drvdata(&pdev->dev, adc);
	adc->dev = &pdev->dev;

	ret = adc_device_register(adc);
	if (ret < 0) {
		pr_err("adc: register device error\n");
		return ret;
	}

	of_property_read_u32(pdev->dev.of_node, "clk_source", &mclk_source_addr);

	if (mclk_source_addr)
		adc->mclk_source = ioremap(mclk_source_addr, 0x100);
	else
		dev_err(&pdev->dev, "get MCLK source failed !!\n");

	/* set default input vol gain to maxmum 48dB, vol range is 0~24 */
	ctrl1 = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL1);
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL1, ctrl1 | AUDIO_ADC_IGR_INIT_EN);

	/* default input volume is 20 */
	adc_set_volume(adc, 20);

	return devm_snd_soc_register_component(&pdev->dev, &soc_component_dev_cv181xadc,
					  &cv181xadc_dai, 1);
}

static int cv181xadc_remove(struct platform_device *pdev)
{
	struct cv181xadc *adc = dev_get_drvdata(&pdev->dev);

	dev_dbg(&pdev->dev, "cvitekaadc_remove\n");
	iounmap(adc->mclk_source);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id cvitek_adc_of_match[] = {
	{ .compatible = "cvitek,cv182xaadc", },
	{},
};

MODULE_DEVICE_TABLE(of, cvitek_adc_of_match);
#endif

#ifdef CONFIG_PM_SLEEP
static int cv181xadc_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct cv181xadc *adc = platform_get_drvdata(pdev);

	if (!adc->reg_ctx) {
		adc->reg_ctx = devm_kzalloc(adc->dev, sizeof(struct cv181xadc_context), GFP_KERNEL);
		if (!adc->reg_ctx)
			return -ENOMEM;
	}

	adc->reg_ctx->ctl0 = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0);
	adc->reg_ctx->ctl1 = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL1);
	adc->reg_ctx->status = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_STATUS);
	adc->reg_ctx->ana0 = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0);
	adc->reg_ctx->ana2 = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2);
	adc->reg_ctx->ana3 = adc_read_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA3);

	return 0;
}

static int cv181xadc_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct cv181xadc *adc = platform_get_drvdata(pdev);

	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL0, adc->reg_ctx->ctl0);
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_CTRL1, adc->reg_ctx->ctl1);
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_STATUS, adc->reg_ctx->status);
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA0, adc->reg_ctx->ana0);
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA2, adc->reg_ctx->ana2);
	adc_write_reg(adc->adc_base, AUDIO_PHY_RXADC_ANA3, adc->reg_ctx->ana3);

	return 0;
}

static SIMPLE_DEV_PM_OPS(cv181xadc_pm_ops, cv181xadc_suspend,
			 cv181xadc_resume);
#endif


static struct platform_driver cv181xadc_platform_driver = {
	.probe		= cv181xadc_probe,
	.remove		= cv181xadc_remove,
	.driver		= {
		.name	= "cvitekaadc",
		.of_match_table = of_match_ptr(cvitek_adc_of_match),
#ifdef CONFIG_PM_SLEEP
		.pm	= &cv181xadc_pm_ops,
#endif
	},
};
module_platform_driver(cv181xadc_platform_driver);

MODULE_DESCRIPTION("ASoC CVITEK cvitekaADC driver");
MODULE_AUTHOR("Ethan Chen <ethan.chen@wisecore.com.tw>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:cvitekaadc");
