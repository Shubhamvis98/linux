// SPDX-License-Identifier: GPL-2.0-only
/*
 * OmniVision OV16A1Q 16 MP image sensor driver
 *
 * The register settings come from the vendor camera stack of the Xiaomi
 * Mi 11T Pro (vili) front camera module.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>

#define OV16A1Q_REG_CHIP_ID		CCI_REG24(0x300a)
#define OV16A1Q_CHIP_ID			0x561641

#define OV16A1Q_REG_MODE_SELECT		CCI_REG8(0x0100)
#define OV16A1Q_MODE_STANDBY		0x00
#define OV16A1Q_MODE_STREAMING		0x01
#define OV16A1Q_REG_SOFTWARE_RST	CCI_REG8(0x0103)

#define OV16A1Q_REG_EXPOSURE		CCI_REG24(0x3500)
#define OV16A1Q_EXPOSURE_MIN		8
#define OV16A1Q_EXPOSURE_STEP		1
#define OV16A1Q_EXPOSURE_MARGIN		8

/* Real gain = register value / 256, up to 15.5x */
#define OV16A1Q_REG_ANALOG_GAIN		CCI_REG16(0x3508)
#define OV16A1Q_ANA_GAIN_MIN		0x100
#define OV16A1Q_ANA_GAIN_MAX		0xf80
#define OV16A1Q_ANA_GAIN_STEP		1
#define OV16A1Q_ANA_GAIN_DEFAULT	0x100

/* 0x350a[1:0], 0x350b[7:0], 0x350c[7:6]: 1/1024 steps, up to 4x */
#define OV16A1Q_REG_DIGITAL_GAIN	CCI_REG24(0x350a)
#define OV16A1Q_DGTL_GAIN_SHIFT		6
#define OV16A1Q_DGTL_GAIN_MIN		1024
#define OV16A1Q_DGTL_GAIN_MAX		4095
#define OV16A1Q_DGTL_GAIN_STEP		1
#define OV16A1Q_DGTL_GAIN_DEFAULT	1024

#define OV16A1Q_REG_VTS			CCI_REG16(0x380e)
#define OV16A1Q_VTS_MAX			0x7fff

#define OV16A1Q_NATIVE_WIDTH		4656U
#define OV16A1Q_NATIVE_HEIGHT		3496U

#define OV16A1Q_MCLK_FREQ		19200000

/* 768 Mbps per lane, DDR */
#define OV16A1Q_LINK_FREQ		384000000ULL
#define OV16A1Q_NUM_LANES		4

/* Output pixel rate: link rate * lanes / bits per pixel */
#define OV16A1Q_PIXEL_RATE		(OV16A1Q_LINK_FREQ * 2 * \
					 OV16A1Q_NUM_LANES / 10)

struct ov16a1q_mode {
	u32 width;
	u32 height;
	/* Line length in pixel clocks, at OV16A1Q_PIXEL_RATE */
	u32 llp;
	/* Default frame length in lines */
	u32 fll_def;
	struct v4l2_rect crop;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

static const char * const ov16a1q_supply_names[] = {
	"vio",	/* I/O (1.8 V) */
	"vana",	/* Analog (2.8 V) */
	"vdig",	/* Digital core (1.2 V) */
};

static const s64 ov16a1q_link_freqs[] = {
	OV16A1Q_LINK_FREQ,
};

static const struct cci_reg_sequence ov16a1q_init_regs[] = {
	{ CCI_REG8(0x0102), 0x00 },
	{ CCI_REG8(0x0301), 0x48 },
	{ CCI_REG8(0x0302), 0x31 },
	{ CCI_REG8(0x0303), 0x04 },
	{ CCI_REG8(0x0305), 0xc2 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0320), 0x02 },
	{ CCI_REG8(0x0323), 0x04 },
	{ CCI_REG8(0x0326), 0xd8 },
	{ CCI_REG8(0x0327), 0x0b },
	{ CCI_REG8(0x0329), 0x01 },
	{ CCI_REG8(0x0343), 0x04 },
	{ CCI_REG8(0x0344), 0x01 },
	{ CCI_REG8(0x0345), 0x2c },
	{ CCI_REG8(0x0346), 0xc0 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x300e), 0x22 },
	{ CCI_REG8(0x3012), 0x41 },
	{ CCI_REG8(0x3016), 0xd2 },
	{ CCI_REG8(0x3018), 0x70 },
	{ CCI_REG8(0x301e), 0x98 },
	{ CCI_REG8(0x3025), 0x03 },
	{ CCI_REG8(0x3026), 0x10 },
	{ CCI_REG8(0x3027), 0x08 },
	{ CCI_REG8(0x3102), 0x00 },
	{ CCI_REG8(0x3400), 0x04 },
	{ CCI_REG8(0x3406), 0x04 },
	{ CCI_REG8(0x3408), 0x04 },
	{ CCI_REG8(0x3421), 0x09 },
	{ CCI_REG8(0x3422), 0x20 },
	{ CCI_REG8(0x3423), 0x15 },
	{ CCI_REG8(0x3424), 0x40 },
	{ CCI_REG8(0x3425), 0x14 },
	{ CCI_REG8(0x3426), 0x04 },
	{ CCI_REG8(0x3504), 0x08 },
	{ CCI_REG8(0x3508), 0x01 },
	{ CCI_REG8(0x3509), 0x00 },
	{ CCI_REG8(0x350a), 0x01 },
	{ CCI_REG8(0x350b), 0x00 },
	{ CCI_REG8(0x350c), 0x00 },
	{ CCI_REG8(0x3548), 0x01 },
	{ CCI_REG8(0x3549), 0x00 },
	{ CCI_REG8(0x354a), 0x01 },
	{ CCI_REG8(0x354b), 0x00 },
	{ CCI_REG8(0x354c), 0x00 },
	{ CCI_REG8(0x3600), 0xff },
	{ CCI_REG8(0x3602), 0x42 },
	{ CCI_REG8(0x3603), 0x7b },
	{ CCI_REG8(0x3608), 0x9b },
	{ CCI_REG8(0x360a), 0x69 },
	{ CCI_REG8(0x360b), 0x53 },
	{ CCI_REG8(0x3618), 0xc0 },
	{ CCI_REG8(0x361a), 0x8b },
	{ CCI_REG8(0x361d), 0x20 },
	{ CCI_REG8(0x361e), 0x30 },
	{ CCI_REG8(0x361f), 0x01 },
	{ CCI_REG8(0x3620), 0x89 },
	{ CCI_REG8(0x3624), 0x8f },
	{ CCI_REG8(0x3629), 0x09 },
	{ CCI_REG8(0x362e), 0x50 },
	{ CCI_REG8(0x3631), 0xe2 },
	{ CCI_REG8(0x3632), 0xe2 },
	{ CCI_REG8(0x3634), 0x10 },
	{ CCI_REG8(0x3635), 0x10 },
	{ CCI_REG8(0x3636), 0x10 },
	{ CCI_REG8(0x3639), 0xa6 },
	{ CCI_REG8(0x363a), 0xaa },
	{ CCI_REG8(0x363b), 0x0c },
	{ CCI_REG8(0x363c), 0x16 },
	{ CCI_REG8(0x363d), 0x29 },
	{ CCI_REG8(0x363e), 0x4f },
	{ CCI_REG8(0x3642), 0xa8 },
	{ CCI_REG8(0x3652), 0x00 },
	{ CCI_REG8(0x3653), 0x00 },
	{ CCI_REG8(0x3654), 0x8a },
	{ CCI_REG8(0x3656), 0x0c },
	{ CCI_REG8(0x3657), 0x8e },
	{ CCI_REG8(0x3660), 0x80 },
	{ CCI_REG8(0x3663), 0x00 },
	{ CCI_REG8(0x3664), 0x00 },
	{ CCI_REG8(0x3668), 0x05 },
	{ CCI_REG8(0x3669), 0x05 },
	{ CCI_REG8(0x370d), 0x10 },
	{ CCI_REG8(0x370e), 0x05 },
	{ CCI_REG8(0x370f), 0x10 },
	{ CCI_REG8(0x3711), 0x01 },
	{ CCI_REG8(0x3712), 0x09 },
	{ CCI_REG8(0x3713), 0x40 },
	{ CCI_REG8(0x3714), 0xe4 },
	{ CCI_REG8(0x3716), 0x04 },
	{ CCI_REG8(0x3717), 0x01 },
	{ CCI_REG8(0x3718), 0x02 },
	{ CCI_REG8(0x3719), 0x01 },
	{ CCI_REG8(0x371a), 0x02 },
	{ CCI_REG8(0x371b), 0x02 },
	{ CCI_REG8(0x371c), 0x01 },
	{ CCI_REG8(0x371d), 0x02 },
	{ CCI_REG8(0x371e), 0x12 },
	{ CCI_REG8(0x371f), 0x02 },
	{ CCI_REG8(0x3720), 0x14 },
	{ CCI_REG8(0x3721), 0x12 },
	{ CCI_REG8(0x3722), 0x44 },
	{ CCI_REG8(0x3723), 0x60 },
	{ CCI_REG8(0x372f), 0x34 },
	{ CCI_REG8(0x3726), 0x21 },
	{ CCI_REG8(0x37d0), 0x02 },
	{ CCI_REG8(0x37d1), 0x10 },
	{ CCI_REG8(0x37db), 0x08 },
	{ CCI_REG8(0x3808), 0x12 },
	{ CCI_REG8(0x3809), 0x30 },
	{ CCI_REG8(0x380a), 0x0d },
	{ CCI_REG8(0x380b), 0xa8 },
	{ CCI_REG8(0x380c), 0x03 },
	{ CCI_REG8(0x380d), 0x52 },
	{ CCI_REG8(0x380e), 0x0f },
	{ CCI_REG8(0x380f), 0x51 },
	{ CCI_REG8(0x3814), 0x11 },
	{ CCI_REG8(0x3815), 0x11 },
	{ CCI_REG8(0x3820), 0x00 },
	{ CCI_REG8(0x3821), 0x06 },
	{ CCI_REG8(0x3822), 0x00 },
	{ CCI_REG8(0x3823), 0x04 },
	{ CCI_REG8(0x3837), 0x10 },
	{ CCI_REG8(0x383c), 0x34 },
	{ CCI_REG8(0x383d), 0xff },
	{ CCI_REG8(0x383e), 0x0d },
	{ CCI_REG8(0x383f), 0x22 },
	{ CCI_REG8(0x3857), 0x00 },
	{ CCI_REG8(0x388f), 0x00 },
	{ CCI_REG8(0x3890), 0x00 },
	{ CCI_REG8(0x3891), 0x00 },
	{ CCI_REG8(0x3d81), 0x10 },
	{ CCI_REG8(0x3d83), 0x0c },
	{ CCI_REG8(0x3d84), 0x00 },
	{ CCI_REG8(0x3d85), 0x1b },
	{ CCI_REG8(0x3d88), 0x00 },
	{ CCI_REG8(0x3d89), 0x00 },
	{ CCI_REG8(0x3d8a), 0x00 },
	{ CCI_REG8(0x3d8b), 0x01 },
	{ CCI_REG8(0x3d8c), 0x77 },
	{ CCI_REG8(0x3d8d), 0xa0 },
	{ CCI_REG8(0x3f00), 0x02 },
	{ CCI_REG8(0x3f0c), 0x07 },
	{ CCI_REG8(0x3f0d), 0x2f },
	{ CCI_REG8(0x4012), 0x0d },
	{ CCI_REG8(0x4015), 0x04 },
	{ CCI_REG8(0x4016), 0x1b },
	{ CCI_REG8(0x4017), 0x04 },
	{ CCI_REG8(0x4018), 0x0b },
	{ CCI_REG8(0x401b), 0x1f },
	{ CCI_REG8(0x401e), 0x01 },
	{ CCI_REG8(0x401f), 0x38 },
	{ CCI_REG8(0x4500), 0x20 },
	{ CCI_REG8(0x4501), 0x6a },
	{ CCI_REG8(0x4502), 0xb4 },
	{ CCI_REG8(0x4586), 0x00 },
	{ CCI_REG8(0x4588), 0x02 },
	{ CCI_REG8(0x4640), 0x01 },
	{ CCI_REG8(0x4641), 0x04 },
	{ CCI_REG8(0x4643), 0x00 },
	{ CCI_REG8(0x4645), 0x03 },
	{ CCI_REG8(0x4806), 0x40 },
	{ CCI_REG8(0x480e), 0x00 },
	{ CCI_REG8(0x4815), 0x2b },
	{ CCI_REG8(0x481b), 0x3c },
	{ CCI_REG8(0x4833), 0x18 },
	{ CCI_REG8(0x4837), 0x08 },
	{ CCI_REG8(0x484b), 0x07 },
	{ CCI_REG8(0x4850), 0x41 },
	{ CCI_REG8(0x4860), 0x00 },
	{ CCI_REG8(0x4861), 0xec },
	{ CCI_REG8(0x4864), 0x00 },
	{ CCI_REG8(0x4883), 0x00 },
	{ CCI_REG8(0x4888), 0x10 },
	{ CCI_REG8(0x4a00), 0x10 },
	{ CCI_REG8(0x4e00), 0x00 },
	{ CCI_REG8(0x4e01), 0x04 },
	{ CCI_REG8(0x4e02), 0x01 },
	{ CCI_REG8(0x4e03), 0x00 },
	{ CCI_REG8(0x4e04), 0x08 },
	{ CCI_REG8(0x4e05), 0x04 },
	{ CCI_REG8(0x4e06), 0x00 },
	{ CCI_REG8(0x4e07), 0x13 },
	{ CCI_REG8(0x4e08), 0x01 },
	{ CCI_REG8(0x4e09), 0x00 },
	{ CCI_REG8(0x4e0a), 0x15 },
	{ CCI_REG8(0x4e0b), 0x0e },
	{ CCI_REG8(0x4e0c), 0x00 },
	{ CCI_REG8(0x4e0d), 0x17 },
	{ CCI_REG8(0x4e0e), 0x07 },
	{ CCI_REG8(0x4e0f), 0x00 },
	{ CCI_REG8(0x4e10), 0x19 },
	{ CCI_REG8(0x4e11), 0x06 },
	{ CCI_REG8(0x4e12), 0x00 },
	{ CCI_REG8(0x4e13), 0x1b },
	{ CCI_REG8(0x4e14), 0x08 },
	{ CCI_REG8(0x4e15), 0x00 },
	{ CCI_REG8(0x4e16), 0x1f },
	{ CCI_REG8(0x4e17), 0x08 },
	{ CCI_REG8(0x4e18), 0x00 },
	{ CCI_REG8(0x4e19), 0x21 },
	{ CCI_REG8(0x4e1a), 0x0e },
	{ CCI_REG8(0x4e1b), 0x00 },
	{ CCI_REG8(0x4e1c), 0x2d },
	{ CCI_REG8(0x4e1d), 0x30 },
	{ CCI_REG8(0x4e1e), 0x00 },
	{ CCI_REG8(0x4e1f), 0x6a },
	{ CCI_REG8(0x4e20), 0x05 },
	{ CCI_REG8(0x4e21), 0x00 },
	{ CCI_REG8(0x4e22), 0x6c },
	{ CCI_REG8(0x4e23), 0x05 },
	{ CCI_REG8(0x4e24), 0x00 },
	{ CCI_REG8(0x4e25), 0x6e },
	{ CCI_REG8(0x4e26), 0x39 },
	{ CCI_REG8(0x4e27), 0x00 },
	{ CCI_REG8(0x4e28), 0x7a },
	{ CCI_REG8(0x4e29), 0x6d },
	{ CCI_REG8(0x4e2a), 0x00 },
	{ CCI_REG8(0x4e2b), 0x00 },
	{ CCI_REG8(0x4e2c), 0x00 },
	{ CCI_REG8(0x4e2d), 0x00 },
	{ CCI_REG8(0x4e2e), 0x00 },
	{ CCI_REG8(0x4e2f), 0x00 },
	{ CCI_REG8(0x4e30), 0x00 },
	{ CCI_REG8(0x4e31), 0x00 },
	{ CCI_REG8(0x4e32), 0x00 },
	{ CCI_REG8(0x4e33), 0x00 },
	{ CCI_REG8(0x4e34), 0x00 },
	{ CCI_REG8(0x4e35), 0x00 },
	{ CCI_REG8(0x4e36), 0x00 },
	{ CCI_REG8(0x4e37), 0x00 },
	{ CCI_REG8(0x4e38), 0x00 },
	{ CCI_REG8(0x4e39), 0x00 },
	{ CCI_REG8(0x4e3a), 0x00 },
	{ CCI_REG8(0x4e3b), 0x00 },
	{ CCI_REG8(0x4e3c), 0x00 },
	{ CCI_REG8(0x4e3d), 0x00 },
	{ CCI_REG8(0x4e3e), 0x00 },
	{ CCI_REG8(0x4e3f), 0x00 },
	{ CCI_REG8(0x4e40), 0x00 },
	{ CCI_REG8(0x4e41), 0x00 },
	{ CCI_REG8(0x4e42), 0x00 },
	{ CCI_REG8(0x4e43), 0x00 },
	{ CCI_REG8(0x4e44), 0x00 },
	{ CCI_REG8(0x4e45), 0x00 },
	{ CCI_REG8(0x4e46), 0x00 },
	{ CCI_REG8(0x4e47), 0x00 },
	{ CCI_REG8(0x4e48), 0x00 },
	{ CCI_REG8(0x4e49), 0x00 },
	{ CCI_REG8(0x4e4a), 0x00 },
	{ CCI_REG8(0x4e4b), 0x00 },
	{ CCI_REG8(0x4e4c), 0x00 },
	{ CCI_REG8(0x4e4d), 0x00 },
	{ CCI_REG8(0x4e4e), 0x00 },
	{ CCI_REG8(0x4e4f), 0x00 },
	{ CCI_REG8(0x4e50), 0x00 },
	{ CCI_REG8(0x4e51), 0x00 },
	{ CCI_REG8(0x4e52), 0x00 },
	{ CCI_REG8(0x4e53), 0x00 },
	{ CCI_REG8(0x4e54), 0x00 },
	{ CCI_REG8(0x4e55), 0x00 },
	{ CCI_REG8(0x4e56), 0x00 },
	{ CCI_REG8(0x4e57), 0x00 },
	{ CCI_REG8(0x4e58), 0x00 },
	{ CCI_REG8(0x4e59), 0x00 },
	{ CCI_REG8(0x4e5a), 0x00 },
	{ CCI_REG8(0x4e5b), 0x00 },
	{ CCI_REG8(0x4e5c), 0x00 },
	{ CCI_REG8(0x4e5d), 0x00 },
	{ CCI_REG8(0x4e5e), 0x00 },
	{ CCI_REG8(0x4e5f), 0x00 },
	{ CCI_REG8(0x4e60), 0x00 },
	{ CCI_REG8(0x4e61), 0x00 },
	{ CCI_REG8(0x4e62), 0x00 },
	{ CCI_REG8(0x4e63), 0x00 },
	{ CCI_REG8(0x4e64), 0x00 },
	{ CCI_REG8(0x4e65), 0x00 },
	{ CCI_REG8(0x4e66), 0x00 },
	{ CCI_REG8(0x4e67), 0x00 },
	{ CCI_REG8(0x4e68), 0x00 },
	{ CCI_REG8(0x4e69), 0x00 },
	{ CCI_REG8(0x4e6a), 0x00 },
	{ CCI_REG8(0x4e6b), 0x00 },
	{ CCI_REG8(0x4e6c), 0x00 },
	{ CCI_REG8(0x4e6d), 0x00 },
	{ CCI_REG8(0x4e6e), 0x00 },
	{ CCI_REG8(0x4e6f), 0x00 },
	{ CCI_REG8(0x4e70), 0x00 },
	{ CCI_REG8(0x4e71), 0x00 },
	{ CCI_REG8(0x4e72), 0x00 },
	{ CCI_REG8(0x4e73), 0x00 },
	{ CCI_REG8(0x4e74), 0x00 },
	{ CCI_REG8(0x4e75), 0x00 },
	{ CCI_REG8(0x4e76), 0x00 },
	{ CCI_REG8(0x4e77), 0x00 },
	{ CCI_REG8(0x4e78), 0x1c },
	{ CCI_REG8(0x4e79), 0x1e },
	{ CCI_REG8(0x4e7a), 0x00 },
	{ CCI_REG8(0x4e7b), 0x00 },
	{ CCI_REG8(0x4e7c), 0x2c },
	{ CCI_REG8(0x4e7d), 0x2f },
	{ CCI_REG8(0x4e7e), 0x79 },
	{ CCI_REG8(0x4e7f), 0x7b },
	{ CCI_REG8(0x4e80), 0x0a },
	{ CCI_REG8(0x4e81), 0x31 },
	{ CCI_REG8(0x4e82), 0x66 },
	{ CCI_REG8(0x4e83), 0x81 },
	{ CCI_REG8(0x4e84), 0x03 },
	{ CCI_REG8(0x4e85), 0x40 },
	{ CCI_REG8(0x4e86), 0x02 },
	{ CCI_REG8(0x4e87), 0x09 },
	{ CCI_REG8(0x4e88), 0x43 },
	{ CCI_REG8(0x4e89), 0x53 },
	{ CCI_REG8(0x4e8a), 0x32 },
	{ CCI_REG8(0x4e8b), 0x67 },
	{ CCI_REG8(0x4e8c), 0x05 },
	{ CCI_REG8(0x4e8d), 0x83 },
	{ CCI_REG8(0x4e8e), 0x00 },
	{ CCI_REG8(0x4e8f), 0x00 },
	{ CCI_REG8(0x4e90), 0x00 },
	{ CCI_REG8(0x4e91), 0x00 },
	{ CCI_REG8(0x4e92), 0x00 },
	{ CCI_REG8(0x4e93), 0x00 },
	{ CCI_REG8(0x4e94), 0x00 },
	{ CCI_REG8(0x4e95), 0x00 },
	{ CCI_REG8(0x4e96), 0x00 },
	{ CCI_REG8(0x4e97), 0x00 },
	{ CCI_REG8(0x4e98), 0x00 },
	{ CCI_REG8(0x4e99), 0x00 },
	{ CCI_REG8(0x4e9a), 0x00 },
	{ CCI_REG8(0x4e9b), 0x00 },
	{ CCI_REG8(0x4e9c), 0x00 },
	{ CCI_REG8(0x4e9d), 0x00 },
	{ CCI_REG8(0x4e9e), 0x00 },
	{ CCI_REG8(0x4e9f), 0x00 },
	{ CCI_REG8(0x4ea0), 0x00 },
	{ CCI_REG8(0x4ea1), 0x00 },
	{ CCI_REG8(0x4ea2), 0x00 },
	{ CCI_REG8(0x4ea3), 0x00 },
	{ CCI_REG8(0x4ea4), 0x00 },
	{ CCI_REG8(0x4ea5), 0x00 },
	{ CCI_REG8(0x4ea6), 0x1e },
	{ CCI_REG8(0x4ea7), 0x20 },
	{ CCI_REG8(0x4ea8), 0x32 },
	{ CCI_REG8(0x4ea9), 0x6d },
	{ CCI_REG8(0x4eaa), 0x18 },
	{ CCI_REG8(0x4eab), 0x7f },
	{ CCI_REG8(0x4eac), 0x00 },
	{ CCI_REG8(0x4ead), 0x00 },
	{ CCI_REG8(0x4eae), 0x7c },
	{ CCI_REG8(0x4eaf), 0x07 },
	{ CCI_REG8(0x4eb0), 0x7c },
	{ CCI_REG8(0x4eb1), 0x07 },
	{ CCI_REG8(0x4eb2), 0x07 },
	{ CCI_REG8(0x4eb3), 0x1c },
	{ CCI_REG8(0x4eb4), 0x07 },
	{ CCI_REG8(0x4eb5), 0x1c },
	{ CCI_REG8(0x4eb6), 0x07 },
	{ CCI_REG8(0x4eb7), 0x1c },
	{ CCI_REG8(0x4eb8), 0x07 },
	{ CCI_REG8(0x4eb9), 0x1c },
	{ CCI_REG8(0x4eba), 0x07 },
	{ CCI_REG8(0x4ebb), 0x14 },
	{ CCI_REG8(0x4ebc), 0x07 },
	{ CCI_REG8(0x4ebd), 0x1c },
	{ CCI_REG8(0x4ebe), 0x07 },
	{ CCI_REG8(0x4ebf), 0x1c },
	{ CCI_REG8(0x4ec0), 0x07 },
	{ CCI_REG8(0x4ec1), 0x1c },
	{ CCI_REG8(0x4ec2), 0x07 },
	{ CCI_REG8(0x4ec3), 0x1c },
	{ CCI_REG8(0x4ec4), 0x2c },
	{ CCI_REG8(0x4ec5), 0x2f },
	{ CCI_REG8(0x4ec6), 0x79 },
	{ CCI_REG8(0x4ec7), 0x7b },
	{ CCI_REG8(0x4ec8), 0x7c },
	{ CCI_REG8(0x4ec9), 0x07 },
	{ CCI_REG8(0x4eca), 0x7c },
	{ CCI_REG8(0x4ecb), 0x07 },
	{ CCI_REG8(0x4ecc), 0x00 },
	{ CCI_REG8(0x4ecd), 0x00 },
	{ CCI_REG8(0x4ece), 0x07 },
	{ CCI_REG8(0x4ecf), 0x31 },
	{ CCI_REG8(0x4ed0), 0x69 },
	{ CCI_REG8(0x4ed1), 0x7f },
	{ CCI_REG8(0x4ed2), 0x67 },
	{ CCI_REG8(0x4ed3), 0x00 },
	{ CCI_REG8(0x4ed4), 0x00 },
	{ CCI_REG8(0x4ed5), 0x00 },
	{ CCI_REG8(0x4ed6), 0x7c },
	{ CCI_REG8(0x4ed7), 0x07 },
	{ CCI_REG8(0x4ed8), 0x7c },
	{ CCI_REG8(0x4ed9), 0x07 },
	{ CCI_REG8(0x4eda), 0x33 },
	{ CCI_REG8(0x4edb), 0x7f },
	{ CCI_REG8(0x4edc), 0x00 },
	{ CCI_REG8(0x4edd), 0x16 },
	{ CCI_REG8(0x4ede), 0x00 },
	{ CCI_REG8(0x4edf), 0x00 },
	{ CCI_REG8(0x4ee0), 0x32 },
	{ CCI_REG8(0x4ee1), 0x70 },
	{ CCI_REG8(0x4ee2), 0x01 },
	{ CCI_REG8(0x4ee3), 0x30 },
	{ CCI_REG8(0x4ee4), 0x22 },
	{ CCI_REG8(0x4ee5), 0x28 },
	{ CCI_REG8(0x4ee6), 0x6f },
	{ CCI_REG8(0x4ee7), 0x75 },
	{ CCI_REG8(0x4ee8), 0x00 },
	{ CCI_REG8(0x4ee9), 0x00 },
	{ CCI_REG8(0x4eea), 0x30 },
	{ CCI_REG8(0x4eeb), 0x7f },
	{ CCI_REG8(0x4eec), 0x00 },
	{ CCI_REG8(0x4eed), 0x00 },
	{ CCI_REG8(0x4eee), 0x00 },
	{ CCI_REG8(0x4eef), 0x00 },
	{ CCI_REG8(0x4ef0), 0x69 },
	{ CCI_REG8(0x4ef1), 0x7f },
	{ CCI_REG8(0x4ef2), 0x07 },
	{ CCI_REG8(0x4ef3), 0x30 },
	{ CCI_REG8(0x4ef4), 0x32 },
	{ CCI_REG8(0x4ef5), 0x09 },
	{ CCI_REG8(0x4ef6), 0x7d },
	{ CCI_REG8(0x4ef7), 0x65 },
	{ CCI_REG8(0x4ef8), 0x00 },
	{ CCI_REG8(0x4ef9), 0x00 },
	{ CCI_REG8(0x4efa), 0x00 },
	{ CCI_REG8(0x4efb), 0x00 },
	{ CCI_REG8(0x4efc), 0x7f },
	{ CCI_REG8(0x4efd), 0x09 },
	{ CCI_REG8(0x4efe), 0x7f },
	{ CCI_REG8(0x4eff), 0x09 },
	{ CCI_REG8(0x4f00), 0x1e },
	{ CCI_REG8(0x4f01), 0x7c },
	{ CCI_REG8(0x4f02), 0x7f },
	{ CCI_REG8(0x4f03), 0x09 },
	{ CCI_REG8(0x4f04), 0x7f },
	{ CCI_REG8(0x4f05), 0x0b },
	{ CCI_REG8(0x4f06), 0x7c },
	{ CCI_REG8(0x4f07), 0x02 },
	{ CCI_REG8(0x4f08), 0x7c },
	{ CCI_REG8(0x4f09), 0x02 },
	{ CCI_REG8(0x4f0a), 0x32 },
	{ CCI_REG8(0x4f0b), 0x64 },
	{ CCI_REG8(0x4f0c), 0x32 },
	{ CCI_REG8(0x4f0d), 0x64 },
	{ CCI_REG8(0x4f0e), 0x32 },
	{ CCI_REG8(0x4f0f), 0x64 },
	{ CCI_REG8(0x4f10), 0x32 },
	{ CCI_REG8(0x4f11), 0x64 },
	{ CCI_REG8(0x4f12), 0x31 },
	{ CCI_REG8(0x4f13), 0x4f },
	{ CCI_REG8(0x4f14), 0x83 },
	{ CCI_REG8(0x4f15), 0x84 },
	{ CCI_REG8(0x4f16), 0x63 },
	{ CCI_REG8(0x4f17), 0x64 },
	{ CCI_REG8(0x4f18), 0x83 },
	{ CCI_REG8(0x4f19), 0x84 },
	{ CCI_REG8(0x4f1a), 0x31 },
	{ CCI_REG8(0x4f1b), 0x32 },
	{ CCI_REG8(0x4f1c), 0x7b },
	{ CCI_REG8(0x4f1d), 0x7c },
	{ CCI_REG8(0x4f1e), 0x2f },
	{ CCI_REG8(0x4f1f), 0x30 },
	{ CCI_REG8(0x4f20), 0x30 },
	{ CCI_REG8(0x4f21), 0x69 },
	{ CCI_REG8(0x4d06), 0x08 },
	{ CCI_REG8(0x5000), 0x01 },
	{ CCI_REG8(0x5001), 0x40 },
	{ CCI_REG8(0x5002), 0x53 },
	{ CCI_REG8(0x5003), 0x42 },
	{ CCI_REG8(0x5004), 0x08 },
	{ CCI_REG8(0x5005), 0x00 },
	{ CCI_REG8(0x5012), 0x60 },
	{ CCI_REG8(0x5038), 0x00 },
	{ CCI_REG8(0x5081), 0x00 },
	{ CCI_REG8(0x5180), 0x00 },
	{ CCI_REG8(0x5181), 0x10 },
	{ CCI_REG8(0x5182), 0x07 },
	{ CCI_REG8(0x5183), 0x8f },
	{ CCI_REG8(0x5184), 0x03 },
	{ CCI_REG8(0x5208), 0xc2 },
	{ CCI_REG8(0x5820), 0xc5 },
	{ CCI_REG8(0x5854), 0x00 },
	{ CCI_REG8(0x58cb), 0x03 },
	{ CCI_REG8(0x5bd0), 0x15 },
	{ CCI_REG8(0x5bd1), 0x02 },
	{ CCI_REG8(0x5c0e), 0x11 },
	{ CCI_REG8(0x5c11), 0x00 },
	{ CCI_REG8(0x5c16), 0x02 },
	{ CCI_REG8(0x5c17), 0x01 },
	{ CCI_REG8(0x5c1a), 0x04 },
	{ CCI_REG8(0x5c1b), 0x03 },
	{ CCI_REG8(0x5c21), 0x10 },
	{ CCI_REG8(0x5c22), 0x10 },
	{ CCI_REG8(0x5c23), 0x04 },
	{ CCI_REG8(0x5c24), 0x0c },
	{ CCI_REG8(0x5c25), 0x04 },
	{ CCI_REG8(0x5c26), 0x0c },
	{ CCI_REG8(0x5c27), 0x04 },
	{ CCI_REG8(0x5c28), 0x0c },
	{ CCI_REG8(0x5c29), 0x04 },
	{ CCI_REG8(0x5c2a), 0x0c },
	{ CCI_REG8(0x5c2b), 0x01 },
	{ CCI_REG8(0x5c2c), 0x01 },
	{ CCI_REG8(0x5c2e), 0x08 },
	{ CCI_REG8(0x5c30), 0x04 },
	{ CCI_REG8(0x5c35), 0x03 },
	{ CCI_REG8(0x5c36), 0x03 },
	{ CCI_REG8(0x5c37), 0x03 },
	{ CCI_REG8(0x5c38), 0x03 },
	{ CCI_REG8(0x5d00), 0xff },
	{ CCI_REG8(0x5d01), 0x0f },
	{ CCI_REG8(0x5d02), 0x80 },
	{ CCI_REG8(0x5d03), 0x44 },
	{ CCI_REG8(0x5d05), 0xfc },
	{ CCI_REG8(0x5d06), 0x0b },
	{ CCI_REG8(0x5d08), 0x10 },
	{ CCI_REG8(0x5d09), 0x10 },
	{ CCI_REG8(0x5d0a), 0x04 },
	{ CCI_REG8(0x5d0b), 0x0c },
	{ CCI_REG8(0x5d0c), 0x04 },
	{ CCI_REG8(0x5d0d), 0x0c },
	{ CCI_REG8(0x5d0e), 0x04 },
	{ CCI_REG8(0x5d0f), 0x0c },
	{ CCI_REG8(0x5d10), 0x04 },
	{ CCI_REG8(0x5d11), 0x0c },
	{ CCI_REG8(0x5d12), 0x01 },
	{ CCI_REG8(0x5d13), 0x01 },
	{ CCI_REG8(0x5d15), 0x10 },
	{ CCI_REG8(0x5d16), 0x10 },
	{ CCI_REG8(0x5d17), 0x10 },
	{ CCI_REG8(0x5d18), 0x10 },
	{ CCI_REG8(0x5d1a), 0x10 },
	{ CCI_REG8(0x5d1b), 0x10 },
	{ CCI_REG8(0x5d1c), 0x10 },
	{ CCI_REG8(0x5d1d), 0x10 },
	{ CCI_REG8(0x5d1e), 0x04 },
	{ CCI_REG8(0x5d1f), 0x04 },
	{ CCI_REG8(0x5d20), 0x04 },
	{ CCI_REG8(0x5d27), 0x64 },
	{ CCI_REG8(0x5d28), 0xc8 },
	{ CCI_REG8(0x5d29), 0x96 },
	{ CCI_REG8(0x5d2a), 0xff },
	{ CCI_REG8(0x5d2b), 0xc8 },
	{ CCI_REG8(0x5d2c), 0xff },
	{ CCI_REG8(0x5d2d), 0x04 },
	{ CCI_REG8(0x5d34), 0x00 },
	{ CCI_REG8(0x5d35), 0x08 },
	{ CCI_REG8(0x5d36), 0x00 },
	{ CCI_REG8(0x5d37), 0x04 },
	{ CCI_REG8(0x5d4a), 0x00 },
	{ CCI_REG8(0x5d4c), 0x00 },
	{ CCI_REG8(0x0323), 0x05 },
	{ CCI_REG8(0x0324), 0x02 },
	{ CCI_REG8(0x0325), 0xee },
	{ CCI_REG8(0x0345), 0x77 },
};

static const struct cci_reg_sequence ov16a1q_mode_2304x1728_regs[] = {
	{ CCI_REG8(0x0305), 0xe0 },
	{ CCI_REG8(0x0307), 0x01 },
	{ CCI_REG8(0x4837), 0x14 },
	{ CCI_REG8(0x0329), 0x01 },
	{ CCI_REG8(0x0344), 0x01 },
	{ CCI_REG8(0x0345), 0x77 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x3608), 0x75 },
	{ CCI_REG8(0x360a), 0x69 },
	{ CCI_REG8(0x361a), 0x8b },
	{ CCI_REG8(0x361e), 0x30 },
	{ CCI_REG8(0x3639), 0x93 },
	{ CCI_REG8(0x363a), 0x99 },
	{ CCI_REG8(0x3642), 0x98 },
	{ CCI_REG8(0x3654), 0x8a },
	{ CCI_REG8(0x3656), 0x0c },
	{ CCI_REG8(0x3663), 0x00 },
	{ CCI_REG8(0x370e), 0x05 },
	{ CCI_REG8(0x3712), 0x08 },
	{ CCI_REG8(0x3713), 0xc0 },
	{ CCI_REG8(0x3714), 0xe2 },
	{ CCI_REG8(0x37d0), 0x02 },
	{ CCI_REG8(0x37d1), 0x10 },
	{ CCI_REG8(0x37db), 0x04 },
	{ CCI_REG8(0x3808), 0x09 },
	{ CCI_REG8(0x3809), 0x00 },
	{ CCI_REG8(0x380a), 0x06 },
	{ CCI_REG8(0x380b), 0xc0 },
	{ CCI_REG8(0x380c), 0x03 },
	{ CCI_REG8(0x380d), 0x52 },
	{ CCI_REG8(0x380e), 0x0f },
	{ CCI_REG8(0x380f), 0x50 },
	{ CCI_REG8(0x3814), 0x22 },
	{ CCI_REG8(0x3815), 0x22 },
	{ CCI_REG8(0x3820), 0x01 },
	{ CCI_REG8(0x3821), 0x0c },
	{ CCI_REG8(0x3822), 0x00 },
	{ CCI_REG8(0x383c), 0x22 },
	{ CCI_REG8(0x383f), 0x33 },
	{ CCI_REG8(0x4015), 0x02 },
	{ CCI_REG8(0x4016), 0x0d },
	{ CCI_REG8(0x4017), 0x00 },
	{ CCI_REG8(0x4018), 0x07 },
	{ CCI_REG8(0x401b), 0x1f },
	{ CCI_REG8(0x401f), 0xfe },
	{ CCI_REG8(0x4500), 0x20 },
	{ CCI_REG8(0x4501), 0x6a },
	{ CCI_REG8(0x4502), 0xe4 },
	{ CCI_REG8(0x4e05), 0x04 },
	{ CCI_REG8(0x4e11), 0x06 },
	{ CCI_REG8(0x4e1d), 0x25 },
	{ CCI_REG8(0x4e26), 0x44 },
	{ CCI_REG8(0x4e29), 0x6d },
	{ CCI_REG8(0x5000), 0x09 },
	{ CCI_REG8(0x5001), 0x42 },
	{ CCI_REG8(0x5003), 0x42 },
	{ CCI_REG8(0x5820), 0xc5 },
	{ CCI_REG8(0x5854), 0x00 },
	{ CCI_REG8(0x5bd0), 0x19 },
	{ CCI_REG8(0x5c0e), 0x13 },
	{ CCI_REG8(0x5c11), 0x00 },
	{ CCI_REG8(0x5c16), 0x01 },
	{ CCI_REG8(0x5c17), 0x00 },
	{ CCI_REG8(0x5c1a), 0x00 },
	{ CCI_REG8(0x5c1b), 0x00 },
	{ CCI_REG8(0x5c21), 0x08 },
	{ CCI_REG8(0x5c22), 0x08 },
	{ CCI_REG8(0x5c23), 0x02 },
	{ CCI_REG8(0x5c24), 0x06 },
	{ CCI_REG8(0x5c25), 0x02 },
	{ CCI_REG8(0x5c26), 0x06 },
	{ CCI_REG8(0x5c27), 0x02 },
	{ CCI_REG8(0x5c28), 0x06 },
	{ CCI_REG8(0x5c29), 0x02 },
	{ CCI_REG8(0x5c2a), 0x06 },
	{ CCI_REG8(0x5c2b), 0x00 },
	{ CCI_REG8(0x5c2c), 0x00 },
	{ CCI_REG8(0x5d01), 0x07 },
	{ CCI_REG8(0x5d08), 0x08 },
	{ CCI_REG8(0x5d09), 0x08 },
	{ CCI_REG8(0x5d0a), 0x02 },
	{ CCI_REG8(0x5d0b), 0x06 },
	{ CCI_REG8(0x5d0c), 0x02 },
	{ CCI_REG8(0x5d0d), 0x06 },
	{ CCI_REG8(0x5d0e), 0x02 },
	{ CCI_REG8(0x5d0f), 0x06 },
	{ CCI_REG8(0x5d10), 0x02 },
	{ CCI_REG8(0x5d11), 0x06 },
	{ CCI_REG8(0x5d12), 0x00 },
	{ CCI_REG8(0x5d13), 0x00 },
	{ CCI_REG8(0x3500), 0x00 },
	{ CCI_REG8(0x3501), 0x0f },
	{ CCI_REG8(0x3502), 0x48 },
	{ CCI_REG8(0x3508), 0x01 },
	{ CCI_REG8(0x3509), 0x00 },
	{ CCI_REG8(0x370e), 0x05 },
	{ CCI_REG8(0x3712), 0x08 },
	{ CCI_REG8(0x3820), 0x05 },
	{ CCI_REG8(0x3821), 0x08 },
	{ CCI_REG8(0x4202), 0x01 },
};

static const struct ov16a1q_mode ov16a1q_modes[] = {
	{
		/* 2x2 binned, 30 fps */
		.width = 2304,
		.height = 1728,
		.llp = 2611,
		.fll_def = 3920,
		.crop = {
			.left = 24,
			.top = 20,
			.width = 4608,
			.height = 3456,
		},
		.regs = ov16a1q_mode_2304x1728_regs,
		.num_regs = ARRAY_SIZE(ov16a1q_mode_2304x1728_regs),
	},
};

struct ov16a1q {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ov16a1q_supply_names)];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;

	const struct ov16a1q_mode *cur_mode;
};

static inline struct ov16a1q *to_ov16a1q(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov16a1q, sd);
}

static u32 ov16a1q_get_format_code(struct ov16a1q *ov16a1q)
{
	return MEDIA_BUS_FMT_SBGGR10_1X10;
}

static int ov16a1q_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov16a1q *ov16a1q = container_of(ctrl->handler, struct ov16a1q,
					     ctrl_handler);
	const struct ov16a1q_mode *mode = ov16a1q->cur_mode;
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = mode->height + ctrl->val - OV16A1Q_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(ov16a1q->exposure,
					 ov16a1q->exposure->minimum, max,
					 ov16a1q->exposure->step,
					 min(ov16a1q->exposure->default_value,
					     max));
	}

	if (!pm_runtime_get_if_in_use(ov16a1q->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(ov16a1q->regmap, OV16A1Q_REG_ANALOG_GAIN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(ov16a1q->regmap, OV16A1Q_REG_DIGITAL_GAIN,
			  ctrl->val << OV16A1Q_DGTL_GAIN_SHIFT, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(ov16a1q->regmap, OV16A1Q_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(ov16a1q->regmap, OV16A1Q_REG_VTS,
			  mode->height + ctrl->val, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(ov16a1q->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov16a1q_ctrl_ops = {
	.s_ctrl = ov16a1q_set_ctrl,
};

static int ov16a1q_init_controls(struct ov16a1q *ov16a1q)
{
	struct v4l2_ctrl_handler *hdl = &ov16a1q->ctrl_handler;
	const struct ov16a1q_mode *mode = ov16a1q->cur_mode;
	struct v4l2_fwnode_device_properties props;
	s64 vblank_def, exposure_max, hblank;
	int ret;

	v4l2_ctrl_handler_init(hdl, 10);

	ov16a1q->link_freq = v4l2_ctrl_new_int_menu(hdl, &ov16a1q_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(ov16a1q_link_freqs) - 1,
						   0, ov16a1q_link_freqs);
	if (ov16a1q->link_freq)
		ov16a1q->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ov16a1q->pixel_rate = v4l2_ctrl_new_std(hdl, &ov16a1q_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       OV16A1Q_PIXEL_RATE,
					       OV16A1Q_PIXEL_RATE, 1,
					       OV16A1Q_PIXEL_RATE);

	vblank_def = mode->fll_def - mode->height;
	ov16a1q->vblank = v4l2_ctrl_new_std(hdl, &ov16a1q_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_def,
					   OV16A1Q_VTS_MAX - mode->height, 1,
					   vblank_def);

	hblank = mode->llp - mode->width;
	ov16a1q->hblank = v4l2_ctrl_new_std(hdl, &ov16a1q_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (ov16a1q->hblank)
		ov16a1q->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	exposure_max = mode->fll_def - OV16A1Q_EXPOSURE_MARGIN;
	ov16a1q->exposure = v4l2_ctrl_new_std(hdl, &ov16a1q_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     OV16A1Q_EXPOSURE_MIN, exposure_max,
					     OV16A1Q_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std(hdl, &ov16a1q_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  OV16A1Q_ANA_GAIN_MIN, OV16A1Q_ANA_GAIN_MAX,
			  OV16A1Q_ANA_GAIN_STEP, OV16A1Q_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(hdl, &ov16a1q_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  OV16A1Q_DGTL_GAIN_MIN, OV16A1Q_DGTL_GAIN_MAX,
			  OV16A1Q_DGTL_GAIN_STEP, OV16A1Q_DGTL_GAIN_DEFAULT);

	if (hdl->error) {
		ret = hdl->error;
		goto err_free;
	}

	ret = v4l2_fwnode_device_parse(ov16a1q->dev, &props);
	if (ret)
		goto err_free;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &ov16a1q_ctrl_ops, &props);
	if (ret)
		goto err_free;

	ov16a1q->sd.ctrl_handler = hdl;

	return 0;

err_free:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static void ov16a1q_update_pad_format(struct ov16a1q *ov16a1q,
				     const struct ov16a1q_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = ov16a1q_get_format_code(ov16a1q);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int ov16a1q_start_streaming(struct ov16a1q *ov16a1q)
{
	const struct ov16a1q_mode *mode = ov16a1q->cur_mode;
	int ret;

	ret = pm_runtime_resume_and_get(ov16a1q->dev);
	if (ret < 0)
		return ret;

	ret = cci_write(ov16a1q->regmap, OV16A1Q_REG_SOFTWARE_RST, 0x01, NULL);
	if (ret) {
		dev_err(ov16a1q->dev, "failed to reset: %d\n", ret);
		goto err_rpm_put;
	}
	usleep_range(5000, 5500);

	ret = cci_multi_reg_write(ov16a1q->regmap, ov16a1q_init_regs,
				  ARRAY_SIZE(ov16a1q_init_regs), NULL);
	if (ret) {
		dev_err(ov16a1q->dev, "failed to write init settings: %d\n",
			ret);
		goto err_rpm_put;
	}

	ret = cci_multi_reg_write(ov16a1q->regmap, mode->regs, mode->num_regs,
				  NULL);
	if (ret) {
		dev_err(ov16a1q->dev, "failed to set mode: %d\n", ret);
		goto err_rpm_put;
	}

	ret = __v4l2_ctrl_handler_setup(ov16a1q->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = cci_write(ov16a1q->regmap, OV16A1Q_REG_MODE_SELECT,
			OV16A1Q_MODE_STREAMING, NULL);
	if (ret)
		goto err_rpm_put;

	return 0;

err_rpm_put:
	pm_runtime_put(ov16a1q->dev);
	return ret;
}

static int ov16a1q_stop_streaming(struct ov16a1q *ov16a1q)
{
	int ret;

	ret = cci_write(ov16a1q->regmap, OV16A1Q_REG_MODE_SELECT,
			OV16A1Q_MODE_STANDBY, NULL);
	if (ret)
		dev_err(ov16a1q->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put(ov16a1q->dev);

	return ret;
}

static int ov16a1q_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov16a1q *ov16a1q = to_ov16a1q(sd);
	struct v4l2_subdev_state *state;
	int ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (enable)
		ret = ov16a1q_start_streaming(ov16a1q);
	else
		ret = ov16a1q_stop_streaming(ov16a1q);

	v4l2_subdev_unlock_state(state);

	return ret;
}

static int ov16a1q_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct ov16a1q *ov16a1q = to_ov16a1q(sd);

	if (code->index > 0)
		return -EINVAL;

	code->code = ov16a1q_get_format_code(ov16a1q);

	return 0;
}

static int ov16a1q_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct ov16a1q *ov16a1q = to_ov16a1q(sd);

	if (fse->index >= ARRAY_SIZE(ov16a1q_modes))
		return -EINVAL;

	if (fse->code != ov16a1q_get_format_code(ov16a1q))
		return -EINVAL;

	fse->min_width = ov16a1q_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = ov16a1q_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int ov16a1q_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct ov16a1q *ov16a1q = to_ov16a1q(sd);
	const struct ov16a1q_mode *mode;
	s64 hblank, vblank_def, exposure_max;

	mode = v4l2_find_nearest_size(ov16a1q_modes, ARRAY_SIZE(ov16a1q_modes),
				      width, height, fmt->format.width,
				      fmt->format.height);

	ov16a1q_update_pad_format(ov16a1q, mode, &fmt->format);
	*v4l2_subdev_state_get_format(sd_state, 0) = fmt->format;
	*v4l2_subdev_state_get_crop(sd_state, 0) = mode->crop;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	ov16a1q->cur_mode = mode;

	hblank = mode->llp - mode->width;
	__v4l2_ctrl_modify_range(ov16a1q->hblank, hblank, hblank, 1, hblank);

	vblank_def = mode->fll_def - mode->height;
	__v4l2_ctrl_modify_range(ov16a1q->vblank, vblank_def,
				 OV16A1Q_VTS_MAX - mode->height, 1,
				 vblank_def);
	__v4l2_ctrl_s_ctrl(ov16a1q->vblank, vblank_def);

	exposure_max = mode->fll_def - OV16A1Q_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(ov16a1q->exposure, OV16A1Q_EXPOSURE_MIN,
				 exposure_max, OV16A1Q_EXPOSURE_STEP,
				 exposure_max);

	return 0;
}

static int ov16a1q_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = OV16A1Q_NATIVE_WIDTH;
		sel->r.height = OV16A1Q_NATIVE_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = ov16a1q_modes[0].crop;
		return 0;
	}

	return -EINVAL;
}

static int ov16a1q_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = ov16a1q_modes[0].width,
			.height = ov16a1q_modes[0].height,
		},
	};

	return ov16a1q_set_pad_format(sd, sd_state, &fmt);
}

static const struct v4l2_subdev_video_ops ov16a1q_video_ops = {
	.s_stream = ov16a1q_set_stream,
};

static const struct v4l2_subdev_pad_ops ov16a1q_pad_ops = {
	.enum_mbus_code = ov16a1q_enum_mbus_code,
	.enum_frame_size = ov16a1q_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ov16a1q_set_pad_format,
	.get_selection = ov16a1q_get_selection,
};

static const struct v4l2_subdev_core_ops ov16a1q_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops ov16a1q_subdev_ops = {
	.core = &ov16a1q_core_ops,
	.video = &ov16a1q_video_ops,
	.pad = &ov16a1q_pad_ops,
};

static const struct v4l2_subdev_internal_ops ov16a1q_internal_ops = {
	.init_state = ov16a1q_init_state,
};

static int ov16a1q_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov16a1q *ov16a1q = to_ov16a1q(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ov16a1q->supplies),
				    ov16a1q->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}
	usleep_range(1000, 1100);

	ret = clk_prepare_enable(ov16a1q->mclk);
	if (ret) {
		dev_err(dev, "failed to enable mclk: %d\n", ret);
		goto err_disable_regulators;
	}
	usleep_range(1000, 1100);

	gpiod_set_value_cansleep(ov16a1q->reset_gpio, 0);
	usleep_range(10000, 11000);

	return 0;

err_disable_regulators:
	regulator_bulk_disable(ARRAY_SIZE(ov16a1q->supplies), ov16a1q->supplies);
	return ret;
}

static int ov16a1q_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov16a1q *ov16a1q = to_ov16a1q(sd);

	gpiod_set_value_cansleep(ov16a1q->reset_gpio, 1);
	clk_disable_unprepare(ov16a1q->mclk);
	regulator_bulk_disable(ARRAY_SIZE(ov16a1q->supplies), ov16a1q->supplies);

	return 0;
}

static int ov16a1q_identify(struct ov16a1q *ov16a1q)
{
	u64 id;
	int ret;

	ret = cci_read(ov16a1q->regmap, OV16A1Q_REG_CHIP_ID, &id, NULL);
	if (ret)
		return dev_err_probe(ov16a1q->dev, ret,
				     "failed to read chip id\n");

	if (id != OV16A1Q_CHIP_ID)
		return dev_err_probe(ov16a1q->dev, -ENODEV,
				     "unexpected chip id 0x%06llx\n", id);

	return 0;
}

static int ov16a1q_check_hwcfg(struct ov16a1q *ov16a1q)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	unsigned long freq_bitmap;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(ov16a1q->dev), NULL);
	if (!ep)
		return dev_err_probe(ov16a1q->dev, -ENXIO,
				     "no endpoint found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(ov16a1q->dev, ret,
				     "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != OV16A1Q_NUM_LANES) {
		ret = dev_err_probe(ov16a1q->dev, -EINVAL,
				    "only %u data lanes are supported\n",
				    OV16A1Q_NUM_LANES);
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(ov16a1q->dev,
				       bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       ov16a1q_link_freqs,
				       ARRAY_SIZE(ov16a1q_link_freqs),
				       &freq_bitmap);
out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int ov16a1q_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov16a1q *ov16a1q;
	unsigned int i;
	int ret;

	ov16a1q = devm_kzalloc(dev, sizeof(*ov16a1q), GFP_KERNEL);
	if (!ov16a1q)
		return -ENOMEM;

	ov16a1q->dev = dev;

	ret = ov16a1q_check_hwcfg(ov16a1q);
	if (ret)
		return ret;

	ov16a1q->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ov16a1q->regmap))
		return dev_err_probe(dev, PTR_ERR(ov16a1q->regmap),
				     "failed to init CCI\n");

	ov16a1q->mclk = devm_clk_get(dev, NULL);
	if (IS_ERR(ov16a1q->mclk))
		return dev_err_probe(dev, PTR_ERR(ov16a1q->mclk),
				     "failed to get mclk\n");

	ret = clk_set_rate(ov16a1q->mclk, OV16A1Q_MCLK_FREQ);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set mclk rate\n");

	ov16a1q->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ov16a1q->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ov16a1q->reset_gpio),
				     "failed to get reset gpio\n");

	for (i = 0; i < ARRAY_SIZE(ov16a1q_supply_names); i++)
		ov16a1q->supplies[i].supply = ov16a1q_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ov16a1q->supplies),
				      ov16a1q->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	v4l2_i2c_subdev_init(&ov16a1q->sd, client, &ov16a1q_subdev_ops);

	ret = ov16a1q_power_on(dev);
	if (ret)
		return ret;

	ret = ov16a1q_identify(ov16a1q);
	if (ret)
		goto err_power_off;

	ov16a1q->cur_mode = &ov16a1q_modes[0];

	ret = ov16a1q_init_controls(ov16a1q);
	if (ret)
		goto err_power_off;

	ov16a1q->sd.internal_ops = &ov16a1q_internal_ops;
	ov16a1q->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	ov16a1q->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ov16a1q->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&ov16a1q->sd.entity, 1, &ov16a1q->pad);
	if (ret)
		goto err_free_ctrls;

	ov16a1q->sd.state_lock = ov16a1q->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&ov16a1q->sd);
	if (ret)
		goto err_entity_cleanup;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&ov16a1q->sd);
	if (ret)
		goto err_rpm_disable;

	pm_runtime_idle(dev);

	return 0;

err_rpm_disable:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&ov16a1q->sd);
err_entity_cleanup:
	media_entity_cleanup(&ov16a1q->sd.entity);
err_free_ctrls:
	v4l2_ctrl_handler_free(&ov16a1q->ctrl_handler);
err_power_off:
	ov16a1q_power_off(dev);
	return ret;
}

static void ov16a1q_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov16a1q *ov16a1q = to_ov16a1q(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&ov16a1q->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		ov16a1q_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(ov16a1q_pm_ops, ov16a1q_power_off,
				 ov16a1q_power_on, NULL);

static const struct of_device_id ov16a1q_of_match[] = {
	{ .compatible = "ovti,ov16a1q" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov16a1q_of_match);

static struct i2c_driver ov16a1q_i2c_driver = {
	.driver = {
		.name = "ov16a1q",
		.of_match_table = ov16a1q_of_match,
		.pm = pm_ptr(&ov16a1q_pm_ops),
	},
	.probe = ov16a1q_probe,
	.remove = ov16a1q_remove,
};
module_i2c_driver(ov16a1q_i2c_driver);

MODULE_DESCRIPTION("OmniVision OV16A1Q image sensor driver");
MODULE_LICENSE("GPL");
