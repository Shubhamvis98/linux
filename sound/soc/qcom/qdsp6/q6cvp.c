// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/qcom/apr.h>
#include "q6cvp.h"
#include "q6voice-common.h"

#define VSS_IVOCPROC_DIRECTION_RX	0
#define VSS_IVOCPROC_DIRECTION_TX	1
#define VSS_IVOCPROC_DIRECTION_RX_TX	2

#define VSS_IVOCPROC_PORT_ID_NONE	0xFFFF

#define VSS_IVOCPROC_TOPOLOGY_ID_NONE			0x00010F70
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS		0x00010F71
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_DM_FLUENCE		0x00010F72

#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT		0x00010F77

#define VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING		0x00010F7C
#define VSS_IVOCPROC_VOCPROC_MODE_EC_EXT_MIXING		0x00010F7D

#define VSS_ICOMMON_CAL_NETWORK_ID_NONE			0x0001135E

#define VSS_IVOCPROC_CMD_ENABLE				0x000100C6
#define VSS_IVOCPROC_CMD_DISABLE			0x000110E1

#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2	0x000112BF

struct vss_ivocproc_cmd_create_full_control_session_v2_cmd {
	struct apr_hdr hdr;

	/*
	 * Vocproc direction. The supported values:
	 * VSS_IVOCPROC_DIRECTION_RX
	 * VSS_IVOCPROC_DIRECTION_TX
	 * VSS_IVOCPROC_DIRECTION_RX_TX
	 */
	u16 direction;

	/*
	 * Tx device port ID to which the vocproc connects. If a port ID is
	 * not being supplied, set this to #VSS_IVOCPROC_PORT_ID_NONE.
	 */
	u16 tx_port_id;

	/*
	 * Tx path topology ID. If a topology ID is not being supplied, set
	 * this to #VSS_IVOCPROC_TOPOLOGY_ID_NONE.
	 */
	u32 tx_topology_id;

	/*
	 * Rx device port ID to which the vocproc connects. If a port ID is
	 * not being supplied, set this to #VSS_IVOCPROC_PORT_ID_NONE.
	 */
	u16 rx_port_id;

	/*
	 * Rx path topology ID. If a topology ID is not being supplied, set
	 * this to #VSS_IVOCPROC_TOPOLOGY_ID_NONE.
	 */
	u32 rx_topology_id;

	/* Voice calibration profile ID. */
	u32 profile_id;

	/*
	 * Vocproc mode. The supported values:
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_EXT_MIXING
	 */
	u32 vocproc_mode;

	/*
	 * Port ID to which the vocproc connects for receiving echo
	 * cancellation reference signal. If a port ID is not being supplied,
	 * set this to #VSS_IVOCPROC_PORT_ID_NONE. This parameter value is
	 * ignored when the vocproc_mode parameter is set to
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING.
	 */
	u16 ec_ref_port_id;

	/*
	 * Session name string used to identify a session that can be shared
	 * with passive controllers (optional).
	 */
	char name[20];
} __packed;

struct q6voice_session *q6cvp_session_create(enum q6voice_path_type path,
					     u16 tx_port, u16 rx_port)
{
	struct vss_ivocproc_cmd_create_full_control_session_v2_cmd cmd = {};

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2;

	/* TODO: Implement calibration */
	cmd.tx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS;
	cmd.rx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT;

	cmd.direction = VSS_IVOCPROC_DIRECTION_RX_TX;
	cmd.tx_port_id = tx_port;
	cmd.rx_port_id = rx_port;
	cmd.profile_id = VSS_ICOMMON_CAL_NETWORK_ID_NONE;
	cmd.vocproc_mode = VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING;
	cmd.ec_ref_port_id = VSS_IVOCPROC_PORT_ID_NONE;

	return q6voice_session_create(Q6VOICE_SERVICE_CVP, path, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_session_create);

/*
 * CVD 2.2+ (e.g. SM8350): the vocproc is created without a committed
 * topology. The device channels and endpoint media formats have to be set
 * before VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT, otherwise the vocproc assumes a
 * mono 8 kHz device.
 */
#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3	0x00013169
#define VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT		0x00013198
#define VSS_ICOMMON_CMD_SET_PARAM_V2			0x0001133D

#define VSS_MODULE_CVD_GENERIC				0x0001316E
#define VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO		0x00013253
#define VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO		0x00013254
#define VSS_PARAM_VOCPROC_TX_CHANNEL_INFO		0x0001328E
#define VSS_PARAM_VOCPROC_RX_CHANNEL_INFO		0x0001328F
#define VSS_PARAM_VOCPROC_EC_REF_CHANNEL_INFO		0x00013290

#define AUDPROC_MODULE_ID_MFC				0x00010912
#define AUDPROC_PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT	0x00010913

#define VSS_NUM_CHANNELS_MAX				32
#define PCM_CHANNEL_FL					1
#define PCM_CHANNEL_FR					2
#define PCM_CHANNEL_FC					3

struct vss_param_vocproc_dev_channel_info {
	u32 num_channels;
	u32 bits_per_sample;
	u8 channel_mapping[VSS_NUM_CHANNELS_MAX];
} __packed;

struct vss_param_endpoint_media_format_info {
	u32 port_id;
	u16 num_channels;
	u16 bits_per_sample;
	u32 sample_rate;
	u8 channel_mapping[VSS_NUM_CHANNELS_MAX];
} __packed;

struct vss_param_mfc_config_info {
	u32 sample_rate;
	u16 bits_per_sample;
	u16 num_channels;
	u16 channel_type[VSS_NUM_CHANNELS_MAX];
} __packed;

/* In-band VSS_ICOMMON_CMD_SET_PARAM_V2 with a single parameter */
struct vss_icommon_cmd_set_param_v2 {
	struct apr_hdr hdr;
	u32 mem_handle;
	u64 mem_address;
	u32 mem_size;
	/* param_hdr_v1 */
	u32 module_id;
	u32 param_id;
	u16 param_size;
	u16 reserved;
	union {
		struct vss_param_vocproc_dev_channel_info channel_info;
		struct vss_param_endpoint_media_format_info media_fmt;
		struct vss_param_mfc_config_info mfc;
	} data;
} __packed;

static void q6cvp_channel_map(u8 *map, unsigned int channels)
{
	if (channels == 1) {
		map[0] = PCM_CHANNEL_FC;
	} else {
		map[0] = PCM_CHANNEL_FL;
		map[1] = PCM_CHANNEL_FR;
	}
}

static int q6cvp_set_param(struct q6voice_session *cvp, u32 module_id,
			   u32 param_id, const void *data, u16 size)
{
	struct vss_icommon_cmd_set_param_v2 cmd = {};
	u32 param_bytes = offsetof(struct vss_icommon_cmd_set_param_v2, data) -
			  offsetof(struct vss_icommon_cmd_set_param_v2, module_id) +
			  size;
	int ret;

	cmd.hdr.pkt_size = offsetof(struct vss_icommon_cmd_set_param_v2, data) + size;
	cmd.hdr.opcode = VSS_ICOMMON_CMD_SET_PARAM_V2;
	cmd.mem_size = param_bytes;
	cmd.module_id = module_id;
	cmd.param_id = param_id;
	cmd.param_size = size;
	memcpy(&cmd.data, data, size);

	ret = q6voice_common_send(cvp, &cmd.hdr);
	if (ret)
		dev_err(cvp->dev, "set param %#x/%#x failed: %d\n",
			module_id, param_id, ret);
	return ret;
}

static int q6cvp_set_channel_info(struct q6voice_session *cvp, u32 param_id,
				  const struct q6cvp_dev_fmt *fmt)
{
	struct vss_param_vocproc_dev_channel_info info = {};

	info.num_channels = fmt->channels;
	info.bits_per_sample = fmt->bits;
	q6cvp_channel_map(info.channel_mapping, fmt->channels);

	return q6cvp_set_param(cvp, VSS_MODULE_CVD_GENERIC, param_id,
			       &info, sizeof(info));
}

static int q6cvp_set_media_fmt(struct q6voice_session *cvp, u32 param_id,
			       const struct q6cvp_dev_fmt *fmt)
{
	struct vss_param_endpoint_media_format_info info = {};

	info.port_id = fmt->port_id;
	info.num_channels = fmt->channels;
	info.bits_per_sample = fmt->bits;
	info.sample_rate = fmt->rate;
	q6cvp_channel_map(info.channel_mapping, fmt->channels);

	return q6cvp_set_param(cvp, VSS_MODULE_CVD_GENERIC, param_id,
			       &info, sizeof(info));
}

/* Upmix the mono voice stream to all RX device channels */
static int q6cvp_set_mfc(struct q6voice_session *cvp,
			 const struct q6cvp_dev_fmt *rx)
{
	struct vss_param_mfc_config_info mfc = {};
	u8 map[VSS_NUM_CHANNELS_MAX] = {};
	int i;

	mfc.sample_rate = rx->rate;
	mfc.bits_per_sample = 16;
	mfc.num_channels = rx->channels;
	q6cvp_channel_map(map, rx->channels);
	for (i = 0; i < rx->channels; i++)
		mfc.channel_type[i] = map[i];

	return q6cvp_set_param(cvp, AUDPROC_MODULE_ID_MFC,
			       AUDPROC_PARAM_ID_MFC_OUTPUT_MEDIA_FORMAT,
			       &mfc, sizeof(mfc));
}

struct q6voice_session *q6cvp_session_create_v3(enum q6voice_path_type path,
						const struct q6cvp_dev_fmt *tx,
						const struct q6cvp_dev_fmt *rx)
{
	struct vss_ivocproc_cmd_create_full_control_session_v2_cmd cmd = {};
	struct q6voice_session *cvp;
	struct apr_pkt commit;
	int ret;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3;

	cmd.tx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS;
	cmd.rx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT;
	cmd.direction = VSS_IVOCPROC_DIRECTION_RX_TX;
	cmd.tx_port_id = tx->port_id;
	cmd.rx_port_id = rx->port_id;
	cmd.profile_id = VSS_ICOMMON_CAL_NETWORK_ID_NONE;
	cmd.vocproc_mode = VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING;
	cmd.ec_ref_port_id = VSS_IVOCPROC_PORT_ID_NONE;

	cvp = q6voice_session_create(Q6VOICE_SERVICE_CVP, path, &cmd.hdr);
	if (IS_ERR(cvp))
		return cvp;

	ret = q6cvp_set_channel_info(cvp, VSS_PARAM_VOCPROC_RX_CHANNEL_INFO, rx);
	if (!ret)
		ret = q6cvp_set_channel_info(cvp, VSS_PARAM_VOCPROC_TX_CHANNEL_INFO, tx);
	if (!ret)
		ret = q6cvp_set_channel_info(cvp, VSS_PARAM_VOCPROC_EC_REF_CHANNEL_INFO, rx);
	if (!ret)
		ret = q6cvp_set_media_fmt(cvp, VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO, rx);
	if (!ret)
		ret = q6cvp_set_media_fmt(cvp, VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO, tx);
	if (ret)
		goto err;

	commit.hdr.pkt_size = APR_HDR_SIZE;
	commit.hdr.opcode = VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT;
	ret = q6voice_common_send(cvp, &commit.hdr);
	if (ret) {
		dev_err(cvp->dev, "topology commit failed: %d\n", ret);
		goto err;
	}

	if (rx->channels > 1) {
		ret = q6cvp_set_mfc(cvp, rx);
		if (ret)
			goto err;
	}

	return cvp;

err:
	q6voice_session_release(cvp);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(q6cvp_session_create_v3);

int q6cvp_enable(struct q6voice_session *cvp, bool state)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = state ? VSS_IVOCPROC_CMD_ENABLE : VSS_IVOCPROC_CMD_DISABLE;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_enable);

static int q6cvp_probe(struct apr_device *adev)
{
	return q6voice_common_probe(adev, Q6VOICE_SERVICE_CVP);
}

static const struct of_device_id q6cvp_device_id[]  = {
	{ .compatible = "qcom,q6cvp" },
	{},
};
MODULE_DEVICE_TABLE(of, q6cvp_device_id);

static struct apr_driver qcom_q6cvp_driver = {
	.probe = q6cvp_probe,
	.remove = q6voice_common_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6cvp",
		.of_match_table = of_match_ptr(q6cvp_device_id),
	},
};

module_apr_driver(qcom_q6cvp_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6 Core Voice Processor");
MODULE_LICENSE("GPL v2");
